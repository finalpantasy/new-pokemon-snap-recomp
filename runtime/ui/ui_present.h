// SPDX-License-Identifier: MIT
// The one call from the NVN present path into the overlay. runtime/nvn includes only this header.
#pragma once
#include <vulkan/vulkan.h>

struct SnapUiFrame {
  VkInstance instance;
  VkPhysicalDevice phys;
  VkDevice device;
  VkQueue queue;                 // the queue that is being recorded for (queueMutex is held by the caller)
  uint32_t family;
  VkCommandBuffer cb;            // recording
  VkImage image;                 // swapchain image, currently in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; leave it in that layout
  VkFormat format;
  VkExtent2D extent;             // native output resolution
  VkSwapchainKHR swapchain;      // changes when the swapchain is recreated (the overlay drops its cached image views)
  uint32_t image_count;
  void* hwnd;                    // the game window
};

// Records the overlay on top of the finished scene blit (nothing when the menu is closed and there is no notice to show).
// Called once per present on the thread that presents. Defined in runtime/ui/ui_overlay.cpp.
extern "C" void snap_ui_present(const SnapUiFrame* frame);
