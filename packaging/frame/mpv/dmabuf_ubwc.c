/* Q10C carrier images, TP10 descriptor rewriting, band splitting and reference
 * layout checks adapted from FramePlayerX, Copyright 2026 remotesojourner.
 * Upstream: crates/fp-render/src/q10c.rs at
 * 0358020e13bc3185fe66a3ebf88d5ad9c50f24f6, Apache-2.0.
 * https://github.com/remotesojourner/frameplayerx
 * Modified for VideoFrameVR's C/mpv DMA-BUF import and conversion pipeline;
 * present in VideoFrameVR by 2026-11-03, attribution added 2026-11-10.
 * See packaging/frameplayerx/ for the upstream license, notices and scope.
 */

/* Private mpv integration for the native Steam Frame bundle.
 * Frame's decoder exposes UBWC NV12/P010. Import the whole compressed image
 * through Vulkan, then transfer its planes into an exported linear buffer.
 * Each decoder buffer is imported once and reused while the decoder keeps it.
 * Input is complete at V4L2 DQBUF. A native fence orders GL after Vulkan;
 * a GL fence protects output reuse and retained decoder input references.
 * Without GL (export mode, frame_ubwc_export) the caller gets the buffer and
 * the conversion's sync_file instead, and holds the buffer until it unholds it.
 * The decoder thread calls frame_ubwc_decoder_closed when it closes a V4L2
 * session, so the next one never competes with buffers kept here.
 *
 * Q10C (10-bit) is TP10: three 10-bit samples per 32-bit word. Turnip has no
 * TP10 images, but the GPU reads TP10 itself (FMT6_TP10_Y/UV), and Turnip
 * lays out a compressed 32-bit image byte for byte as Iris lays out a TP10
 * plane (a 48x4 TP10 block is 64x4 bytes, as is a 16x4 32-bit block). So
 * each plane is bound as such a "carrier" image, its texture descriptor is
 * fetched through VK_EXT_descriptor_buffer and turned into a TP10 one, and a
 * compute pass writes P010.
 * frame_ubwc_create checks every Turnip assumption on a reference picture.
 */
#include <vulkan/vulkan.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "common/msg.h"
#include "video/mp_image.h"
#include "dmabuf_ubwc.h"
#include "dmabuf_ubwc.spv.h"
#include "video_export.h"

// A decoder buffer imported once and reused while the decoder keeps it: the
// decoder's own fd number still names the same DMA-BUF (inode).
struct input_entry {
    VkImage image;  // TP10: the Y plane's carrier
    VkImage uv_image;
    VkImageView views[2];
    VkDeviceMemory memory;
    int fd;  // the decoder's, not owned
    dev_t dev;
    ino_t ino;
    VkSubresourceLayout layouts[2];
    size_t size;
    int users;  // output slots whose transfer may still read it
    uint64_t last_use;
};

#define MAX_INPUTS 24

// TP10 carriers: four bytes, three samples.
#define CARRIER VK_FORMAT_A2B10G10R10_UNORM_PACK32
// TP10 texture reads past 2^24 samples need separate descriptors. Read planes
// in bands of whole 64-row metadata tiles, each with its own descriptor.
#define MAX_BAND_SAMPLES (1u << 24)
#define MAX_BANDS 24

struct band {
    unsigned plane, row0, rows;
};

struct output_slot {
    VkBuffer buffer;
    VkDeviceAddress address;
    VkDeviceMemory memory;
    VkCommandBuffer cmd;
    VkFence ready;
    struct input_entry *input;
    struct mp_image *source;
    int input_done_fd;
    GLsync fence;
    bool exported;
    bool held;  // export mode: the caller still uses the buffer
    uint64_t id;
    AVDRMFrameDescriptor desc;
};

#define GL_SLOTS 3
#define MAX_SLOTS 6

struct frame_ubwc {
    pthread_mutex_t lock;  // the render thread's calls against frame_ubwc_decoder_closed
    struct frame_ubwc *next_instance;
    GL *gl;
    struct mp_log *log;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    PFN_vkGetMemoryFdPropertiesKHR fd_properties;
    PFN_vkGetMemoryFdKHR export_fd;
    PFN_vkGetFenceFdKHR export_fence;
    EGLDisplay display;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    bool async;
    struct output_slot slots[MAX_SLOTS];
    unsigned nslots;
    unsigned next;
    int active;
    unsigned w, h, pitch;
    unsigned bytes_per_sample;
    uint32_t drm_format;
    VkFormat vk_format;
    VkDeviceSize uv_offset, size;
    struct input_entry inputs[MAX_INPUTS];
    uint64_t frame;
    bool tp10;
    PFN_vkGetDescriptorEXT get_descriptor;
    PFN_vkCmdBindDescriptorBuffersEXT bind_descriptors;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT set_descriptor_offsets;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipelines[2];  // luma, chroma
    // Each input's band descriptors, nbands sets of set_size bytes.
    VkBuffer descriptors;
    VkDeviceMemory descriptor_memory;
    uint8_t *descriptor_map;
    VkDeviceAddress descriptor_address;
    VkDeviceSize set_size, binding_offset;
    struct band bands[MAX_BANDS];
    unsigned nbands;
};

#define VK_CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    mp_err(p->log, "UBWC GPU transfer failed at line %d: %d\n", __LINE__, r_); \
    goto fail; } } while (0)

// Every instance, for frame_ubwc_decoder_closed.
static pthread_mutex_t instances_lock = PTHREAD_MUTEX_INITIALIZER;
static struct frame_ubwc *instances;
static struct video_export *watched;
static int watchers;  // decoder threads waiting on watched
static pthread_cond_t watchers_done = PTHREAD_COND_INITIALIZER;

