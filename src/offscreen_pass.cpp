#include "offscreen_pass.h"

#include <cstdint>
// The SPIR-V headers declare bare arrays: <cstdint> first.
#include "shaders/fullscreen.vert.h"

bool createOffscreenPass(GpuContext& gpu, VkFormat format, VkImageLayout initialLayout,
                         VkImageLayout finalLayout, VkPipelineStageFlags waitFor,
                         VkPipelineStageFlags readBy, VkAccessFlags readAccess, VkRenderPass& pass,
                         const char* what) {
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = initialLayout;
  attachment.finalLayout = finalLayout;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color;
  VkSubpassDependency before{};
  before.srcSubpass = VK_SUBPASS_EXTERNAL;
  before.dstSubpass = 0;
  before.srcStageMask = waitFor;
  before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkSubpassDependency after{};
  after.srcSubpass = 0;
  after.dstSubpass = VK_SUBPASS_EXTERNAL;
  after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  after.dstStageMask = readBy;
  after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  after.dstAccessMask = readAccess;
  const VkSubpassDependency dependencies[2] = {before, after};
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &attachment;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 2;
  info.pDependencies = dependencies;
  return gpu.check(vkCreateRenderPass(gpu.device(), &info, nullptr, &pass), what);
}

bool createOffscreenFramebuffer(GpuContext& gpu, VkRenderPass pass, VkImageView view, uint32_t width,
                                uint32_t height, VkFramebuffer& framebuffer, const char* what) {
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = pass;
  info.attachmentCount = 1;
  info.pAttachments = &view;
  info.width = width;
  info.height = height;
  info.layers = 1;
  return gpu.check(vkCreateFramebuffer(gpu.device(), &info, nullptr, &framebuffer), what);
}

VkPipeline fullscreenPipeline(GpuContext& gpu, VkPipelineLayout layout, VkRenderPass pass,
                              const uint32_t* fragment, size_t fragmentBytes,
                              VkPipelineCreateFlags flags, VkColorComponentFlags writeMask,
                              const char* what) {
  VkDevice d = gpu.device();
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule modules[2] = {};
  const uint32_t* code[2] = {kSpirv_fullscreen_vert, fragment};
  const size_t bytes[2] = {sizeof(kSpirv_fullscreen_vert), fragmentBytes};
  bool ok = true;
  for (int i = 0; i < 2 && ok; ++i) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes[i];
    info.pCode = code[i];
    ok = gpu.check(vkCreateShaderModule(d, &info, nullptr, &modules[i]), what);
  }
  if (ok) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules[0];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules[1];
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = writeMask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicInfo.dynamicStateCount = 2;
    dynamicInfo.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.flags = flags;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamicInfo;
    info.layout = layout;
    info.renderPass = pass;
    ok = gpu.check(vkCreateGraphicsPipelines(d, gpu.pipelineCache(), 1, &info, nullptr, &pipeline),
                   what);
  }
  for (VkShaderModule m : modules)
    if (m) vkDestroyShaderModule(d, m, nullptr);
  return ok ? pipeline : VK_NULL_HANDLE;
}
