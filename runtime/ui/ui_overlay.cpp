// SPDX-License-Identifier: MIT
// In-game overlay core: Dear ImGui (Vulkan + Win32 backends) drawn on top of the finished scene blit at native output resolution.
//
//  - snap_ui_present() is the only entry from the NVN layer (runtime/nvn/nvn_vk.cpp, PresentWindow). It runs on the presenting
//    thread, records into the present command buffer and leaves the swapchain image in TRANSFER_DST for the caller's final barrier.
//  - The game window (a plain HWND owned by host/boot.cpp) is subclassed once: F1 toggles the menu, the wheel feeds the input hub,
//    the cursor is hidden/released per the hub's policy and the rest goes to ImGui while the menu is open.
//  - While the menu is open the input hub reports an idle pad to the game; the menu reads keyboard/mouse through ImGui's Win32
//    backend and the pad through the hub (SDL3 + XInput merged), so every controller the game accepts also drives the menu.
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"
#include "ui_font.h"
#include "ui_internal.h"
#include "ui_present.h"
#include "ui_theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace snap::ui {

Theme& theme() {
  static Theme t;
  return t;
}

namespace {

using hle::input::hub;

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Core {
  std::recursive_mutex mu;             // ImGui context (recursive: ImGui_ImplWin32_NewFrame can re-enter the window procedure on the presenting thread): the window thread (messages) and the present thread (frames)
  std::atomic<bool> open{false};
  std::atomic<bool> modal{false};      // shader prompt / progress screen
  bool was_modal = false;
  std::atomic<bool> imgui_ready{false};
  HWND hwnd = nullptr;
  WNDPROC orig = nullptr;
  bool was_open = false;
  // Vulkan
  VkDevice device = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::unordered_map<VkImage, VkImageView> views;
  bool vk_ready = false, srgb = false, failed = false;
  // style
  float applied_scale = 0;
  // notice
  uint64_t seen_serial = 0;
  double toast_until = 0;
};
Core g;

// ---- window subclass ------------------------------------------------------------------------------------------------
LRESULT ui_wndproc_impl(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (w == VK_F1) {
        if (!(l & (1 << 30)) && !g.modal.load()) overlay_set_open(!g.open.load());   // key-down edge only, not the auto-repeat
        return 0;
      }
      break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (w == VK_F1) return 0;
      break;
    case WM_MOUSEWHEEL:
      hle::input::post_wheel(GET_WHEEL_DELTA_WPARAM(w));
      break;
    case WM_KILLFOCUS:
    case WM_CLOSE:
    case WM_DESTROY:
      ClipCursor(nullptr);   // never leave the cursor confined to a window that is going away or inactive
      break;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE) ClipCursor(nullptr);
      break;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT && hub().cursor_hidden() && !g.open.load() && !g.modal.load()) { SetCursor(nullptr); return TRUE; }
      break;
    default: break;
  }
  if ((g.open.load() || g.modal.load()) && g.imgui_ready.load()) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return TRUE;
  }
  return CallWindowProcW(g.orig, h, m, w, l);
}

// No host exception may leave a window procedure (the OS turns it into STATUS_FATAL_USER_CALLBACK_EXCEPTION and kills the process).
LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  try {
    return ui_wndproc_impl(h, m, w, l);
  } catch (const std::exception& e) {
    static int n = 0;
    if (n++ < 20) hle::log("ui: exception in the window procedure (msg 0x%x): %s", m, e.what());
  } catch (...) {
    static int n = 0;
    if (n++ < 20) hle::log("ui: unknown exception in the window procedure (msg 0x%x)", m);
  }
  return g.orig ? CallWindowProcW(g.orig, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void ensure_window(HWND h) {
  if (!h || g.hwnd == h) return;
  g.hwnd = h;
  g.orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ui_wndproc)));
  hle::log("ui: overlay hooked into the game window (F1 or hold Back+Start for 0.5 s opens the controls menu)");
}

// ---- colors: the swapchain is often an sRGB format, which would brighten straight ImGui colors; pre-convert them
float lin(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); }
ImVec4 fb(const ImVec4& c) { return g.srgb ? ImVec4(lin(c.x), lin(c.y), lin(c.z), c.w) : c; }