static uint32_t memory_type(struct frame_ubwc *p, uint32_t mask, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(p->physical, &props);
    for (unsigned i = 0; i < props.memoryTypeCount; i++)
        if ((mask & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

static bool wait_slot(struct frame_ubwc *p, struct output_slot *slot)
{
    if (!p->gl) {
        // The caller unheld it, so only the conversion itself may still run.
        if (slot->input_done_fd >= 0) {
            struct pollfd done = {.fd = slot->input_done_fd, .events = POLLIN};
            if (poll(&done, 1, 1000) <= 0) {
                mp_err(p->log, "UBWC conversion did not finish\n");
                return false;
            }
        }
        return true;
    }
    if (!slot->fence)
        return true;
    GLenum status = p->gl->ClientWaitSync(slot->fence,
                                         GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        mp_err(p->log, "UBWC output still in use by GL: 0x%x\n", status);
        return false;
    }
    p->gl->DeleteSync(slot->fence);
    slot->fence = NULL;
    return true;
}

static void destroy_input(struct frame_ubwc *p, struct input_entry *e)
{
    for (unsigned i = 0; i < 2; i++)
        if (e->views[i])
            vkDestroyImageView(p->device, e->views[i], NULL);
    if (e->image)
        vkDestroyImage(p->device, e->image, NULL);
    if (e->uv_image)
        vkDestroyImage(p->device, e->uv_image, NULL);
    if (e->memory)
        vkFreeMemory(p->device, e->memory, NULL);
    *e = (struct input_entry){.fd = -1};
}

static unsigned make_bands(struct band *bands, unsigned w, unsigned h)
{
    // Both planes have w samples a row (the UV plane w/2 pairs).
    unsigned step = MAX_BAND_SAMPLES / w / 64 * 64, n = 0;
    if (!step)
        step = 64;
    for (unsigned plane = 0; plane < 2; plane++) {
        unsigned rows = plane ? h / 2 : h;
        for (unsigned row0 = 0; row0 < rows && n < MAX_BANDS; row0 += step)
            bands[n++] = (struct band){plane, row0, rows - row0 < step ? rows - row0 : step};
    }
    return n;
}

// a6xx/a7xx texture descriptor fields (Mesa's A6XX_TEX_MEMOBJ).
static uint32_t tex_get(const uint32_t *d, unsigned word, unsigned shift, unsigned bits)
{
    return (d[word] >> shift) & ((1u << bits) - 1);
}

static void tex_set(uint32_t *d, unsigned word, unsigned shift, unsigned bits, uint32_t v)
{
    uint32_t mask = ((1u << bits) - 1) << shift;
    d[word] = (d[word] & ~mask) | ((v << shift) & mask);
}

// Moves an address (a low word, then 17 bits of the next) on by add.
static void tex_offset(uint32_t *d, unsigned word, uint64_t add)
{
    uint64_t a = (d[word] | (uint64_t)(d[word + 1] & 0x1ffff) << 32) + add;
    d[word] = (uint32_t)a;
    d[word + 1] = (d[word + 1] & ~0x1ffffu) | ((uint32_t)(a >> 32) & 0x1ffff);
}

// Turns the descriptor d of a plane's carrier (texels x rows) into the TP10
// descriptor of band b of a picture w samples wide. False when the carrier's
// descriptor is not what this expects.
static bool tp10_band(uint32_t d[16], unsigned texels, unsigned rows,
                      const struct band *b, unsigned w, unsigned pitch)
{
    if (tex_get(d, 0, 22, 8) != 0x36 /* FMT6_10_10_10_2_UNORM */ ||
        tex_get(d, 0, 0, 2) != 3 /* TILE6_3 */ || tex_get(d, 1, 0, 15) != texels ||
        tex_get(d, 1, 15, 15) != rows || !tex_get(d, 3, 28, 1) /* FLAG */ ||
        !tex_get(d, 10, 0, 7) /* FLAG_BUFFER_PITCH */ || b->row0 % 64)
        return false;
    tex_set(d, 0, 22, 8, b->plane ? 0x9f : 0x9e);  // FMT6_TP10_UV, FMT6_TP10_Y
    tex_set(d, 1, 0, 15, b->plane ? w / 2 : w);
    tex_set(d, 1, 15, 15, b->rows);
    unsigned logh = 0;  // FLAG_BUFFER_LOGH: the band's 4-row blocks, rounded up
    while ((1u << logh) < (b->rows + 3) / 4)
        logh++;
    tex_set(d, 10, 12, 4, logh);
    tex_offset(d, 7, (uint64_t)b->row0 / 4 * tex_get(d, 10, 0, 7) * 64);  // flag buffer
    tex_offset(d, 4, (uint64_t)b->row0 * pitch);
    return true;
}

// Binds a Q10C picture, w x h in size bytes of the DMA-BUF fd (new memory
// if fd < 0) with its planes at layouts, as carriers, and writes its bands'
// TP10 descriptors at out. False if Turnip lays the carriers out otherwise.
static bool import_carriers(struct frame_ubwc *p, struct input_entry *e, int fd, size_t size,
                            const VkSubresourceLayout *layouts, unsigned w, unsigned h,
                            const struct band *bands, unsigned nbands, uint8_t *out)
{
    int imported_fd = -1;
    VkImage *images[2] = {&e->image, &e->uv_image};
    VkMemoryRequirements req[2];
    unsigned texels = layouts[0].rowPitch / 4;
    // The decoder lays out its coded height, a few rows more than h, which
    // the descriptor doesn't give: the carrier that needs its Y plane size.
    unsigned rows = (h + 15) & ~15u;
    for (unsigned i = 0; i < 2;) {
        VkSubresourceLayout layout = {.rowPitch = layouts[i].rowPitch};
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED,
            .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &layout};
        VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &mod, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext,
            .imageType = VK_IMAGE_TYPE_2D, .format = CARRIER,
            .extent = {texels, i ? rows / 2 : rows, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
        VK_CHECK(vkCreateImage(p->device, &ici, NULL, images[i]));
        vkGetImageMemoryRequirements(p->device, *images[i], &req[i]);
        if (!i && req[0].size < layouts[1].offset && rows < h + 64) {
            vkDestroyImage(p->device, e->image, NULL);
            e->image = VK_NULL_HANDLE;
            rows += 16;
            continue;
        }
        i++;
    }
    if (layouts[0].offset || layouts[0].rowPitch != layouts[1].rowPitch ||
        req[0].size != layouts[1].offset || layouts[1].offset % req[1].alignment ||
        layouts[1].offset + req[1].size > size) {
        mp_err(p->log, "Q10C %ux%u: the GPU lays the planes out in %llu + %llu bytes, "
               "the decoder at %llu in %zu\n", w, h, (unsigned long long)req[0].size,
               (unsigned long long)req[1].size, (unsigned long long)layouts[1].offset, size);
        goto fail;
    }
    VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
                                     .memoryTypeBits = UINT32_MAX};
    if (fd >= 0) {
        VK_CHECK(p->fd_properties(p->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                  fd, &props));
        imported_fd = dup(fd);
        if (imported_fd < 0)
            goto fail;
    }
    VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = imported_fd};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = fd >= 0 ? &import : NULL, .allocationSize = size,
        .memoryTypeIndex = memory_type(p, req[0].memoryTypeBits & req[1].memoryTypeBits &
                                       props.memoryTypeBits, 0)};
    if (alloc.memoryTypeIndex == UINT32_MAX)
        goto fail;
    VK_CHECK(vkAllocateMemory(p->device, &alloc, NULL, &e->memory));
    imported_fd = -1;
    for (unsigned i = 0; i < 2; i++) {
        VK_CHECK(vkBindImageMemory(p->device, *images[i], e->memory, layouts[i].offset));
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = *images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = CARRIER,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(p->device, &vci, NULL, &e->views[i]));
    }
    for (unsigned i = 0; i < nbands; i++) {
        uint32_t d[16];
        VkDescriptorImageInfo image = {.imageView = e->views[bands[i].plane],
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorGetInfoEXT get = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
            .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .data.pSampledImage = &image};
        p->get_descriptor(p->device, &get, sizeof(d), d);
        if (!tp10_band(d, texels, bands[i].plane ? rows / 2 : rows, &bands[i], w, layouts[0].rowPitch)) {
            mp_err(p->log, "Unexpected texture descriptor for a compressed 32-bit image\n");
            goto fail;
        }
        memcpy(out + i * p->set_size + p->binding_offset, d, sizeof(d));
    }
    return true;
