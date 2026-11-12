#include "scene_renderer.h"

#include "eye_targets.h"
#include "direct_video.h"
#include "gl_interop.h"
#include "mirror_window.h"
#include "uploader.h"
#include "visible_part.h"
#include "vr_space.h"
#include "xr_context.h"

#include <cstdint>
// The SPIR-V headers declare bare arrays: <cstdint> first.
#include "shaders/fullscreen.vert.h"
#include "shaders/sky.frag.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// The sky: an equirectangular image made once at startup.
constexpr uint32_t kSkyWidth = 4096, kSkyHeight = 2048;
// The flat screen without a picture (linear).
constexpr float kScreenColor[3] = {0.02f, 0.02f, 0.025f};

VkClearColorValue clearColor(ClearMode mode) {
  switch (mode) {
    case ClearMode::KeyGreen: return {{0.0f, 1.0f, 0.0f, 1.0f}};
    case ClearMode::Transparent: return {{0.0f, 0.0f, 0.0f, 0.0f}};
    case ClearMode::Dark: break;
  }
  return {{0.004f, 0.004f, 0.006f, 1.0f}};
}

}  // namespace

void stereoRect(StereoLayout stereo, int eye, float* rect) {
  rect[0] = rect[1] = 1.0f;
  rect[2] = rect[3] = 0.0f;
  if (stereo == StereoLayout::SideBySide) {
    rect[0] = 0.5f;
    rect[2] = 0.5f * eye;
  } else if (stereo == StereoLayout::TopBottom) {
    rect[1] = 0.5f;
    rect[3] = 0.5f * eye;
  }
}

int surroundProjection(const VideoFormat& format, float eyeAspect, float& fisheyeFov) {
  fisheyeFov = 0.0f;
  switch (format.projection) {
    case Projection::Equirect360: return 1;
    case Projection::Fisheye180: fisheyeFov = kPi; return 2;
    case Projection::Fisheye190: fisheyeFov = 190.0f * kPi / 180.0f; return 2;
    case Projection::Fisheye200: fisheyeFov = 200.0f * kPi / 180.0f; return 2;
    // A cubemap is wider than tall; a tall eye picture holds it turned.
    case Projection::EquiAngularCubemap: return eyeAspect < 1.0f ? 4 : 3;
    default: return 0;
  }
}

EyeData eyeData(const EyeMatrices& eye, const VkRect2D& area, int index) {
  EyeData e{};
  e.orientation[0] = eye.orientation.x;
  e.orientation[1] = eye.orientation.y;
  e.orientation[2] = eye.orientation.z;
  e.orientation[3] = eye.orientation.w;
  e.position[0] = eye.position.x;
  e.position[1] = eye.position.y;
  e.position[2] = eye.position.z;
  e.position[3] = static_cast<float>(index);
  e.tangents[0] = eye.tanLeft;
  e.tangents[1] = eye.tanRight;
  e.tangents[2] = eye.tanDown;
  e.tangents[3] = eye.tanUp;
  e.area[0] = static_cast<float>(area.offset.x);
  e.area[1] = static_cast<float>(area.offset.y);
  e.area[2] = static_cast<float>(area.extent.width);
  e.area[3] = static_cast<float>(area.extent.height);
  e.stereo[0] = e.stereo[1] = 1.0f;
  return e;
}

namespace {

// Video v's turn as the surround shader takes it: local space into the
// picture's axes, the transpose of its rotation (columns).
void toPicture(const VideoDraw& v, float (*columns)[4]) {
  const Mat4 r = v.screen.rotation();
  for (int c = 0; c < 3; ++c)
    for (int k = 0; k < 3; ++k) columns[c][k] = r.m[k * 4 + c];
}

// The stereo correction for eye `index` into eye, if on.
bool correctEye(const VideoDraw& v, int index, EyeData& eye) {
  if (index >= 2 || !v.correction.eyes[index].on) return false;
  const EyeCorrection& correct = v.correction.eyes[index];
  eye.viewer[0] = correct.viewer.x;
  eye.viewer[1] = correct.viewer.y;
  eye.viewer[2] = correct.viewer.z;
  eye.viewer[3] = correct.distance;
  eye.camera[0] = correct.camera.x;
  eye.camera[1] = correct.camera.y;
  eye.camera[2] = correct.camera.z;
  return true;
}

}  // namespace

SceneRenderer::~SceneRenderer() {
  if (gpu_) gpu_->retire(skyImage_);
}

bool SceneRenderer::init(GpuContext& gpu, GlInterop& interop, Uploader& uploader,
                         EyeTargets& targets) {
  gpu_ = &gpu;
  interop_ = &interop;
  uploader_ = &uploader;
  targets_ = &targets;
  return keyMask_.initPasses(gpu) &&
         pipelines_.init(gpu, targets.renderPass(), targets.plainRenderPass(),
                         keyMask_.distancePass(), keyMask_.reachPass()) &&
         keyMask_.init(pipelines_) && createSky();
}