void apply_style(float scale) {
  const Theme& t = theme();
  ImGuiStyle s;
  ImGui::StyleColorsLight(&s);
  s.WindowRounding = t.window_round;
  s.ChildRounding = s.FrameRounding = s.PopupRounding = s.GrabRounding = s.TabRounding = t.rounding;
  s.ScrollbarRounding = t.rounding;
  s.WindowPadding = ImVec2(t.window_pad, t.window_pad);
  s.FramePadding = ImVec2(t.frame_pad_x, t.frame_pad_y);
  s.ItemSpacing = ImVec2(t.item_spacing, t.item_spacing);
  s.ItemInnerSpacing = ImVec2(t.item_spacing * 0.75f, t.item_spacing * 0.5f);
  s.WindowBorderSize = 2.0f;
  s.TabBarBorderSize = 0.0f;
  s.FrameBorderSize = 0.0f;
  s.FrameBorderSize = 0.0f;
  s.ScrollbarSize = 14.0f;
  s.GrabMinSize = 16.0f;
  s.CellPadding = ImVec2(6.0f, 4.0f);
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = fb(t.text);
  c[ImGuiCol_TextDisabled] = fb(t.text_dim);
  c[ImGuiCol_WindowBg] = fb(t.panel);
  c[ImGuiCol_ChildBg] = fb(t.panel_alt);
  c[ImGuiCol_PopupBg] = fb(t.panel);
  c[ImGuiCol_Border] = fb(t.border);
  c[ImGuiCol_FrameBg] = fb(t.slot);
  c[ImGuiCol_FrameBgHovered] = fb(t.slot_hover);
  c[ImGuiCol_FrameBgActive] = fb(t.accent);
  c[ImGuiCol_Button] = fb(t.slot);
  c[ImGuiCol_ButtonHovered] = fb(t.slot_hover);
  c[ImGuiCol_ButtonActive] = fb(t.accent);
  c[ImGuiCol_Header] = fb(t.slot);
  c[ImGuiCol_HeaderHovered] = fb(t.slot_hover);
  c[ImGuiCol_HeaderActive] = fb(t.accent);
  c[ImGuiCol_CheckMark] = fb(t.title);
  c[ImGuiCol_SliderGrab] = fb(t.accent);
  c[ImGuiCol_SliderGrabActive] = fb(t.accent_hover);
  c[ImGuiCol_Separator] = fb(t.border);
  c[ImGuiCol_Tab] = fb(t.slot);
  c[ImGuiCol_TabHovered] = fb(t.slot_hover);
  c[ImGuiCol_TabSelected] = fb(t.accent);
  c[ImGuiCol_TabSelectedOverline] = fb(t.accent_hover);   // orange line under the yellow tab
  c[ImGuiCol_TabDimmed] = fb(t.slot);
  c[ImGuiCol_TabDimmedSelected] = fb(t.accent);
  c[ImGuiCol_TableHeaderBg] = fb(t.panel_alt);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0.4f, 0.3f, 0.2f, g.srgb ? 0.025f : 0.05f);
  c[ImGuiCol_TableBorderLight] = fb(ImVec4(t.border.x, t.border.y, t.border.z, 0.5f));
  c[ImGuiCol_TableBorderStrong] = fb(t.border);
  c[ImGuiCol_NavCursor] = fb(t.accent_hover);
  c[ImGuiCol_TextSelectedBg] = fb(ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.5f));
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
  c[ImGuiCol_ScrollbarBg] = fb(t.panel);
  c[ImGuiCol_ScrollbarGrab] = fb(t.slot_hover);
  s.ScaleAllSizes(scale);
  s.FontSizeBase = t.font_px;
  s.FontScaleMain = scale;
  ImGui::GetStyle() = s;
  g.applied_scale = scale;
}