fail:
    if (imported_fd >= 0)
        close(imported_fd);
    return false;  // the caller destroys e
}

static void release_input(struct frame_ubwc *p, struct output_slot *slot)
{
    if (slot->input_done_fd >= 0)
        close(slot->input_done_fd);
    slot->input_done_fd = -1;
    if (slot->input)
        slot->input->users--;
    talloc_free(slot->source);
    slot->input = NULL;
    slot->source = NULL;
}

// Drops imports of buffers the decoder has let go of (its fd closed or
// reused), so a closed decoder's memory is not kept alive by the cache.
static void evict_inputs(struct frame_ubwc *p)
{
    for (unsigned i = 0; i < MAX_INPUTS; i++) {
        struct input_entry *e = &p->inputs[i];
        if (!e->image || e->users)
            continue;
        struct stat st;
        if (fstat(e->fd, &st) != 0 || st.st_dev != e->dev || st.st_ino != e->ino)
            destroy_input(p, e);
    }
}

static struct input_entry *find_input(struct frame_ubwc *p, const AVDRMFrameDescriptor *src,
                                      const VkSubresourceLayout *layouts)
{
    struct stat st;
    if (fstat(src->objects[0].fd, &st) != 0)
        return NULL;
    struct input_entry *free_entry = NULL, *oldest = NULL;
    for (unsigned i = 0; i < MAX_INPUTS; i++) {
        struct input_entry *e = &p->inputs[i];
        if (!e->image) {
            if (!free_entry)
                free_entry = e;
            continue;
        }
        if (e->dev == st.st_dev && e->ino == st.st_ino && e->fd == src->objects[0].fd &&
            e->size == src->objects[0].size &&
            !memcmp(e->layouts, layouts, sizeof(e->layouts)))
            return e;
        if (!e->users && (!oldest || e->last_use < oldest->last_use))
            oldest = e;
    }
    struct input_entry *e = free_entry ? free_entry : oldest;
    if (!e)
        return NULL;
    destroy_input(p, e);
    e->fd = src->objects[0].fd;
    e->dev = st.st_dev;
    e->ino = st.st_ino;
    e->size = src->objects[0].size;
    memcpy(e->layouts, layouts, sizeof(e->layouts));
    return e;  // without an image yet: the caller imports it
}