bool SceneRenderer::initGlass(const XrContext& xr) {
  const int eyes = std::min(xr.eyeCount(), 2);
  VkRect2D areas[2] = {};
  for (int i = 0; i < eyes; ++i)
    areas[i].extent = {static_cast<uint32_t>(xr.eyeWidth(i)), static_cast<uint32_t>(xr.eyeHeight(i))};
  glass_ = std::make_unique<GlassBackdrop>();
  if (glass_->init(*gpu_, eyes, areas) && pipelines_.initGlass(glass_->scenePass())) return true;
  glass_.reset();
  return false;
}

// Renders the night sky once into a mipmapped image, on the GPU at startup.
bool SceneRenderer::createSky() {
  GpuContext& gpu = *gpu_;
  VkDevice d = gpu.device();
  const uint32_t levels = 1 + static_cast<uint32_t>(std::floor(std::log2(kSkyWidth)));
  if (!gpu.createImage(kSkyWidth, kSkyHeight, levels, VK_FORMAT_R8G8B8A8_SRGB,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       skyImage_, "sky"))
    return false;

  VkAttachmentDescription2 color{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
  color.format = VK_FORMAT_R8G8B8A8_SRGB;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is drawn
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference2 ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
  ref.attachment = 0;
  ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ref.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &ref;
  VkSubpassDependency2 dep{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
  dep.srcSubpass = 0;
  dep.dstSubpass = VK_SUBPASS_EXTERNAL;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo2 passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
  passInfo.attachmentCount = 1;
  passInfo.pAttachments = &color;
  passInfo.subpassCount = 1;
  passInfo.pSubpasses = &subpass;
  passInfo.dependencyCount = 1;
  passInfo.pDependencies = &dep;
  VkRenderPass pass = VK_NULL_HANDLE;
  VkImageView level0 = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  bool ok = gpu.check(vkCreateRenderPass2(d, &passInfo, nullptr, &pass), "sky render pass");
  if (ok) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = skyImage_.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ok = gpu.check(vkCreateImageView(d, &view, nullptr, &level0), "sky view");
  }
  if (ok) {
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &level0;
    fb.width = kSkyWidth;
    fb.height = kSkyHeight;
    fb.layers = 1;
    ok = gpu.check(vkCreateFramebuffer(d, &fb, nullptr, &framebuffer), "sky framebuffer");
  }
  if (ok) {
    pipeline = pipelines_.create({kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert),
                                  kSpirv_sky_frag, sizeof(kSpirv_sky_frag),
                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false},
                                 pass, "sky pipeline");
    ok = pipeline != VK_NULL_HANDLE;
  }
  VkCommandBuffer cmd = ok ? gpu.beginOneShot() : VK_NULL_HANDLE;
  if (cmd) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {kSkyWidth, kSkyHeight}};
    VkSubpassBeginInfo subpassBegin{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
    subpassBegin.contents = VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass2(cmd, &begin, &subpassBegin);
    const VkViewport viewport{0, 0, static_cast<float>(kSkyWidth), static_cast<float>(kSkyHeight),
                              0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &begin.renderArea);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    VkSubpassEndInfo end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    vkCmdEndRenderPass2(cmd, &end);
    recordMipmaps(cmd, skyImage_.image, kSkyWidth, kSkyHeight, levels, 0);
    ok = gpu.submitOneShot(cmd) != 0;
  } else {
    ok = false;
  }
  gpu.retire([d, pass, level0, framebuffer, pipeline] {
    if (pipeline) vkDestroyPipeline(d, pipeline, nullptr);
    if (framebuffer) vkDestroyFramebuffer(d, framebuffer, nullptr);
    if (level0) vkDestroyImageView(d, level0, nullptr);
    if (pass) vkDestroyRenderPass(d, pass, nullptr);
  });
  if (!ok) return false;
  sky_.image = skyImage_.image;
  sky_.view = skyImage_.view;
  sky_.width = kSkyWidth;
  sky_.height = kSkyHeight;
  sky_.levels = levels;
  return true;
}

void SceneRenderer::setDirectVideo(DirectVideo* direct) {
  direct_ = direct;
  if (direct && direct->tp10SetLayout() &&
      pipelines_.initTp10(direct->tp10SetLayout(), direct->tp10MaskSetLayout())) {
    direct->setTp10Mask(keyMask_.maskView(), pipelines_.sampler(Pipelines::kVideoBase));
    direct->enableTp10(pipelines_.sampler(Pipelines::kVideoBase));
  }
  if (direct && direct->q08cSetLayouts() && pipelines_.initQ08c(direct->q08cSetLayouts()))
    direct->enableQ08c();
}

void SceneRenderer::timestamp(GpuContext::FrameSlot& slot, Timestamp t,
                              VkPipelineStageFlagBits stage) {
  if (!slot.queries) return;
  vkCmdWriteTimestamp(slot.cmd, stage, slot.queries, t);
  slot.timestampsWritten = t + 1;
}

void SceneRenderer::pushEye(VkCommandBuffer cmd, const EyeData& eye) {
  vkCmdPushConstants(cmd, pipelines_.layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(EyeData), &eye);
}