bool init_imgui(const SnapUiFrame* f) {
  if (g.failed) return false;
  if (g.imgui_ready.load()) return g.format == f->format;   // a format change after init is not supported: no overlay then
  std::lock_guard<std::recursive_mutex> lk(g.mu);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  g.format = f->format;
  g.srgb = f->format == VK_FORMAT_B8G8R8A8_SRGB || f->format == VK_FORMAT_R8G8B8A8_SRGB;
  g.device = f->device;
  if (!ImGui_ImplWin32_Init(f->hwnd)) { hle::log("ui: ImGui_ImplWin32_Init failed"); g.failed = true; ImGui::DestroyContext(); return false; }
  ImGui_ImplVulkan_InitInfo ii{};
  ii.ApiVersion = VK_API_VERSION_1_3;
  ii.Instance = f->instance;
  ii.PhysicalDevice = f->phys;
  ii.Device = f->device;
  ii.QueueFamily = f->family;
  ii.Queue = f->queue;
  ii.DescriptorPoolSize = 16;
  ii.MinImageCount = 2;
  ii.ImageCount = std::max(4u, f->image_count + 2);   // frames the present path can keep in flight
  ii.UseDynamicRendering = true;
  ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  static VkFormat color_format;
  color_format = f->format;
  ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
  if (!ImGui_ImplVulkan_Init(&ii)) { hle::log("ui: ImGui_ImplVulkan_Init failed"); g.failed = true; ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return false; }
  g.vk_ready = true;
  // font: the game's own UI font decoded from the user's extracted files at run time (ui_font.h; never copied into the repo), else Segoe UI
  // from the Windows fonts folder when present (system file, not shipped), else the embedded vector font
  bool font_ok = false;
  {
    static std::vector<unsigned char> game_font;   // must outlive the atlas
    if (game_font_ttf(game_font)) {
      ImFontConfig fc;
      fc.FontDataOwnedByAtlas = false;
      font_ok = io.Fonts->AddFontFromMemoryTTF(game_font.data(), (int)game_font.size(), theme().font_px, &fc) != nullptr;
      if (font_ok) hle::log("ui: using the game's UI font (%s) decoded from the extracted files", game_font_family());
    }
  }
  if (!font_ok)
  if (const char* root = std::getenv("SystemRoot")) {
    std::string path = std::string(root) + "\\Fonts\\" + theme().font_file;
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) font_ok = io.Fonts->AddFontFromFileTTF(path.c_str(), theme().font_px) != nullptr;
  }
  if (!font_ok) io.Fonts->AddFontDefaultVector();
  apply_style(1.0f);
  g.imgui_ready = true;
  hle::log("ui: ImGui %s ready, swapchain format %d%s", IMGUI_VERSION, (int)f->format, g.srgb ? " (sRGB, colors pre-converted)" : "");
  return true;
}

VkImageView view_for(const SnapUiFrame* f) {
  if (g.swapchain != f->swapchain) {   // swapchain recreated (the present path waited for the device to go idle): drop old views
    for (auto& kv : g.views) vkDestroyImageView(f->device, kv.second, nullptr);
    g.views.clear();
    g.swapchain = f->swapchain;
  }
  auto it = g.views.find(f->image);
  if (it != g.views.end()) return it->second;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = f->image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = f->format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v = VK_NULL_HANDLE;
  if (vkCreateImageView(f->device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
  g.views[f->image] = v;
  return v;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

// Gamepad navigation from the hub's merged pad state (SDL3 + XInput), so any controller the game accepts drives the menu.
void feed_gamepad(ImGuiIO& io, bool enabled) {
  using namespace hle::input;
  RawInput r = hub().raw();
  const bool on = enabled && r.focused;
  auto btn = [&](ImGuiKey k, bool v) { io.AddKeyEvent(k, on && v); };
  btn(ImGuiKey_GamepadFaceDown, r.pad.test(kPadSouth));
  btn(ImGuiKey_GamepadFaceRight, r.pad.test(kPadEast));
  btn(ImGuiKey_GamepadFaceLeft, r.pad.test(kPadWest));
  btn(ImGuiKey_GamepadFaceUp, r.pad.test(kPadNorth));
  btn(ImGuiKey_GamepadDpadUp, r.pad.test(kPadDUp));
  btn(ImGuiKey_GamepadDpadDown, r.pad.test(kPadDDown));
  btn(ImGuiKey_GamepadDpadLeft, r.pad.test(kPadDLeft));
  btn(ImGuiKey_GamepadDpadRight, r.pad.test(kPadDRight));
  btn(ImGuiKey_GamepadL1, r.pad.test(kPadLShoulder));
  btn(ImGuiKey_GamepadR1, r.pad.test(kPadRShoulder));
  btn(ImGuiKey_GamepadL3, r.pad.test(kPadLStick));
  btn(ImGuiKey_GamepadR3, r.pad.test(kPadRStick));
  auto analog = [&](ImGuiKey k, float v) {
    const float dz = 0.25f;
    float a = on && v > dz ? std::min(1.0f, (v - dz) / (1.0f - dz)) : 0.0f;
    io.AddKeyAnalogEvent(k, a > 0.0f, a);
  };
  analog(ImGuiKey_GamepadLStickLeft, -r.lx);
  analog(ImGuiKey_GamepadLStickRight, r.lx);
  analog(ImGuiKey_GamepadLStickUp, r.ly);
  analog(ImGuiKey_GamepadLStickDown, -r.ly);
  analog(ImGuiKey_GamepadRStickLeft, -r.rx);
  analog(ImGuiKey_GamepadRStickRight, r.rx);
  analog(ImGuiKey_GamepadRStickUp, r.ry);
  analog(ImGuiKey_GamepadRStickDown, -r.ry);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, on && r.lt > 0.3f, on ? r.lt : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, on && r.rt > 0.3f, on ? r.rt : 0.0f);
}

}  // namespace

bool overlay_open() { return g.open.load(); }
void* overlay_hwnd() { return g.hwnd; }

// Window thread (F1) and present thread (chord, scripted test) both call this.
void overlay_set_open(bool open) {
  if (g.open.exchange(open) == open) return;
  hle::input::hub().set_menu_open(open || g.modal.load());
  if (open) ClipCursor(nullptr);   // the cursor must reach the menu, not stay confined by the game
  hle::log("ui: controls menu %s", open ? "opened" : "closed");
}

void overlay_set_modal(bool on) {
  if (g.modal.exchange(on) == on) return;
  hle::input::hub().set_menu_open(on || g.open.load());
  if (on) ClipCursor(nullptr);
  hle::log("ui: shader screen %s", on ? "shown" : "closed");
}

}  // namespace snap::ui