static bool create_slot(struct frame_ubwc *p, struct output_slot *slot)
{
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_CHECK(vkAllocateCommandBuffers(p->device, &cai, &slot->cmd));
    if (p->async) {
        if (!p->export_fence)
            goto fail;
        VkExportFenceCreateInfo export_fence = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                  .pNext = &export_fence};
        VK_CHECK(vkCreateFence(p->device, &fence, NULL, &slot->ready));
    }
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext,
        .size = p->size, .usage = p->tp10 ?
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT :
            VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VK_CHECK(vkCreateBuffer(p->device, &bci, NULL, &slot->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(p->device, slot->buffer, &req);
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = p->tp10 ? &flags : NULL, .buffer = slot->buffer};
    VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export,
        .allocationSize = req.size, .memoryTypeIndex = memory_type(p, req.memoryTypeBits, 0)};
    if (alloc.memoryTypeIndex == UINT32_MAX)
        goto fail;
    VK_CHECK(vkAllocateMemory(p->device, &alloc, NULL, &slot->memory));
    VK_CHECK(vkBindBufferMemory(p->device, slot->buffer, slot->memory, 0));
    if (p->tp10) {
        VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                             .buffer = slot->buffer};
        slot->address = vkGetBufferDeviceAddress(p->device, &address);
    }
    VkMemoryGetFdInfoKHR fd = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = slot->memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VK_CHECK(p->export_fd(p->device, &fd, &slot->desc.objects[0].fd));
    slot->desc.nb_objects = slot->desc.nb_layers = 1;
    slot->desc.objects[0].size = req.size;
    slot->desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
    slot->desc.layers[0] = (AVDRMLayerDescriptor){.format = p->drm_format,
        .nb_planes = 2, .planes = {{.object_index = 0, .pitch = p->pitch},
            {.object_index = 0, .offset = p->uv_offset, .pitch = p->pitch}}};
    return true;
fail:
    return false;  // frame_ubwc_destroy frees what was made
}

void frame_ubwc_destroy(struct frame_ubwc *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&instances_lock);
    for (struct frame_ubwc **it = &instances; *it; it = &(*it)->next_instance) {
        if (*it == p) {
            *it = p->next_instance;
            break;
        }
    }
    pthread_mutex_unlock(&instances_lock);
    if (p->active >= 0)
        p->gl->Finish();
    if (p->device)
        vkDeviceWaitIdle(p->device);
    for (unsigned i = 0; i < MAX_SLOTS; i++) {
        struct output_slot *slot = &p->slots[i];
        if (p->gl && !wait_slot(p, slot)) {
            p->gl->Finish();
            p->gl->DeleteSync(slot->fence);
        }
        release_input(p, slot);
    }
    for (unsigned i = 0; i < MAX_INPUTS; i++)
        destroy_input(p, &p->inputs[i]);
    for (unsigned i = 0; i < MAX_SLOTS; i++) {
        struct output_slot *slot = &p->slots[i];
        if (slot->cmd)
            vkFreeCommandBuffers(p->device, p->pool, 1, &slot->cmd);
        if (slot->ready)
            vkDestroyFence(p->device, slot->ready, NULL);
        if (slot->desc.objects[0].fd >= 0)
            close(slot->desc.objects[0].fd);
        if (slot->buffer)
            vkDestroyBuffer(p->device, slot->buffer, NULL);
        if (slot->memory)
            vkFreeMemory(p->device, slot->memory, NULL);
    }
    if (p->pool)
        vkDestroyCommandPool(p->device, p->pool, NULL);
    for (unsigned i = 0; i < 2; i++)
        if (p->pipelines[i])
            vkDestroyPipeline(p->device, p->pipelines[i], NULL);
    if (p->layout)
        vkDestroyPipelineLayout(p->device, p->layout, NULL);
    if (p->set_layout)
        vkDestroyDescriptorSetLayout(p->device, p->set_layout, NULL);
    if (p->descriptors)
        vkDestroyBuffer(p->device, p->descriptors, NULL);
    if (p->descriptor_memory)
        vkFreeMemory(p->device, p->descriptor_memory, NULL);
    if (p->device)
        vkDestroyDevice(p->device, NULL);
    if (p->instance)
        vkDestroyInstance(p->instance, NULL);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

// The TP10 pipelines and descriptor buffer, then the reference check: the
// 1920x1080 picture Iris writes (as measured on the Frame) must be laid out
// as Iris does and give the expected descriptors.
static bool create_tp10(struct frame_ubwc *p)
{
    struct input_entry check = {.fd = -1};
    VkShaderModule module = VK_NULL_HANDLE;
    p->get_descriptor = (void *)vkGetDeviceProcAddr(p->device, "vkGetDescriptorEXT");
    p->bind_descriptors = (void *)vkGetDeviceProcAddr(p->device, "vkCmdBindDescriptorBuffersEXT");
    p->set_descriptor_offsets = (void *)vkGetDeviceProcAddr(p->device,
                                                            "vkCmdSetDescriptorBufferOffsetsEXT");
    PFN_vkGetDescriptorSetLayoutSizeEXT layout_size =
        (void *)vkGetDeviceProcAddr(p->device, "vkGetDescriptorSetLayoutSizeEXT");
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT binding_offset =
        (void *)vkGetDeviceProcAddr(p->device, "vkGetDescriptorSetLayoutBindingOffsetEXT");
    if (!p->get_descriptor || !p->bind_descriptors || !p->set_descriptor_offsets ||
        !layout_size || !binding_offset)
        goto fail;
    VkPhysicalDeviceDescriptorBufferPropertiesEXT db = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &db};
    vkGetPhysicalDeviceProperties2(p->physical, &props);
    if (db.sampledImageDescriptorSize != 64)
        goto fail;
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = 1, .pBindings = &binding};
    VK_CHECK(vkCreateDescriptorSetLayout(p->device, &slci, NULL, &p->set_layout));
    layout_size(p->device, p->set_layout, &p->set_size);
    VkDeviceSize align = db.descriptorBufferOffsetAlignment ? db.descriptorBufferOffsetAlignment : 1;
    p->set_size = (p->set_size + align - 1) / align * align;
    binding_offset(p->device, p->set_layout, 0, &p->binding_offset);
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VK_CHECK(vkCreatePipelineLayout(p->device, &plci, NULL, &p->layout));
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(dmabuf_ubwc_spv), .pCode = dmabuf_ubwc_spv};
    VK_CHECK(vkCreateShaderModule(p->device, &smci, NULL, &module));
    for (unsigned i = 0; i < 2; i++) {
        VkBool32 chroma = i;
        VkSpecializationMapEntry entry = {0, 0, sizeof(chroma)};
        VkSpecializationInfo spec = {1, &entry, sizeof(chroma), &chroma};
        VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT, .layout = p->layout,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
                      .pSpecializationInfo = &spec}};
        VK_CHECK(vkCreateComputePipelines(p->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                          &p->pipelines[i]));
    }
    vkDestroyShaderModule(p->device, module, NULL);
    module = VK_NULL_HANDLE;
    p->nbands = make_bands(p->bands, p->w, p->h);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = MAX_INPUTS * p->nbands * p->set_size,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VK_CHECK(vkCreateBuffer(p->device, &bci, NULL, &p->descriptors));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(p->device, p->descriptors, &req);
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
        .allocationSize = req.size, .memoryTypeIndex = memory_type(p, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (alloc.memoryTypeIndex == UINT32_MAX)
        goto fail;
    VK_CHECK(vkAllocateMemory(p->device, &alloc, NULL, &p->descriptor_memory));
    VK_CHECK(vkBindBufferMemory(p->device, p->descriptors, p->descriptor_memory, 0));
    VK_CHECK(vkMapMemory(p->device, p->descriptor_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&p->descriptor_map));
    VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                         .buffer = p->descriptors};
    p->descriptor_address = vkGetBufferDeviceAddress(p->device, &address);
    struct band bands[MAX_BANDS];
    unsigned nbands = make_bands(bands, 1920, 1080);
    VkSubresourceLayout layouts[2] = {{.rowPitch = 2560}, {.offset = 2805760, .rowPitch = 2560}};
    if (!import_carriers(p, &check, -1, 4210688, layouts, 1920, 1080, bands, nbands,
                         p->descriptor_map))
        goto fail;
    destroy_input(p, &check);
    return true;
