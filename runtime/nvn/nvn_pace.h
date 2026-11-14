// Frame pacing (2026-11-13, pace worker; analysis/perf/pacing.md). One frame clock drives both the host frame limiter and the
// guest nn::vi vsync event, so the two can no longer drift apart. SNAP_PACE=legacy restores the old limiter (steady 60 Hz
// schedule, 1.5 ms spin) and the free-running 16667 us vsync thread.
//
// Model (default "display"):
//  - Refresh R of the monitor the window is on (QueryDisplayConfig, exact rational; SNAP_PACE_REFRESH=<hz> overrides).
//  - If R is within 0.6% of a whole multiple k of the target rate (60, 59.94, 120, 119.88, 240 Hz for 60 fps), the clock is
//    "locked": frame period = k vblanks, phase-locked to the real vblank through VK_KHR_present_wait. Each frame is released
//    half a vblank after the vblank before the one it should show on, so jitter on either side never moves it to another
//    vblank, and the FIFO queue never holds more than one frame (no queue build-up at 59.94 Hz, no 1-3 vblank judder at 120).
//  - Otherwise (144, 165, 75 Hz...) the clock is "free": the period is the target on the steady clock (VRR shows it as is;
//    a fixed-refresh display shows the unavoidable 2/3-vblank cadence). present_wait still caps the queue at one frame.
//  - No present_wait (unsupported, minimized, hidden desktop, timeouts): the same grid on the steady clock.
//  - Waits: high-resolution waitable timer, 0.3 ms final spin (Sleep(1) + timeBeginPeriod when the timer is missing).
//  - SNAP_PACE_LOG=<file.csv>: one row per present (release interval, waits, acquire/present times, display interval, vsync ticks).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace snap_pace {

bool Legacy();   // SNAP_PACE=legacy
bool LogOn();    // SNAP_PACE_LOG set

// Device setup: adds VK_KHR_present_id + VK_KHR_present_wait (when offered and not legacy) and chains their feature structs after f11.
void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& dext, VkPhysicalDeviceVulkan11Features* f11);
void DeviceCreated(VkDevice dev);
void SwapchainCreated(VkSwapchainKHR sc, void* hwnd);
// Present mode of the new swapchain is IMMEDIATE (SNAP_VSYNC=off): locked releases go right after the vblank, not mid-frame.
void SetImmediate(bool on);

// Frame limiter, called before the present. basePeriodNs = presentInterval / paceHz (and the frame cap), 0 = unlimited;
// vsyncNs = 1 / paceHz (the guest vsync period before it is fitted to the display).
void Release(void* hwnd, VkSwapchainKHR sc, int64_t basePeriodNs, int64_t vsyncNs);
void PaceMark(bool end);   // SNAP_PACE_LOG: around the limiter in nvn_vk.cpp Pace() (both models)

// Present: chain the returned struct (or nullptr) into VkPresentInfoKHR::pNext, then report the result.
const void* PresentChain(VkSwapchainKHR sc);
void Presented(VkSwapchainKHR sc, VkResult r);

// SNAP_PACE_LOG timestamps inside the present (no-ops when the log is off).
enum Mark { M_ACQ_BEGIN = 0, M_ACQ_END, M_QP_BEGIN, M_QP_END, M_COUNT };
void Stamp(Mark m);

}  // namespace snap_pace
