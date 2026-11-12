// Color looks (mod #7 part 2) (project code, ours): a host-side tone/tint pass on the final output image, never on a game render target.
//
//   SNAP_LOOK=<preset>[:<intensity>]   preset neutral | vivid | warm | cool, intensity 0..1.5 (default 1; 0 or neutral = off)
//
// snap_look_present() is called by the NVN present path once per present, after the scene has been blitted to the swapchain image and before
// the overlay (analysis/mods/nvn_hook_requests.md has the diff). The image is in TRANSFER_DST_OPTIMAL and must be left there. The pass copies it
// to a scratch image and redraws it through a fragment shader (GLSL in shaders/, SPIR-V in look_spv.h). The photo capture and the Evaluation
// pass read the game's own render targets, which this never touches, so scoring and saved photos are unchanged.
#include <windows.h>

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "../../runtime/ui/ui_present.h"
#include "look_spv.h"

namespace {

struct Look { float gain[3], sat, contrast; };
bool g_parsed, g_on;
float g_gain[3] = {1, 1, 1}, g_sat = 1, g_con = 1, g_t = 1;

void parse() {
  g_parsed = true;
  const char* e = getenv("SNAP_LOOK");
  if (!e || !*e) return;
  std::string s = e, name = s;
  float t = 1.0f;
  size_t c = s.find(':');
  if (c != std::string::npos) { name = s.substr(0, c); t = (float)atof(s.c_str() + c + 1); }
  for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
  if (t < 0) t = 0;
  if (t > 1.5f) t = 1.5f;
  if (name == "vivid") { g_sat = 1.35f; g_con = 1.10f; }
  else if (name == "warm") { g_gain[0] = 1.07f; g_gain[1] = 1.0f; g_gain[2] = 0.88f; g_sat = 1.06f; g_con = 1.03f; }
  else if (name == "cool") { g_gain[0] = 0.90f; g_gain[1] = 1.0f; g_gain[2] = 1.09f; g_sat = 1.04f; g_con = 1.03f; }
  else return;   // neutral or unknown: off
  g_t = t;
  g_on = t > 0.001f;
  fprintf(stderr, "look: SNAP_LOOK=%s intensity %.2f\n", name.c_str(), t);
}

struct State {
  VkDevice dev = VK_NULL_HANDLE;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  VkExtent2D ext{};
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  VkRenderPass rp = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkSampler samp = VK_NULL_HANDLE;
  VkImage img = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  struct Fb { VkImageView v; VkFramebuffer f; };
  std::map<VkImage, Fb> fbs;
  bool failed = false;
} S;

uint32_t findMem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return 0xffffffffu;
}

void dropSized() {
  for (auto& kv : S.fbs) { vkDestroyFramebuffer(S.dev, kv.second.f, nullptr); vkDestroyImageView(S.dev, kv.second.v, nullptr); }
  S.fbs.clear();
  if (S.view) vkDestroyImageView(S.dev, S.view, nullptr);
  if (S.img) vkDestroyImage(S.dev, S.img, nullptr);
  if (S.mem) vkFreeMemory(S.dev, S.mem, nullptr);
  S.view = VK_NULL_HANDLE; S.img = VK_NULL_HANDLE; S.mem = VK_NULL_HANDLE;
}