fail:
    if (module)
        vkDestroyShaderModule(p->device, module, NULL);
    destroy_input(p, &check);
    mp_err(p->log, "Cannot convert Q10C with this Vulkan driver\n");
    return false;
}

struct frame_ubwc *frame_ubwc_create(GL *gl, struct mp_log *log, int w, int h,
                                      uint32_t drm_format)
{
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (w & 1) || (h & 1) ||
        (gl && (!gl->FenceSync || !gl->ClientWaitSync || !gl->DeleteSync)) ||
        (drm_format != DRM_FORMAT_NV12 && drm_format != DRM_FORMAT_P010))
        return NULL;
    struct frame_ubwc *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    pthread_mutex_init(&p->lock, NULL);
    p->gl = gl;
    p->log = log;
    p->w = w;
    p->h = h;
    p->drm_format = drm_format;
    p->bytes_per_sample = drm_format == DRM_FORMAT_P010 ? 2 : 1;
    p->tp10 = drm_format == DRM_FORMAT_P010;
    p->vk_format = p->tp10 ? CARRIER : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    p->pitch = ((w + 127) & ~127) * p->bytes_per_sample;
    p->uv_offset = (VkDeviceSize)p->pitch * ((h + 31) & ~31);
    p->size = p->uv_offset + (VkDeviceSize)p->pitch * (((h / 2) + 31) & ~31);
    p->active = -1;
    for (unsigned i = 0; i < MAX_SLOTS; i++) {
        p->slots[i].desc.objects[0].fd = -1;
        p->slots[i].input_done_fd = -1;
    }
    for (unsigned i = 0; i < MAX_INPUTS; i++)
        p->inputs[i].fd = -1;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
    VK_CHECK(vkCreateInstance(&ici, NULL, &p->instance));
    uint32_t count = 16;
    VkPhysicalDevice devices[16];
    VK_CHECK(vkEnumeratePhysicalDevices(p->instance, &count, devices));
    for (unsigned i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.vendorID == 0x5143 && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            p->physical = devices[i];
            break;
        }
    }
    if (!p->physical)
        goto fail;
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceExternalImageFormatInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modifier, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 format = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_image, .format = p->vk_format,
        .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = p->tp10 ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkExternalImageFormatProperties external_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 format_props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props};
    VK_CHECK(vkGetPhysicalDeviceImageFormatProperties2(p->physical, &format, &format_props));
    if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        p->w > format_props.imageFormatProperties.maxExtent.width ||
        p->h > format_props.imageFormatProperties.maxExtent.height)
        goto fail;
    count = 32;
    VkQueueFamilyProperties queues[32];
    vkGetPhysicalDeviceQueueFamilyProperties(p->physical, &count, queues);
    for (p->family = 0; p->family < count; p->family++)
        if (queues[p->family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (p->family == count)
        goto fail;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = p->family, .queueCount = 1, .pQueuePriorities = &priority};
    p->display = gl ? eglGetCurrentDisplay() : EGL_NO_DISPLAY;
    const char *egl_ext = p->display != EGL_NO_DISPLAY ?
        eglQueryString(p->display, EGL_EXTENSIONS) : NULL;
    p->create_sync = (void *)eglGetProcAddress("eglCreateSyncKHR");
    p->wait_sync = (void *)eglGetProcAddress("eglWaitSyncKHR");
    p->destroy_sync = (void *)eglGetProcAddress("eglDestroySyncKHR");
    VkPhysicalDeviceExternalFenceInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalFenceProperties fence_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
    vkGetPhysicalDeviceExternalFenceProperties(p->physical, &fence_info, &fence_props);
    bool exportable = fence_props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;
    p->async = gl ? drm_format == DRM_FORMAT_P010 && egl_ext &&
        strstr(egl_ext, "EGL_ANDROID_native_fence_sync") &&
        strstr(egl_ext, "EGL_KHR_wait_sync") && p->create_sync && p->wait_sync &&
        p->destroy_sync && exportable : exportable;
    if (!gl && !p->async)
        goto fail;  // export mode hands out the conversion's fence
    const char *extensions[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier", "VK_EXT_queue_family_foreign", "VK_KHR_external_fence_fd",
        "VK_EXT_descriptor_buffer"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = p->async ? 5 : 4, .ppEnabledExtensionNames = extensions};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features vk12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &descriptor_buffer};
    if (p->tp10) {
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                              .pNext = &vk12};
        vkGetPhysicalDeviceFeatures2(p->physical, &features);
        if (!descriptor_buffer.descriptorBuffer || !vk12.bufferDeviceAddress)
            goto fail;
        vk12 = (VkPhysicalDeviceVulkan12Features){.sType = vk12.sType,
            .pNext = &descriptor_buffer, .bufferDeviceAddress = true};
        descriptor_buffer = (VkPhysicalDeviceDescriptorBufferFeaturesEXT){
            .sType = descriptor_buffer.sType, .descriptorBuffer = true};
        dci.pNext = &vk12;
        if (!p->async)
            extensions[4] = extensions[5];
        dci.enabledExtensionCount++;
    }
    VK_CHECK(vkCreateDevice(p->physical, &dci, NULL, &p->device));
    vkGetDeviceQueue(p->device, p->family, 0, &p->queue);
    p->fd_properties = (void *)vkGetDeviceProcAddr(p->device, "vkGetMemoryFdPropertiesKHR");
    p->export_fd = (void *)vkGetDeviceProcAddr(p->device, "vkGetMemoryFdKHR");
    p->export_fence = (void *)vkGetDeviceProcAddr(p->device, "vkGetFenceFdKHR");
    if (!p->fd_properties || !p->export_fd)
        goto fail;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = p->family};
    VK_CHECK(vkCreateCommandPool(p->device, &pci, NULL, &p->pool));
    if (p->tp10 && !create_tp10(p))
        goto fail;
    p->nslots = gl ? GL_SLOTS : 0;  // export mode adds them as it needs them
    for (unsigned i = 0; i < p->nslots; i++)
        if (!create_slot(p, &p->slots[i]))
            goto fail;
    mp_verbose(log, "Using GPU UBWC-to-linear transfer for Frame decoder (%s)\n",
               !gl ? "export" : p->async ? "native GPU fence" : "synchronous");
    pthread_mutex_lock(&instances_lock);
    p->next_instance = instances;
    instances = p;
    pthread_mutex_unlock(&instances_lock);
    return p;
