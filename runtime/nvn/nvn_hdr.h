// SPDX-License-Identifier: MIT
// HDR10 output (owner-approved, off by default: SNAP_HDR=hdr10|scrgb). Project code, ours.
//
// The NVN layer's present step normally blits the game's window texture into an SDR swapchain. With SNAP_HDR=hdr10|scrgb and a display/driver that
// offers A2B10G10R10 + HDR10_ST2084 (VK_EXT_swapchain_colorspace), the swapchain is created in that format, VK_EXT_hdr_metadata is set, and
// this file draws the frame through a PQ pass instead of the blit. Photo capture, thumbnails and scoring read the game's own render
// targets and never see this output. Settings (env): SNAP_HDR=hdr10|scrgb, SNAP_HDR_PEAK_NITS (default: the display MaxLuminance, else 1000), SNAP_HDR_PAPER_NITS
// (default 200), SNAP_HDR_EXPAND (0..1, default 0.3).
#pragma once
#include <vulkan/vulkan.h>
#include "nvn_vkcount.h"

#include <vector>

namespace snap_hdr {

bool Requested();            // SNAP_HDR=hdr10|scrgb
float Peak();
float Paper();
const char* ModeName();      // "itm" or "guided"

// Instance/device extension helpers: append VK_EXT_swapchain_colorspace / VK_EXT_hdr_metadata when requested and available.
void AppendInstanceExt(std::vector<const char*>& ext);
void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& ext);
void LoadDeviceFuncs(VkDevice dev);

// Chooses the HDR10 surface format. Logs a one-line reason and returns false when the surface does not offer it (caller falls back to SDR).
bool PickFormat(const VkSurfaceFormatKHR* fm, uint32_t n, VkSurfaceFormatKHR* out, void* hwnd);   // scRGB first if SNAP_HDR=scrgb, then HDR10
bool IsScrgb();   // the active swapchain is scRGB (RGBA16F), else A2B10G10R10 PQ
void SetMetadata(VkDevice dev, VkSwapchainKHR sc);   // after vkCreateSwapchainKHR

struct Frame {
  VkPhysicalDevice phys; VkDevice device; VkCommandBuffer cb;
  VkImage dst; VkFormat dstFmt; VkExtent2D dstExt; VkSwapchainKHR sc;   // dst: UNDEFINED in, TRANSFER_DST_OPTIMAL out
  VkImage src; VkFormat srcFmt; uint32_t srcW, srcH;                    // src: the SDR scratch image (TRANSFER_DST_OPTIMAL in, SHADER_READ_ONLY out)
  int sx, sy, sw, sh;                                                   // source rect (pixels)
  int dx, dy, dw, dh;                                                   // destination rect (pixels, letterboxed)
  VkImage layer;                                                        // overlay layer from PrepScratch (TRANSFER_DST_OPTIMAL in, SHADER_READ_ONLY out)
};
// SDR scratch images for the HDR path: `sdr` (RGBA8 sRGB, extent of the swapchain) takes the scene blit and SNAP_LOOK; `layer` (RGBA8 UNORM, cleared to
// transparent) takes the F1 overlay. Both come back in TRANSFER_DST_OPTIMAL, the layout snap_look_present/snap_ui_present expect.
struct Scratch { VkImage sdr; VkFormat sdrFmt; VkImage layer; VkFormat layerFmt; int dbg; };
bool PrepScratch(VkCommandBuffer cb, VkPhysicalDevice phys, VkDevice dev, VkExtent2D ext, Scratch* out);
bool Record(const Frame& f);   // false = setup failed (the caller should fall back to the blit)

// Debug dump: writes <base>.bmp (SDR preview of the PQ image) and <base>_nits.bmp (highlight map) from a copy of the swapchain image
// (A2B10G10R10 words) and logs luminance statistics.
void DumpPQ(const char* base, const void* px, uint32_t w, uint32_t h);   // px: A2B10G10R10 words, or RGBA16F texels when IsScrgb()

}  // namespace snap_hdr