bool build(const SnapUiFrame* f) {
  // called with the device idle for our resources only: the previous frames' command buffers may still use the old scratch image, so wait.
  if (S.dev) vkDeviceWaitIdle(S.dev);
  if (S.dev && (S.dev != f->device || S.fmt != f->format)) {
    dropSized();
    if (S.pipe) vkDestroyPipeline(S.dev, S.pipe, nullptr);
    if (S.rp) vkDestroyRenderPass(S.dev, S.rp, nullptr);
    S.pipe = VK_NULL_HANDLE; S.rp = VK_NULL_HANDLE;
  }
  S.dev = f->device;
  VkDevice d = S.dev;
  if (!S.rp) {
    VkAttachmentDescription a{};
    a.format = f->format; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAttachmentReference r{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp;
    if (vkCreateRenderPass(d, &ci, nullptr, &S.rp) != VK_SUCCESS) return false;
    S.fmt = f->format;
  }
  if (!S.samp) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &si, nullptr, &S.samp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1; dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(d, &dl, nullptr, &S.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &S.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &S.pl) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dp, nullptr, &S.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = S.pool; da.descriptorSetCount = 1; da.pSetLayouts = &S.dsl;
    if (vkAllocateDescriptorSets(d, &da, &S.set) != VK_SUCCESS) return false;
  }
  if (!S.pipe) {
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof kLook_vert; sm.pCode = kLook_vert;
    if (vkCreateShaderModule(d, &sm, nullptr, &vs) != VK_SUCCESS) return false;
    sm.codeSize = sizeof kLook_frag; sm.pCode = kLook_frag;
    if (vkCreateShaderModule(d, &sm, nullptr, &fs) != VK_SUCCESS) return false;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = S.pl; gp.renderPass = S.rp; gp.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &S.pipe);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (r != VK_SUCCESS) return false;
  }
  if (!S.img || S.ext.width != f->extent.width || S.ext.height != f->extent.height || S.sc != f->swapchain) {
    dropSized();
    S.ext = f->extent; S.sc = f->swapchain;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = f->format; ic.extent = {f->extent.width, f->extent.height, 1};
    ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d, &ic, nullptr, &S.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d, S.img, &mr);
    uint32_t mt = findMem(f->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == 0xffffffffu) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(d, &ai, nullptr, &S.mem) != VK_SUCCESS || vkBindImageMemory(d, S.img, S.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = S.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d, &vc, nullptr, &S.view) != VK_SUCCESS) return false;
    VkDescriptorImageInfo di{S.samp, S.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = S.set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
  }
  return true;
}

void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

extern "C" void snap_look_present(const SnapUiFrame* f) {
  if (!g_parsed) parse();
  if (!g_on || S.failed || !f) return;
  static bool needBuild = true;
  if (needBuild || S.dev != f->device || S.fmt != f->format || S.ext.width != f->extent.width || S.ext.height != f->extent.height || S.sc != f->swapchain) {
    if (!build(f)) { S.failed = true; fprintf(stderr, "look: setup failed, look disabled\n"); return; }
    needBuild = false;
  }
  VkCommandBuffer cb = f->cb;
  // framebuffer for this swapchain image (created on first use)
  auto it = S.fbs.find(f->image);
  if (it == S.fbs.end()) {
    State::Fb fb{};
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = f->image; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(S.dev, &vc, nullptr, &fb.v) != VK_SUCCESS) { S.failed = true; return; }
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = S.rp; fi.attachmentCount = 1; fi.pAttachments = &fb.v; fi.width = f->extent.width; fi.height = f->extent.height; fi.layers = 1;
    if (vkCreateFramebuffer(S.dev, &fi, nullptr, &fb.f) != VK_SUCCESS) { vkDestroyImageView(S.dev, fb.v, nullptr); S.failed = true; return; }
    it = S.fbs.emplace(f->image, fb).first;
  }
  // 1. swapchain image TRANSFER_DST -> TRANSFER_SRC, scratch -> TRANSFER_DST, copy
  barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  barrier(cb, S.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageCopy cp{};
  cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  cp.extent = {f->extent.width, f->extent.height, 1};
  vkCmdCopyImage(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  barrier(cb, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
  // 2. redraw through the look shader; the render pass leaves the image in TRANSFER_DST_OPTIMAL
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = S.rp; rb.framebuffer = it->second.f; rb.renderArea = {{0, 0}, f->extent};
  vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl, 0, 1, &S.set, 0, nullptr);
  VkViewport vp{0, 0, (float)f->extent.width, (float)f->extent.height, 0, 1};
  VkRect2D sc{{0, 0}, f->extent};
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetScissor(cb, 0, 1, &sc);
  const bool srgb = f->format == VK_FORMAT_B8G8R8A8_SRGB || f->format == VK_FORMAT_R8G8B8A8_SRGB;
  float pc[8] = {g_gain[0], g_gain[1], g_gain[2], 1.0f, g_sat, g_con, g_t, srgb ? 1.0f : 0.0f};
  vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRenderPass(cb);
}