fail:
    frame_ubwc_destroy(p);
    return NULL;
}

// Export mode: the next slot the caller does not hold, a new one if all are.
static struct output_slot *pick_slot(struct frame_ubwc *p)
{
    for (unsigned k = 0; k < p->nslots; k++) {
        unsigned i = (p->next + k) % p->nslots;
        if (!p->slots[i].held) {
            p->next = i;
            return &p->slots[i];
        }
    }
    if (p->nslots == MAX_SLOTS) {
        mp_err(p->log, "UBWC export: all %d buffers are held\n", MAX_SLOTS);
        return NULL;
    }
    struct output_slot *slot = &p->slots[p->nslots];
    if (!create_slot(p, slot))
        return NULL;
    p->next = p->nslots++;
    return slot;
}

static const AVDRMFrameDescriptor *convert(struct frame_ubwc *p,
                                           const AVDRMFrameDescriptor *src,
                                           struct mp_image *source,
                                            int *export_fence, uint64_t export_id)
{
    struct input_entry *in = NULL;
    bool fresh = false;
    int imported_fd = -1;
    int fence_fd = -1;
    struct mp_image *source_ref = NULL;
    struct output_slot *slot = p->gl ? &p->slots[p->next] : pick_slot(p);
    if (!slot)
        return NULL;
    bool used = slot->exported;
    if (p->active >= 0 || !wait_slot(p, slot) || src->nb_objects != 1 ||
        src->nb_layers != 1 || src->layers[0].format != p->drm_format ||
        src->layers[0].nb_planes != 2 ||
        src->objects[0].format_modifier != DRM_FORMAT_MOD_QCOM_COMPRESSED)
        return NULL;
    release_input(p, slot);
    // Return decoded buffers as soon as Vulkan has finished reading them,
    // rather than retaining three extra capture buffers until slot reuse.
    for (unsigned i = 0; i < p->nslots; i++) {
        struct output_slot *old = &p->slots[i];
        if (old->source && old->input_done_fd >= 0) {
            struct pollfd done = {.fd = old->input_done_fd, .events = POLLIN};
            if (poll(&done, 1, 0) > 0 && (done.revents & POLLIN))
                release_input(p, old);
        }
    }
    bool async = p->async && source;
    if (async && !(source_ref = mp_image_new_ref(source)))
        return NULL;
    VkSubresourceLayout layouts[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        const AVDRMPlaneDescriptor *plane = &src->layers[0].planes[i];
        if (plane->object_index != 0 || plane->offset < 0 || plane->pitch <= 0 ||
            (size_t)plane->offset >= src->objects[0].size)
            goto fail;
        layouts[i].offset = plane->offset;
        layouts[i].rowPitch = plane->pitch;
    }
    evict_inputs(p);
    in = find_input(p, src, layouts);
    if (!in)
        goto fail;
    VkDeviceSize descriptors = (in - p->inputs) * p->nbands * p->set_size;
    if (!in->image && p->tp10) {
        fresh = true;
        if (!import_carriers(p, in, src->objects[0].fd, src->objects[0].size, layouts,
                             p->w, p->h, p->bands, p->nbands, p->descriptor_map + descriptors))
            goto fail;
    } else if (!in->image) {
        fresh = true;
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED,
            .drmFormatModifierPlaneCount = 2, .pPlaneLayouts = layouts};
        VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &mod, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext,
            .imageType = VK_IMAGE_TYPE_2D, .format = p->vk_format,
            .extent = {p->w, p->h, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VK_CHECK(vkCreateImage(p->device, &ici, NULL, &in->image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(p->device, in->image, &req);
        if (req.size > src->objects[0].size) {
            mp_err(p->log, "UBWC import needs %llu bytes; decoder exported %zu\n",
                   (unsigned long long)req.size, src->objects[0].size);
            goto fail;
        }
        VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        VK_CHECK(p->fd_properties(p->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                  src->objects[0].fd, &props));
        imported_fd = dup(src->objects[0].fd);
        if (imported_fd < 0)
            goto fail;
        VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = in->image};
        VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = &dedicated, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = imported_fd};
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
            .allocationSize = src->objects[0].size,
            .memoryTypeIndex = memory_type(p, req.memoryTypeBits & props.memoryTypeBits, 0)};
        if (alloc.memoryTypeIndex == UINT32_MAX)
            goto fail;
        VK_CHECK(vkAllocateMemory(p->device, &alloc, NULL, &in->memory));
        imported_fd = -1;
        VK_CHECK(vkBindImageMemory(p->device, in->image, in->memory, 0));
    }
    in->last_use = ++p->frame;
    VK_CHECK(vkResetCommandBuffer(slot->cmd, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(slot->cmd, &begin));
    VkPipelineStageFlags stage = p->tp10 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT :
                                           VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkImageLayout layout = p->tp10 ? VK_IMAGE_LAYOUT_GENERAL :
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier ib[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = p->tp10 ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = p->family,
        .image = in->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
    ib[1] = ib[0];
    ib[1].image = in->uv_image;
    unsigned images = p->tp10 ? 2 : 1;
    VkBufferMemoryBarrier bb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .dstAccessMask = p->tp10 ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = used ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = used ? p->family : VK_QUEUE_FAMILY_IGNORED,
        .buffer = slot->buffer, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(slot->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        stage, 0, 0, NULL, 1, &bb, images, ib);
    if (p->tp10) {
        VkDescriptorBufferBindingInfoEXT binding = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
            .address = p->descriptor_address,
            .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT};
        p->bind_descriptors(slot->cmd, 1, &binding);
        for (unsigned i = 0; i < p->nbands; i++) {
            const struct band *b = &p->bands[i];
            uint32_t index = 0;
            VkDeviceSize offset = descriptors + i * p->set_size;
            struct {
                VkDeviceAddress rows;
                uint32_t pitch, pad;
            } push = {slot->address + (b->plane ? p->uv_offset : 0) +
                      (VkDeviceSize)b->row0 * p->pitch, p->pitch / 4};
            vkCmdBindPipeline(slot->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipelines[b->plane]);
            p->set_descriptor_offsets(slot->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout,
                                      0, 1, &index, &offset);
            vkCmdPushConstants(slot->cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            // A word a thread: two luma samples or a UV pair.
            vkCmdDispatch(slot->cmd, (p->w / 2 + 31) / 32, (b->rows + 3) / 4, 1);
        }
    } else {
        VkBufferImageCopy regions[2] = {
            {.bufferRowLength = p->pitch / p->bytes_per_sample,
                 .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
                .imageExtent = {p->w, p->h, 1}},
            {.bufferOffset = p->uv_offset, .bufferRowLength = p->pitch / (2 * p->bytes_per_sample),
                .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
                .imageExtent = {p->w / 2, p->h / 2, 1}}};
        vkCmdCopyImageToBuffer(slot->cmd, in->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               slot->buffer, 2, regions);
    }
    for (unsigned i = 0; i < 2; i++) {
        ib[i].srcAccessMask = ib[i].dstAccessMask;
        ib[i].dstAccessMask = 0;
        ib[i].oldLayout = layout;
        ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = p->family;
        ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    bb.srcAccessMask = bb.dstAccessMask;
    bb.dstAccessMask = 0;
    bb.srcQueueFamilyIndex = p->family;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(slot->cmd, stage,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1, &bb, images, ib);
    VK_CHECK(vkEndCommandBuffer(slot->cmd));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &slot->cmd};
    VK_CHECK(vkQueueSubmit(p->queue, 1, &submit, async ? slot->ready : VK_NULL_HANDLE));
    if (async) {
        VkFenceGetFdInfoKHR fd = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
            .fence = slot->ready, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VK_CHECK(p->export_fence(p->device, &fd, &fence_fd));
        if (fence_fd >= 0 && !p->gl) {
            slot->input_done_fd = fence_fd;
            fence_fd = -1;
            *export_fence = dup(slot->input_done_fd);
            if (*export_fence < 0)
                goto fail;
        } else if (fence_fd >= 0) {
            slot->input_done_fd = dup(fence_fd);
            if (slot->input_done_fd < 0)
                goto fail;
            EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence_fd, EGL_NONE};
            EGLSyncKHR sync = p->create_sync(p->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
            if (sync == EGL_NO_SYNC_KHR)
                goto fail;
            fence_fd = -1; // EGL owns the fd after successful creation.
            EGLBoolean waited = p->wait_sync(p->display, sync, 0);
            p->destroy_sync(p->display, sync);
            if (!waited)
                goto fail;
        }
        slot->input = in;
        in->users++;
        slot->source = source_ref;
        if (slot->input_done_fd < 0)
            release_input(p, slot); // Export returned an already-signalled fence.
    } else {
        VK_CHECK(vkQueueWaitIdle(p->queue));
    }
    slot->exported = true;
    if (!p->gl) {
        slot->held = true;
        slot->id = export_id;
        mp_trace(p->log, "UBWC export %p id=%llu\n", (void *)p,
                 (unsigned long long)slot->id);
    } else {
        p->active = p->next;
    }
    p->next = (p->next + 1) % p->nslots;
    return &slot->desc;
fail:
    vkDeviceWaitIdle(p->device);
    if (fence_fd >= 0)
        close(fence_fd);
    if (slot->input_done_fd >= 0) {
        close(slot->input_done_fd);
        slot->input_done_fd = -1;
    }
    talloc_free(source_ref);
    if (imported_fd >= 0)
        close(imported_fd);
    if (in && fresh)
        destroy_input(p, in);
    if (export_fence && *export_fence >= 0) {
        close(*export_fence);
        *export_fence = -1;
    }
    return NULL;
}

const AVDRMFrameDescriptor *frame_ubwc_convert(struct frame_ubwc *p,
                                             const AVDRMFrameDescriptor *src,
                                             struct mp_image *source)
{
    pthread_mutex_lock(&p->lock);
    const AVDRMFrameDescriptor *r = convert(p, src, source, NULL, 0);
    pthread_mutex_unlock(&p->lock);
    return r;
}

const AVDRMFrameDescriptor *frame_ubwc_export(struct frame_ubwc *p,
                                            const AVDRMFrameDescriptor *src,
                                            struct mp_image *source,
                                            int *fence_fd, uint64_t id)
{
    *fence_fd = -1;
    pthread_mutex_lock(&p->lock);
    const AVDRMFrameDescriptor *r = convert(p, src, source, fence_fd, id);
    pthread_mutex_unlock(&p->lock);
    return r;
}

void frame_ubwc_unhold(struct frame_ubwc *p, uint64_t id)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    for (unsigned i = 0; i < p->nslots; i++)
        if (p->slots[i].held && p->slots[i].id == id) {
            mp_trace(p->log, "UBWC release %p id=%llu\n", (void *)p,
                     (unsigned long long)id);
            p->slots[i].held = false;
        }
    pthread_mutex_unlock(&p->lock);
}