template <typename T>
void SceneRenderer::pushDraw(VkCommandBuffer cmd, const T& draw) {
  vkCmdPushConstants(cmd, pipelines_.layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     kDrawPushOffset, sizeof(T), &draw);
}

bool SceneRenderer::record(GpuContext::FrameSlot& slot, const EyeFrame& eyes,
                           const EyeMatrices* matrices, const FrameScene& scene, double time,
                           std::vector<SemaphoreWait>& waits) {
  VkCommandBuffer cmd = slot.cmd;
  slot.timestampsWritten = 0;
  if (slot.queries) vkCmdResetQueryPool(cmd, slot.queries, 0, kCount);
  timestamp(slot, kStart, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

  // A picture GL is still drawing into is not shown.
  std::optional<VideoDraw> video = scene.video;
  if (video && video->texture && video->texture->shared &&
      !interop_->useInFrame(*video->texture->shared))
    video->texture = nullptr;
  if (video && video->texture && video->texture->yuv) {
    if (direct_) direct_->useInFrame(*video->texture->yuv);
    else video->texture = nullptr;
  }
  for (const OverlayDraw& item : scene.overlays)
    if (const PreviewItem* preview = std::get_if<PreviewItem>(&item))
      if (preview->texture && preview->texture->shared)
        interop_->useInFrame(*preview->texture->shared);
  interop_->frameWaits(waits);
  const size_t sharedWaits = waits.size();
  if (direct_ && !direct_->frameWaits(waits)) {
    waits.resize(sharedWaits);
    direct_->frameAbandoned();
    if (video && video->texture && video->texture->yuv) video->texture = nullptr;
  }
  uploader_->record(cmd, slot.index);
  interop_->recordAcquires(cmd);
  if (direct_) direct_->recordAcquires(cmd);
  timestamp(slot, kUploaded, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  // The colour key's mask, before any pass reads it (its time is glass's).
  if (video && !testPattern_) drawKeyMask(cmd, *video, scene.clear, eyes, matrices);

  // Glass menus (desktop): what each eye shows before the first of them,
  // drawn small and blurred before the eye passes (docs/vulkan-renderer.md, 7).
  size_t behindGlass = scene.overlays.size();
  for (size_t k = 0; k < scene.overlays.size(); ++k) {
    const PanelItem* panel = std::get_if<PanelItem>(&scene.overlays[k]);
    if (panel && panel->texture && panel->glassAlpha > 0) {
      behindGlass = k;
      break;
    }
  }
  bool glass = glass_ && !testPattern_ && behindGlass < scene.overlays.size();
  for (int i = 0; glass && i < eyes.count; ++i) glass = glass_->prepare(i, eyes.eyes[i].area);
  if (glass) {
    glassPass_ = true;
    for (int i = 0; i < eyes.count; ++i) {
      const VkRect2D area = glass_->beginScene(cmd, i, clearColor(scene.clear));
      const EyeData eye = eyeData(matrices[i], area, i);
      if (scene.sky) drawSky(cmd, eye);
      if (video) drawVideo(cmd, eye, i, *video, scene.clear, 0.0f);  // blurred anyway
      if (behindGlass > 0) drawOverlays(cmd, eye, matrices[i], scene, behindGlass, nullptr);
      glass_->endScene(cmd);
      glass_->blur(cmd, i, pipelines_, scene.clear == ClearMode::KeyGreen);
    }
    glassPass_ = false;
  }
  timestamp(slot, kGlassDone, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

  for (int i = 0; i < eyes.count; ++i) {
    const EyeImage& image = eyes.eyes[i];
    VkClearColorValue clear = clearColor(scene.clear);
    if (testPattern_) {
      const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(time * 2.0));
      clear = i == 0 ? VkClearColorValue{{0.25f + 0.5f * pulse, 0.05f, 0.05f, 1.0f}}
                     : VkClearColorValue{{0.05f, 0.05f, 0.25f + 0.5f * pulse, 1.0f}};
    }
    targets_->beginEye(cmd, i, image, clear);
    plainEye_ = !image.densityMap;
    if (!testPattern_) {
      const EyeData eye = eyeData(matrices[i], image.area, i);
      if (scene.sky) drawSky(cmd, eye);
      if (video) drawVideo(cmd, eye, i, *video, scene.clear, video->sharpen);
      if (!scene.overlays.empty())
        drawOverlays(cmd, eye, matrices[i], scene, scene.overlays.size(),
                     glass ? glass_->texture(i) : nullptr);
    }
    if (scene.showFoveation) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.foveation(plainEye_));
      pushDraw(cmd, scene.foveation[i]);
      vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    targets_->endEye(cmd, image);
    timestamp(slot, i == 0 ? kEye0Done : kEye1Done, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  }
  if (eyes.count < 2) timestamp(slot, kEye1Done, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  interop_->recordReleases(cmd);
  if (direct_) direct_->recordReleases(cmd);
  if (mirror_ && eyes.count > 0) mirror_->record(cmd, eyes.eyes[0], waits);
  timestamp(slot, kEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  return !scene.video || !scene.video->texture || video->texture;
}

// Room-fixed: the eye's own orientation alone turns it.
void SceneRenderer::drawSky(VkCommandBuffer cmd, EyeData eye) {
  SurroundPush p{};
  for (int c = 0; c < 3; ++c) p.toPicture[c][c] = 1.0f;
  p.params[1] = 1.0f;
  p.mode[0] = 1;  // 360
  p.mode[3] = 100;  // opaque
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(Pipelines::kSurround));
  pipelines_.bindTextures(cmd, &sky_, Pipelines::kSky);
  pushEye(cmd, eye);
  pushDraw(cmd, p);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

void SceneRenderer::drawKeyMask(VkCommandBuffer cmd, const VideoDraw& v, ClearMode clear,
                                const EyeFrame& eyes, const EyeMatrices* matrices) {
  // As drawVideo() keys it.
  const SeeThrough& see = v.seeThrough;
  if (!v.texture || clear == ClearMode::Dark || v.format.projection == Projection::Flat ||
      see.packedAlpha || see.greenKey > 0 || !see.colourKey)
    return;
  const DirectFrame* yuv = v.texture->yuv;
  const bool tp10 = yuv && yuv->tp10() && direct_;
  const int q08c = yuv && direct_ ? yuv->q08c() : -1;
  const uint32_t width = v.texture->width, height = v.texture->height;
  KeyMaskPush p{};
  p.frame[0] = static_cast<float>(width);
  p.frame[1] = static_cast<float>(height);
  p.frame[2] = static_cast<float>(KeyMask::texelsPer(width, height));
  p.frame[3] = see.packedKeyColour();
  // The cut fades in over up to 0.3 (colourAlpha()); the shrink reaches up
  // to 40 texels (its halo: beyond about 25 it eats into the subject), spill
  // removal up to 64 past it.
  p.key[0] = see.keyCut;
  p.key[1] = see.keySoft;
  p.key[2] = 40.0f * v.key.shrink / 100.0f;
  p.key[3] = 64.0f * v.key.spill / 100.0f;
  if (yuv) {
    for (int i = 0; i < 4; ++i) {
      p.yuv[i] = yuv->color().neutralRange[i];
      p.matrix[i] = yuv->color().matrix[i];
    }
  } else {
    p.matrix[0] = 0.2126f;  // mpv's image: BT.709
    p.matrix[1] = 0.0722f;
  }
  p.mode[0] = q08c >= 0 ? 2 : yuv ? 1 : 0;  // video.glsl's kSource...
  p.mode[1] = (yuv ? yuv->color().tvGamma : v.tvGamma) ? static_cast<uint32_t>(kTvGamma) : 0u;
  float stereo[4], fisheyeFov;
  stereoRect(v.format.stereo, 0, stereo);
  p.mode[2] = stereo[0] < 1.0f ? 1 : stereo[1] < 1.0f ? 2 : 0;
  p.mode[3] = static_cast<uint32_t>(
      surroundProjection(v.format, v.frameAspect * stereo[0] / stereo[1], fisheyeFov));
  // Without shrink or spill removal the mask is the same for every frame.
  const bool distances = p.key[2] > 0.0f || p.key[3] > 0.0f;
  const void* image = !distances ? nullptr
                      : yuv      ? static_cast<const void*>(yuv)
                                 : static_cast<const void*>(v.texture->shared);
  const uint64_t generation = !distances ? 0
                              : yuv      ? yuv->serial()
                              : v.texture->shared ? v.texture->shared->content()
                                                  : ++unsharedMasks_;
  // What the eyes can read of a frame that changes the mask (none: the
  // whole part): a rectangle per eye's part, one for both of a mono video.
  KeyMask::Rect needs[2];
  int count = 0;
  if (distances && matrices && (p.mode[3] == 0 || p.mode[3] == 2)) {
    for (int i = 0; i < eyes.count && i < 2; ++i) {
      // As drawVideo() draws this eye.
      EyeData eye = eyeData(matrices[i], eyes.eyes[i].area, i);
      stereoRect(v.format.stereo, i, eye.stereo);
      VisibleView view;
      for (int k = 0; k < 4; ++k) {
        view.tangents[k] = eye.tangents[k];
        view.orientation[k] = eye.orientation[k];
      }
      float columns[3][4];
      toPicture(v, columns);
      for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) view.toPicture[c][k] = columns[c][k];
      view.zoom = v.screen.zoom;
      view.correct = correctEye(v, i, eye) && eye.viewer[3] > 0.0f;
      for (int k = 0; k < 3; ++k) {
        view.viewer[k] = eye.viewer[k];
        view.camera[k] = eye.camera[k];
      }
      view.distance = eye.viewer[3];
      view.projection = surroundProjection(
          v.format, v.frameAspect * eye.stereo[0] / eye.stereo[1], view.fisheyeFov);
      view.flip = v.flip;
      UvBounds bounds;
      if (!visiblePart(view, bounds)) {
        count = 0;
        break;
      }
      const KeyMask::Rect r = KeyMask::needed(eye.stereo, bounds, width, height);
      if (count == 1 && p.mode[2] == 0)
        needs[0] = {std::min(needs[0].x0, r.x0), std::min(needs[0].y0, r.y0),
                    std::max(needs[0].x1, r.x1), std::max(needs[0].y1, r.y1)};
      else
        needs[count++] = r;
    }
  }
  if (keyMask_.current(image, generation, p, needs, count, width, height)) return;

  auto push = [&] {
    vkCmdPushConstants(cmd, pipelines_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
  };
  if (distances) {
    const auto source =
        tp10        ? Pipelines::kTp10
        : q08c >= 0 ? static_cast<Pipelines::SurroundSource>(Pipelines::kYcbcr + q08c)
        : yuv       ? Pipelines::kPlanes
                    : Pipelines::kImage;
    for (int i = 0; i < keyMask_.distanceCount(); ++i) {
      keyMask_.begin(cmd, 0, width, height, keyMask_.distance(i));
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.keyDistance(source));
      if (tp10) direct_->bindTp10(cmd, pipelines_.tp10Layout(), *yuv);
      else if (q08c >= 0) pipelines_.bindQ08c(cmd, q08c, *v.texture);
      else pipelines_.bindTextures(cmd, v.texture, Pipelines::kVideoBase, yuv ? &yuv->chroma() : nullptr);
      push();
      vkCmdDraw(cmd, 3, 1, 0, 0);
      keyMask_.end(cmd);
    }
  }
  for (int i = 0; i < keyMask_.madeCount(); ++i) {
    keyMask_.begin(cmd, 1, width, height, keyMask_.made(i));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.keyReach());
    pipelines_.bindTextures(cmd, distances ? &keyMask_.distances() : nullptr, Pipelines::kVideoBase);
    push();
    vkCmdDraw(cmd, 3, 1, 0, 0);
    keyMask_.end(cmd);
  }
}

void SceneRenderer::drawVideo(VkCommandBuffer cmd, EyeData eye, int index, const VideoDraw& v,
                              ClearMode clear, float sharpen) {
  stereoRect(v.format.stereo, index, eye.stereo);
  uint32_t flags = 0;
  if (v.flip) flags |= kFlip;
  if (!v.mipmaps) flags |= kBaseLevel;
  if (v.look) flags |= kLook;
  if (v.photo) flags |= kPhoto;
  // A decoded frame: its planes, from their only level.
  const DirectFrame* yuv = v.texture ? v.texture->yuv : nullptr;
  if (yuv) flags |= kYuv | kBaseLevel;
  if (yuv ? yuv->color().tvGamma : v.tvGamma) flags |= kTvGamma;
  const Pipelines::SamplerId sampler =
      v.mipmaps && !yuv ? Pipelines::kVideoTrilinear : Pipelines::kVideoBase;
  const Texture* chroma = yuv ? &yuv->chroma() : nullptr;
  // A Q10C frame: its TP10 descriptors, for the pipelines for them; a Q08C
  // one: its image, for those of its chroma siting (no sharpening: with it
  // on, VideoPlayer::setSharpened has mpv convert frames).
  const bool tp10 = yuv && yuv->tp10() && direct_;
  const int q08c = yuv && direct_ ? yuv->q08c() : -1;
  // keyMask: with the colour key's mask (KeyMask) as set 1.
  auto bind = [&](VkPipeline pipeline, bool keyMask = false) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (tp10) {
      direct_->bindTp10(cmd, keyMask ? pipelines_.tp10KeyLayout() : pipelines_.tp10Layout(), *yuv,
                        keyMask);
      return;
    }
    if (q08c >= 0) pipelines_.bindQ08c(cmd, q08c, *v.texture);
    else pipelines_.bindTextures(cmd, v.texture, sampler, chroma);
    if (keyMask) pipelines_.bindKeyMask(cmd, q08c, keyMask_.set());
  };
  // The general pipelines.
  const MaskColours colours = v.seeThrough.maskColours;
  auto general = [&](bool screen, bool maskFirst, bool colourKey) {
    if (tp10) return pipelines_.tp10(screen, maskFirst, colours, colourKey, plainEye_);
    if (q08c >= 0) return pipelines_.q08c(q08c, screen, maskFirst, colours, colourKey, plainEye_);
    return pipeline(
        Pipelines::videoPipeline(screen, maskFirst, colours, colourKey, sharpen > 0.0f));
  };

  if (v.format.projection == Projection::Flat) {
    const ScreenSurface surface(v.screen, eyeDisplayAspect(v.format, v.frameAspect));
    ScreenPush p{};
    const Mat4& pose = surface.pose();
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) p.pose[r][c] = pose.m[c * 4 + r];
    p.size[0] = surface.width();
    p.size[1] = surface.height();
    p.size[2] = surface.distance();
    p.size[3] = surface.radius();
    for (int c = 0; c < 3; ++c) {
      p.color[c] = v.texture ? v.balance[c] : kScreenColor[c];
      p.adjust[c] = v.adjust[c];
    }
    p.color[3] = clear == ClearMode::Transparent ? v.opacity / 100.0f : 1.0f;
    p.adjust[3] = sharpen;
    if (v.texture) flags |= kTexture;
    p.mode[1] = flags;
    p.mode[2] = static_cast<uint32_t>(antiShimmerTaps(v.antiShimmer));
    if (yuv)
      for (int i = 0; i < 4; ++i) {
        p.yuv[i] = yuv->color().range[i];
        p.yuvMatrix[i] = yuv->color().matrix[i];
      }
    bind(general(true, false, false));
    pushEye(cmd, eye);
    pushDraw(cmd, p);
    vkCmdDraw(cmd, 2 * 65, 1, 0, 0);
    return;
  }
  if (!v.texture) return;

  SurroundPush p{};
  // Turned with the screen (head translation only through the correction's
  // motion emulation, stereo_correction.h): local space into the picture's
  // axes, the transpose of its rotation.
  toPicture(v, p.toPicture);
  for (int c = 0; c < 3; ++c) p.toPicture[c][3] = v.balance[c];
  float fisheyeFov = 0.0f;
  const int projection =
      surroundProjection(v.format, v.frameAspect * eye.stereo[0] / eye.stereo[1], fisheyeFov);
  p.params[0] = fisheyeFov;
  p.params[1] = v.screen.zoom;
  for (int c = 0; c < 3; ++c) p.adjust[c] = v.adjust[c];
  p.adjust[3] = sharpen;
  if (clear == ClearMode::KeyGreen) flags |= kKeyGreen;
  if (clear == ClearMode::Transparent) flags |= kAlphaOut;
  if (clear != ClearMode::Dark) {
    // The packed mask is centred on each eye's part at 40% of its size.
    if (v.seeThrough.packedAlpha) flags |= kPackedAlpha;
    else if (v.seeThrough.greenKey > 0) flags |= kChromaKey;
    else if (v.seeThrough.colourKey) flags |= kColourKey;
    p.alpha[0] = 0.4f * eye.stereo[0];
    p.alpha[1] = 0.4f * eye.stereo[1];
    // The green screen settings (0..100) as the shader's thresholds.
    p.chroma[0] = v.seeThrough.greenKey;
    p.chroma[1] = 0.005f + 0.045f * v.key.softness / 100.0f;
    p.chroma[2] = v.key.spill / 50.0f;
    p.chroma[3] = 6.0f * v.key.shrink / 100.0f;
    if (flags & kColourKey) {
      // A flat background: its colour in alpha.x (keyColour() in
      // video.glsl), its cut, and the part of its mask's image the frame's
      // mask fills (drawKeyMask()).
      p.chroma[0] = v.seeThrough.keyCut;
      p.chroma[1] = v.seeThrough.keySoft;
      KeyMask::scale(v.texture->width, v.texture->height, &p.chroma[2]);
      p.alpha[0] = v.seeThrough.packedKeyColour();
    }
  }
  if (correctEye(v, index, eye)) flags |= kCorrect;
  p.mode[0] = static_cast<uint32_t>(projection);
  p.mode[1] = flags;
  const int taps = antiShimmerTaps(v.antiShimmer);
  p.mode[2] = static_cast<uint32_t>(taps);
  p.mode[3] = clear == ClearMode::Transparent ? static_cast<uint32_t>(v.opacity) : 100;
  // A mask or key reads the frame as it came (VideoDraw::readsFrame); the
  // picture settings are applied to what is shown (a general pipeline, as
  // they aren't neutral then), a video's as mpv does (its Kr, Kb).
  const bool readsFrame = (flags & (kPackedAlpha | kChromaKey | kColourKey)) ||
                          (clear != ClearMode::Dark && v.seeThrough.picking);
  if (yuv) {
    const YuvColor& c = yuv->color();
    for (int i = 0; i < 4; ++i) p.yuv[i] = readsFrame ? c.neutralRange[i] : c.range[i];
    p.params[2] = c.matrix[0];
    p.params[3] = c.matrix[1];
    p.alpha[2] = c.matrix[2];
    p.alpha[3] = c.matrix[3];
  } else if (readsFrame) {
    p.params[2] = 0.2126f;  // mpv's image: BT.709
    p.params[3] = 0.0722f;
  }
  const bool maskFirst = (flags & kPackedAlpha) && (flags & (kKeyGreen | kAlphaOut));
  // A common mode has a pipeline of its own: the full-size level with one
  // tap, no adjustments, no sharpening, a packed mask only with transparent
  // output, and a green screen (with shrink its own), with the colour
  // balance or the tone look without key green (whose cap is the general
  // ones'). The colour balance has its own, and the look its own with the
  // balance. The colour key has its own pipelines: for the common modes but
  // cubemaps, with the colour balance, without and with the look, for the
  // eye pass with a density map, else general ones.
  const bool adjusted = v.adjust[0] != 0.0f || v.adjust[1] != 0.0f || v.adjust[2] != 0.0f;
  const bool balance = v.balance[0] != 0.0f || v.balance[1] != 0.0f || v.balance[2] != 0.0f;
  const bool commonMode =
      (flags & kBaseLevel) && taps == 1 && !adjusted && sharpen <= 0.0f && !glassPass_;
  const auto source =
      tp10        ? Pipelines::kTp10
      : q08c >= 0 ? static_cast<Pipelines::SurroundSource>(Pipelines::kYcbcr + q08c)
      : yuv       ? Pipelines::kPlanes
                  : Pipelines::kImage;
  if (flags & kColourKey) {
    VkPipeline keyed = VK_NULL_HANDLE;
    if (commonMode && projection < 3 && !(flags & kKeyGreen))
      keyed = pipelines_.keyed(source, plainEye_, v.look);
    bind(keyed ? keyed : general(false, false, true), true);
    pushEye(cmd, eye);
    pushDraw(cmd, p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    return;
  }
  VkPipeline common = VK_NULL_HANDLE;
  if (commonMode && !(maskFirst && (flags & kKeyGreen)) &&
      !((flags & kChromaKey) && (balance || v.look) && (flags & kKeyGreen))) {
    const auto family = projection >= 3   ? Pipelines::kCubemap
                        : projection == 2 ? Pipelines::kFisheye
                                          : Pipelines::kEquirect;
    const auto packed =
        colours == MaskColours::RedBlue ? Pipelines::kPackedRedBlue : Pipelines::kPackedMask;
    const auto cut = maskFirst            ? packed
                     : flags & kChromaKey ? (p.chroma[3] > 0.0f ? Pipelines::kGreenScreenShrink
                                                                : Pipelines::kGreenScreen)
                                          : Pipelines::kUncut;
    const auto finish = v.look  ? Pipelines::kLookBalanced
                        : balance ? Pipelines::kBalanced
                                  : Pipelines::kAsItIs;
    // The other colours' cuts are made for fisheye alone, and shrink's not
    // for cubemaps or with the look: else the general one.
    common = pipelines_.surround(family, source, cut, plainEye_, finish);
  }
  bind(common ? common : general(false, maskFirst, false));
  pushEye(cmd, eye);
  pushDraw(cmd, p);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

void SceneRenderer::drawOverlays(VkCommandBuffer cmd, const EyeData& eye,
                                 const EyeMatrices& matrices, const FrameScene& scene,
                                 size_t count, const Texture* backdrop) {
  // Pushed once: the pipelines share the layout, so it stays for every draw.
  pushEye(cmd, eye);
  VkPipeline bound = VK_NULL_HANDLE;
  auto bind = [&](Pipelines::Id id) {
    if (bound == pipeline(id)) return;
    bound = pipeline(id);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
  };
  for (size_t k = 0; k < count && k < scene.overlays.size(); ++k) {
    const OverlayDraw& item = scene.overlays[k];
    if (const PanelItem* panel = std::get_if<PanelItem>(&item)) {
      if (!panel->texture) continue;
      bind(Pipelines::kPanel);
      drawPanel(cmd, *panel, scene.clear, backdrop);
    } else if (const QuadsItem* quads = std::get_if<QuadsItem>(&item)) {
      bind(Pipelines::kPointer);
      for (const Quad& q : quads->quads) drawPointer(cmd, q, false);
    } else if (const LaserItem* laser = std::get_if<LaserItem>(&item)) {
      bind(Pipelines::kPointer);
      const Vec3 from = laser->from, to = laser->to;
      // A flat strip that always faces the eye: an unlit tube looks the same.
      const Vec3 dir = normalize(to - from);
      const Vec3 side = cross(dir, normalize(matrices.position - from));
      if (length(side) >= 1e-4f) {
        const Vec3 offset = normalize(side) * 0.001f;  // 2 mm wide
        drawPointer(cmd, {from - offset, from + offset, to - offset, to + offset}, false);
      }
      // The dot: a disc facing the eye, which an unlit sphere looks like.
      const Vec3 toEye = matrices.position - to;
      const float distance = length(toEye);
      if (distance > 1e-4f) {
        const Vec3 n = toEye * (1.0f / distance);
        const Vec3 up = std::abs(n.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 a = normalize(cross(up, n)) * laser->dotRadius;
        const Vec3 b = normalize(cross(n, a)) * laser->dotRadius;
        drawPointer(cmd, {to - a - b, to + a - b, to - a + b, to + a + b}, true);
      }
    } else if (const PreviewItem* preview = std::get_if<PreviewItem>(&item)) {
      // Not one GL still holds (record() did not acquire it).
      const Texture* t = preview->texture;
      if (!t || (t->shared && t->shared->inGl())) continue;
      bind(Pipelines::kPreview);
      drawPreview(cmd, *preview);
    }
  }
}

void SceneRenderer::drawPanel(VkCommandBuffer cmd, const PanelItem& panel, ClearMode clear,
                              const Texture* backdrop) {
  PanelPush p{};
  uint32_t flags = 0;
  const Mat4 model = panel.curved ? panel.curved->model() : panel.model;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) p.model[r][c] = model.m[c * 4 + r];
  p.uv[1] = p.uv[3] = 1.0f;
  p.shape[1] = 1.0f;
  if (panel.curved) {
    const CurvedPanel& c = *panel.curved;
    const float width = c.right - c.left, height = c.top - c.bottom;
    flags |= kPanelCurved;
    p.uv[0] = c.u0;
    p.uv[1] = c.u1;
    p.uv[2] = c.v0;
    p.uv[3] = c.v1;
    p.arc[0] = (c.left + c.u0 * width) / c.radius;
    p.arc[1] = (c.left + c.u1 * width) / c.radius;
    p.arc[2] = c.top - c.v0 * height;
    p.arc[3] = c.top - c.v1 * height;
    p.shape[0] = c.radius;
    p.shape[1] = c.pixelScale;
    p.lodBias = menuLodBias_;
  }
  if (panel.hover) {
    flags |= kPanelHover;
    if (panel.hover->danger) flags |= kPanelHoverDanger;
    flags |= uint32_t(panel.hover->rim[0]) << 8 | uint32_t(panel.hover->rim[1]) << 16 |
             uint32_t(panel.hover->rim[2]) << 24;
    for (int i = 0; i < 4; ++i) p.hover[i] = panel.hover->rect[i];
    for (int i = 0; i < 4; ++i) p.hoverClip[i] = panel.hover->clip[i];
    p.shape[2] = panel.hover->radius;
  }
  // Glass: the blurred backdrop behind its translucent parts; without one
  // (the Frame) it shows its own translucency.
  const bool glass = backdrop && panel.glassAlpha > 0;
  if (glass) flags |= kPanelGlass;
  p.shape[3] = panel.glassAlpha;
  p.opacity = panel.opacity;
  if (clear == ClearMode::KeyGreen) flags |= kPanelPassthrough;
  p.flags = flags;
  pipelines_.bindTextures(cmd, panel.texture, Pipelines::kCanvas, glass ? backdrop : nullptr);
  pushDraw(cmd, p);
  vkCmdDraw(cmd, panel.curved ? 2 * 65 : 4, 1, 0, 0);
}

void SceneRenderer::drawPointer(VkCommandBuffer cmd, const Quad& corners, bool dot) {
  PointerPush p{};
  for (int i = 0; i < 4; ++i) {
    p.corners[i][0] = corners[i].x;
    p.corners[i][1] = corners[i].y;
    p.corners[i][2] = corners[i].z;
    p.corners[i][3] = 1.0f;
  }
  // Orange corner handle, unlit and opaque (linear).
  p.color[0] = 1.0f;
  p.color[1] = 0.25f;
  p.color[3] = 1.0f;
  p.mode[0] = dot ? 1 : 0;
  pushDraw(cmd, p);
  vkCmdDraw(cmd, 4, 1, 0, 0);
}

void SceneRenderer::drawPreview(VkCommandBuffer cmd, const PreviewItem& preview) {
  PreviewPush p{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) p.model[r][c] = preview.model.m[c * 4 + r];
  stereoRect(preview.format.stereo, 0, p.picture);  // the left eye
  uint32_t flags = 0;
  if (preview.flip) flags |= kPreviewFlip;
  if (preview.format.projection != Projection::Flat) {
    flags |= kPreviewSurround;
    const Texture& t = *preview.texture;
    const float eyeAspect = static_cast<float>(t.width) * p.picture[0] /
                            (static_cast<float>(t.height) * p.picture[1]);
    p.mode[0] = static_cast<uint32_t>(surroundProjection(preview.format, eyeAspect, p.view[2]));
    p.view[0] = preview.aheadTangents[0];
    p.view[1] = preview.aheadTangents[1];
  }
  p.mode[1] = flags;
  pipelines_.bindTextures(cmd, preview.texture, Pipelines::kVideoBase);
  pushDraw(cmd, p);
  vkCmdDraw(cmd, 4, 1, 0, 0);
}

bool SceneRenderer::readTimes(const GpuContext::FrameSlot& slot, GpuTimes& times) {
  if (slot.timestampsWritten < kCount) return false;
  uint64_t ticks[GpuContext::kTimestamps];
  if (!gpu_->readTimestamps(slot, ticks)) return false;
  const double ms = gpu_->caps().timestampPeriod * 1e-6;
  auto span = [&](Timestamp a, Timestamp b) {
    // Masked to the valid bits, a counter may wrap between two of them.
    return ticks[b] >= ticks[a] ? static_cast<double>(ticks[b] - ticks[a]) * ms : 0.0;
  };
  times.upload = span(kStart, kUploaded);
  times.glass = span(kUploaded, kGlassDone);
  times.eye[0] = span(kGlassDone, kEye0Done);
  times.eye[1] = span(kEye0Done, kEye1Done);
  times.mirror = span(kEye1Done, kEnd);
  times.frame = span(kStart, kEnd);
  return true;
}
