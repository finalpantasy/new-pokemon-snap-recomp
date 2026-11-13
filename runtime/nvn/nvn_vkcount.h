// Vulkan object counters for the soak log (SNAP_SOAK_LOG, runtime/nvn/nvn_soak.cpp). Included right after <vulkan/vulkan.h> in
// nvn_internal.h and nvn_hdr.h: the macros below route the create/destroy calls of every nvn_*.cpp through counting wrappers
// (one relaxed atomic add per call; always compiled in, only read when SNAP_SOAK_LOG is set).
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>

namespace snapvk {
enum Kind { kImage, kBuffer, kMemory, kDescPool, kPipeline, kImageView, kSampler, kShaderModule, kKinds };
struct Counts {
  std::atomic<int64_t> made[kKinds];
  std::atomic<int64_t> freed[kKinds];
  std::atomic<int64_t> memBytesMade;   // vkAllocateMemory sizes (never decremented; vkFreeMemory has no size)
};
inline Counts& C() { static Counts c; return c; }
inline void Made(Kind k) { C().made[k].fetch_add(1, std::memory_order_relaxed); }
inline void Freed(Kind k, const void* h) { if (h) C().freed[k].fetch_add(1, std::memory_order_relaxed); }
}  // namespace snapvk

static inline VkResult snapvk_CreateImage(VkDevice d, const VkImageCreateInfo* i, const VkAllocationCallbacks* a, VkImage* o) {
  VkResult r = vkCreateImage(d, i, a, o); if (r == VK_SUCCESS) snapvk::Made(snapvk::kImage); return r; }
static inline void snapvk_DestroyImage(VkDevice d, VkImage h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kImage, (const void*)h); vkDestroyImage(d, h, a); }
static inline VkResult snapvk_CreateBuffer(VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* o) {
  VkResult r = vkCreateBuffer(d, i, a, o); if (r == VK_SUCCESS) snapvk::Made(snapvk::kBuffer); return r; }
static inline void snapvk_DestroyBuffer(VkDevice d, VkBuffer h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kBuffer, (const void*)h); vkDestroyBuffer(d, h, a); }
static inline VkResult snapvk_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks* a, VkDeviceMemory* o) {
  VkResult r = vkAllocateMemory(d, i, a, o);
  if (r == VK_SUCCESS) { snapvk::Made(snapvk::kMemory); snapvk::C().memBytesMade.fetch_add((int64_t)i->allocationSize, std::memory_order_relaxed); }
  return r; }
static inline void snapvk_FreeMemory(VkDevice d, VkDeviceMemory h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kMemory, (const void*)h); vkFreeMemory(d, h, a); }
static inline VkResult snapvk_CreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo* i, const VkAllocationCallbacks* a, VkDescriptorPool* o) {
  VkResult r = vkCreateDescriptorPool(d, i, a, o); if (r == VK_SUCCESS) snapvk::Made(snapvk::kDescPool); return r; }
static inline void snapvk_DestroyDescriptorPool(VkDevice d, VkDescriptorPool h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kDescPool, (const void*)h); vkDestroyDescriptorPool(d, h, a); }
static inline VkResult snapvk_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* i, const VkAllocationCallbacks* a, VkPipeline* o) {
  VkResult r = vkCreateGraphicsPipelines(d, c, n, i, a, o);
  if (r == VK_SUCCESS) for (uint32_t k = 0; k < n; k++) snapvk::Made(snapvk::kPipeline);
  return r; }
static inline VkResult snapvk_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* i, const VkAllocationCallbacks* a, VkPipeline* o) {
  VkResult r = vkCreateComputePipelines(d, c, n, i, a, o);
  if (r == VK_SUCCESS) for (uint32_t k = 0; k < n; k++) snapvk::Made(snapvk::kPipeline);
  return r; }
static inline void snapvk_DestroyPipeline(VkDevice d, VkPipeline h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kPipeline, (const void*)h); vkDestroyPipeline(d, h, a); }
static inline VkResult snapvk_CreateImageView(VkDevice d, const VkImageViewCreateInfo* i, const VkAllocationCallbacks* a, VkImageView* o) {
  VkResult r = vkCreateImageView(d, i, a, o); if (r == VK_SUCCESS) snapvk::Made(snapvk::kImageView); return r; }
static inline void snapvk_DestroyImageView(VkDevice d, VkImageView h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kImageView, (const void*)h); vkDestroyImageView(d, h, a); }
static inline VkResult snapvk_CreateSampler(VkDevice d, const VkSamplerCreateInfo* i, const VkAllocationCallbacks* a, VkSampler* o) {
  VkResult r = vkCreateSampler(d, i, a, o); if (r == VK_SUCCESS) snapvk::Made(snapvk::kSampler); return r; }
static inline void snapvk_DestroySampler(VkDevice d, VkSampler h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kSampler, (const void*)h); vkDestroySampler(d, h, a); }
static inline VkResult snapvk_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* i, const VkAllocationCallbacks* a, VkShaderModule* o) {
  VkResult r = vkCreateShaderModule(d, i, a, o); if (r == VK_SUCCESS) snapvk::Made(snapvk::kShaderModule); return r; }
static inline void snapvk_DestroyShaderModule(VkDevice d, VkShaderModule h, const VkAllocationCallbacks* a) {
  snapvk::Freed(snapvk::kShaderModule, (const void*)h); vkDestroyShaderModule(d, h, a); }

#define vkCreateImage snapvk_CreateImage
#define vkDestroyImage snapvk_DestroyImage
#define vkCreateBuffer snapvk_CreateBuffer
#define vkDestroyBuffer snapvk_DestroyBuffer
#define vkAllocateMemory snapvk_AllocateMemory
#define vkFreeMemory snapvk_FreeMemory
#define vkCreateDescriptorPool snapvk_CreateDescriptorPool
#define vkDestroyDescriptorPool snapvk_DestroyDescriptorPool
#define vkCreateGraphicsPipelines snapvk_CreateGraphicsPipelines
#define vkCreateComputePipelines snapvk_CreateComputePipelines
#define vkDestroyPipeline snapvk_DestroyPipeline
#define vkCreateImageView snapvk_CreateImageView
#define vkDestroyImageView snapvk_DestroyImageView
#define vkCreateSampler snapvk_CreateSampler
#define vkDestroySampler snapvk_DestroySampler
#define vkCreateShaderModule snapvk_CreateShaderModule
#define vkDestroyShaderModule snapvk_DestroyShaderModule