bool frame_ubwc_decoder_idle(void)
{
    bool idle = true;
    if (pthread_mutex_trylock(&instances_lock))
        return false;
    for (struct frame_ubwc *p = instances; p; p = p->next_instance) {
        if (pthread_mutex_trylock(&p->lock)) {
            idle = false;
            continue;
        }
        for (unsigned i = 0; i < p->nslots; i++) {
            struct output_slot *slot = &p->slots[i];
            if (!slot->source && !slot->input)
                continue;
            struct pollfd done = {.fd = slot->input_done_fd, .events = POLLIN};
            if (slot->input_done_fd >= 0 &&
                (poll(&done, 1, 0) <= 0 || !(done.revents & POLLIN) ||
                 (done.revents & (POLLERR | POLLNVAL)))) {
                idle = false;
                continue;
            }
            release_input(p, slot);
        }
        for (unsigned i = 0; i < MAX_INPUTS; i++)
            if (p->inputs[i].image && !p->inputs[i].users)
                destroy_input(p, &p->inputs[i]);
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&instances_lock);
    return idle;
}

// The decoder closed a session: its capture buffers stay allocated while
// anything here references them (imports kept for reuse, decoder frames
// kept until their transfer finished), and a new 8K session then fails to
// allocate its own. Let go of all of them before it starts.
void frame_ubwc_decoder_closed(void)
{
    pthread_mutex_lock(&instances_lock);
    for (struct frame_ubwc *p = instances; p; p = p->next_instance) {
        pthread_mutex_lock(&p->lock);
        for (unsigned i = 0; i < p->nslots; i++) {
            struct output_slot *slot = &p->slots[i];
            if (!slot->source && !slot->input)
                continue;
            if (slot->input_done_fd >= 0) {
                struct pollfd done = {.fd = slot->input_done_fd, .events = POLLIN};
                if (poll(&done, 1, 200) <= 0 || !(done.revents & POLLIN) ||
                    (done.revents & (POLLERR | POLLNVAL)))
                    continue;  // still read by the GPU: kept
            }
            release_input(p, slot);
        }
        for (unsigned i = 0; i < MAX_INPUTS; i++)
            if (p->inputs[i].image && !p->inputs[i].users)
                destroy_input(p, &p->inputs[i]);
        pthread_mutex_unlock(&p->lock);
    }
    // The app's imports of decoder buffers: it lets go of them on its next
    // loop iteration. Waited for without the lock, which the app's render
    // thread may need meanwhile.
    struct video_export *e = watched;
    uint32_t session = 0;
    if (e) {
        watchers++;
        session = __atomic_add_fetch(&e->session, 1, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_unlock(&instances_lock);
    if (!e)
        return;
    for (int i = 0; i < 100 && __atomic_load_n(&e->session_done, __ATOMIC_ACQUIRE) != session; i++)
        usleep(1000);
    pthread_mutex_lock(&instances_lock);
    watchers--;
    pthread_cond_broadcast(&watchers_done);
    pthread_mutex_unlock(&instances_lock);
}

void frame_ubwc_watch(struct video_export *e)
{
    pthread_mutex_lock(&instances_lock);
    watched = e;
    while (!e && watchers)
        pthread_cond_wait(&watchers_done, &instances_lock);
    pthread_mutex_unlock(&instances_lock);
}

void frame_ubwc_release(struct frame_ubwc *p)
{
    if (!p || p->active < 0)
        return;
    p->slots[p->active].fence = p->gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!p->slots[p->active].fence)
        p->gl->Finish();
    p->active = -1;
}