static void ui_present_impl(const SnapUiFrame* f);
// Exceptions never reach the NVN present path: log, drop the frame's overlay and keep the game running.
extern "C" void snap_ui_present(const SnapUiFrame* f) {
  static int errors = 0;
  try {
    ui_present_impl(f);
  } catch (const std::exception& e) {
    if (errors++ < 20) hle::log("ui: exception in the overlay frame: %s", e.what());
  } catch (...) {
    if (errors++ < 20) hle::log("ui: unknown exception in the overlay frame");
  }
}

static void ui_present_impl(const SnapUiFrame* f) {
  using namespace snap::ui;
  auto& hubr = hle::input::hub();
  ensure_window(static_cast<HWND>(f->hwnd));
  const double now = now_s();
  test_tick(now);
  const bool shader = shader_screen_active(now);
  overlay_set_modal(shader);
  if (!shader && g.was_modal) shader_screen_closed();
  g.was_modal = shader;
  if (hubr.consume_menu_toggle() && !shader) overlay_set_open(!g.open.load());
  const bool open = g.open.load() && !shader;
  if (open && !g.was_open) menu_opened();
  if (!open && g.was_open) menu_closed();
  g.was_open = open;
  menu_tick_save();

  // a load or save problem (invalid input.json, migration) shows as a notice at the bottom for a few seconds
  const uint64_t serial = hubr.message_serial();
  if (serial != g.seen_serial) {
    g.seen_serial = serial;
    if (!hubr.message().empty()) g.toast_until = now + 12.0;
  }
  const bool toast = !open && now < g.toast_until;
  if (!open && !shader && !toast) return;
  if (!init_imgui(f)) return;
  if (open || shader) hubr.poll(8);   // keeps rebinding and the live readout fresh even when the game is not polling

  const ImVec2 size(static_cast<float>(f->extent.width), static_cast<float>(f->extent.height));
  ImDrawData* dd = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    const float scale = std::clamp(hubr.mapping().ui_scale, 0.5f, 3.0f) * std::max(1.0f, size.y / 1080.0f);
    if (std::fabs(scale - g.applied_scale) > 0.001f) apply_style(scale);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = size;
    feed_gamepad(io, (open || shader) && !hubr.capturing());
    ImGui::NewFrame();
    if (shader) {
      draw_shader_screen(size);
    } else if (open) {
      bool close = false;
      draw_menu(size, &close);
      if (close) overlay_set_open(false);
    } else if (!draw_toast(size, now)) {
      g.toast_until = 0;
    }
    ImGui::Render();
    dd = ImGui::GetDrawData();
  }
  if (!dd || dd->CmdListsCount == 0) return;
  VkImageView view = view_for(f);
  if (!view) return;
  barrier(f->cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = view;
  ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // keep the scene
  ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, f->extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &ca;
  vkCmdBeginRendering(f->cb, &ri);
  ImGui_ImplVulkan_RenderDrawData(dd, f->cb);
  vkCmdEndRendering(f->cb);
  barrier(f->cb, f->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}
