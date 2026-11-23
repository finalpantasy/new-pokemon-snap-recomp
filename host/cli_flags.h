// SPDX-License-Identifier: GPL-3.0-or-later
// Player command-line flags of the game exe (docs/command-line.md). Header only and free of Windows and runtime includes, so
// tests/hle/cli_flags_test.cpp checks it on its own. host/boot.cpp main() calls parse() first, applies the result, and hands `rest`
// to the older handlers (save import/export, benchmark, shader preparation, the developer boot commands).
//
//   --play                        skip the launcher window and start the game; the process ends when the game closes
//   --safe-mode                   docs/safe-mode.md
//   --resume                      after a session that did not close, skip the title screen and menus back to where it was (docs/crash-resume.md)
//   --crash-prompt on|off         the "The game closed unexpectedly" question (SNAP_CRASH_PROMPT); this run only
//   --fullscreen | --borderless   borderless full-screen window on the chosen monitor (the game has no exclusive mode)
//   --windowed                    a normal window
//   --monitor <n>  --width <px>  --height <px>
//   --steam-deck                  the Steam Deck preset (host/steamdeck.h, docs/steam-deck.md): borderless 1280 x 800, handheld picture settings,
//                                 bigger HUD, Xbox button icons. Flags after it win (--set hud_scale=200, --width ...).
//   --pause-in-background on|off  --mute-in-background on|off  --frame-cap <fps>   the F1 / launcher Window rows (0 = no extra limit); this run only
//   --fps <n|unlocked|monitor|default>  frame rate target (SNAP_FPS, host/mods/highfps_core.h): any rate from 61 to 1000, unlocked, the monitor's
//                                 refresh, or the game's own 60; real frames, the game keeps its speed; this run only
//   --hdr-photos on|off           also write <name>.hdr.png (PQ, BT.2020) beside HD photo copies and screenshots; needs HDR output (docs/hdr-photos.md)
//   --vsync on|adaptive|fast|off  --aniso off|2|4|8|16   present mode and texture filtering (SNAP_VSYNC, SNAP_ANISO); this run only
//   --gpl auto|on|off             Vulkan graphics pipeline libraries (fast-linked pipelines, SNAP_GPL); this run only
//   --mods-readahead on|off       read a course's HD pack files into the file cache ahead of the loader (docs/mods.md); this run only
//   --distant-scenery off|high|ultra   farther model detail and draw distance, display only (SNAP_DISTANT_SCENERY, docs/draw-distance.md); this run only
//   --shader-workers <n|auto>     translator processes for shaders missing from the cache (SNAP_XLATE_WORKERS, docs/shader-prebuild.md); 1 = translate
//                                 inline one at a time (the old first launch); this run only
//   --update-check on|off  --update-channel stable|testing                            the launcher's update rows; this run only
//   --backup on|off  --backup-folder <folder>  --backup-keep <n>  --backup-include <saves,settings,photos>
//                                 the profile backups to a sync folder (docs/sync-backups.md): after the game closes, and with --backups now; this run only
//   --profile <name or id>        the profile to play (also the profile of --import-save / --export-save)
//   --set <key>=<value>           one settings key for this run only (repeatable); a name that starts with SNAP_ sets that variable.
//                                 Rows kept outside settings.cfg (window.json, audio.json, input.json) take their key from set_key_env() below.
//   --data-dir <folder>           one folder for saves, settings, mods, caches and logs
//   --no-prompt                   never open a window to ask a question: a game folder the exe cannot write to ends the start with exit code 3
//                                 and a message on stderr (docs/readonly-install.md). --play, the actions and the other commands behave this way already.
//   --update | --check-install | --report | --restore-backup | --backups | --upscalers | --hdtex | --mods
//                                 one launcher action without the launcher window (host/launcher.cpp run_action); the game does not start.
//                                 No action opens a dialog. --restore-backup, --backups, --upscalers, --hdtex and --mods take everything after them as
//                                 their own arguments (action_takes_args), so the other player flags go before them.
//   --help | -h | /?   --version
#pragma once
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "mods/highfps_core.h"
#include "steamdeck.h"

namespace snap_cli {

struct Parsed {
  bool help = false, version = false, play = false, safe = false, no_prompt = false;
  std::string data_dir, profile;
  std::string action;                                      // one launcher action (action_names) without its "--", or ""
  std::vector<std::string> action_args;                    // what followed an action that takes arguments (action_takes_args)
  std::vector<std::pair<std::string, std::string>> env;    // variables to set for this run (window flags, --set SNAP_X=v)
  std::vector<std::pair<std::string, std::string>> sets;   // --set key=value, in order
  std::vector<std::string> rest;                           // everything else, in order, for the older handlers
  std::string error;                                       // not empty: print it and exit 2
};

// --set keys for rows that settings.cfg does not hold: the window and audio files, the volumes and the controls mapping. Each turns into a
// variable the game reads at start (window_opts.cpp, audio_out.cpp, hle/hid.cpp), so the stored file is not changed. Volumes are 0 to 1.
inline const char* set_key_env(const std::string& k) {
  static const struct { const char* key; const char* env; } kMap[] = {
      {"win_mode", "SNAP_WINDOW_MODE"}, {"win_monitor", "SNAP_WINDOW_MONITOR"}, {"win_pause", "SNAP_WINDOW_PAUSE"}, {"win_mute", "SNAP_WINDOW_MUTE_BG"},
      {"frame_cap", "SNAP_WINDOW_FRAME_CAP"}, {"audio_output", "SNAP_AUDIO_OUTPUT"},
      {"master_volume", "SNAP_VOL_MASTER"}, {"music_volume", "SNAP_VOL_BGM"}, {"effects_volume", "SNAP_VOL_SE"}, {"voice_volume", "SNAP_VOL_VOICE"},
      {"face_layout", "SNAP_CTL_FACE_LAYOUT"}, {"swap_sticks", "SNAP_CTL_SWAP_STICKS"}, {"left_invert_y", "SNAP_CTL_LEFT_INVERT_Y"},
      {"right_invert_y", "SNAP_CTL_RIGHT_INVERT_Y"}, {"toggle_zl", "SNAP_CTL_TOGGLE_ZL"}, {"toggle_zr", "SNAP_CTL_TOGGLE_ZR"},
      {"mouse_look", "SNAP_CTL_MOUSE_LOOK"}, {"mouse_sensitivity", "SNAP_CTL_MOUSE_SENSITIVITY"}, {"mouse_invert_x", "SNAP_CTL_MOUSE_INVERT_X"},
      {"mouse_invert_y", "SNAP_CTL_MOUSE_INVERT_Y"}, {"mouse_touch", "SNAP_CTL_MOUSE_TOUCH"}, {"lock_cursor", "SNAP_CTL_LOCK_CURSOR"},
  };
  for (const auto& m : kMap) if (k == m.key) return m.env;
  return nullptr;
}

inline bool is_number(const std::string& s) {
  if (s.empty() || s.size() > 6) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

// "on"/"1"/"true"/"yes" -> "1", "off"/"0"/"false"/"no" -> "0", anything else -> "".
inline std::string on_off(const std::string& s) {
  if (s == "on" || s == "1" || s == "true" || s == "yes") return "1";
  if (s == "off" || s == "0" || s == "false" || s == "no") return "0";
  return "";
}

// Launcher actions that have a flag of their own: "--" + name.
inline const std::vector<std::string>& action_names() {
  static const std::vector<std::string> k = {"update", "check-install", "report", "restore-backup", "backups", "upscalers", "hdtex", "mods"};
  return k;
}

// These actions own every argument after them: --restore-backup [latest | <n>], --backups [list | now | auto | verify [<n>] | restore <n|latest> ...],
// --upscalers [<name> --accept-license <name>], --hdtex [the builder's hdtex options], --mods list|conflicts|order|move|enable|disable (docs/mods.md). Without arguments each one prints its state and changes nothing.
inline bool action_takes_args(const std::string& n) { return n == "restore-backup" || n == "backups" || n == "upscalers" || n == "hdtex" || n == "mods"; }

// "saves,settings,photos" (any subset, comma or space separated) -> the same list in lower case, or "" when a word is not one of them.
inline std::string backup_include(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find_first_of(", ;", i);
    if (j == std::string::npos) j = s.size();
    std::string w = s.substr(i, j - i);
    for (char& c : w) c = (char)std::tolower((unsigned char)c);
    i = j + 1;
    if (w.empty()) continue;
    if (w != "saves" && w != "settings" && w != "photos") return "";
    if (!out.empty()) out += ",";
    out += w;
  }
  return out;
}

inline Parsed parse(const std::vector<std::string>& args) {
  Parsed p;
  auto value = [&](size_t& i, std::string* out) {
    if (i + 1 >= args.size()) { p.error = args[i] + " needs a value"; return false; }
    *out = args[++i];
    return true;
  };
  for (size_t i = 0; i < args.size() && p.error.empty(); i++) {
    const std::string& a = args[i];
    std::string v;
    if (a == "--help" || a == "-h" || a == "/?") p.help = true;
    else if (a == "--version") p.version = true;
    else if (a == "--play") p.play = true;
    else if (a == "--safe-mode") p.safe = true;
    else if (a == "--no-prompt") p.no_prompt = true;
    else if (a == "--resume") p.env.push_back({"SNAP_CRASH_RESUME", "resume"});
    else if (a == "--fullscreen" || a == "--borderless") p.env.push_back({"SNAP_WINDOW_MODE", "borderless"});
    else if (a == "--windowed") p.env.push_back({"SNAP_WINDOW_MODE", "windowed"});
    else if (a == "--steam-deck") {
      for (auto& kv : snapdeck::env()) p.env.push_back(kv);
      for (const snapdeck::Kv& kv : snapdeck::cfg()) p.sets.push_back({kv.key, kv.val});
    }
    else if (a == "--pause-in-background" || a == "--mute-in-background" || a == "--update-check" || a == "--crash-prompt" || a == "--mods-readahead") {
      if (!value(i, &v)) break;
      const std::string b = on_off(v);
      if (b.empty()) { p.error = a + " needs on or off, got \"" + v + "\""; break; }
      p.env.push_back({a == "--pause-in-background" ? "SNAP_WINDOW_PAUSE" : a == "--mute-in-background" ? "SNAP_WINDOW_MUTE_BG" : a == "--crash-prompt" ? "SNAP_CRASH_PROMPT" : a == "--mods-readahead" ? "SNAP_MODS_READAHEAD" : "SNAP_UPDATE_CHECK", b});
    } else if (a == "--fps") {
      if (!value(i, &v)) break;
      if (highfps::parse(v.c_str()).kind == highfps::K_INVALID) { p.error = a + " needs a rate from 61 to 1000, unlocked, monitor or default, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_FPS", v});
    } else if (a == "--frame-cap") {
      if (!value(i, &v)) break;
      if (!is_number(v)) { p.error = a + " needs a whole number of frames per second (0 = no extra limit), got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_WINDOW_FRAME_CAP", v});
    } else if (a == "--hdr-photos") {   // HDR copies of HD photos and screenshots (runtime/nvn/nvn_hdrphoto.cpp, docs/hdr-photos.md); needs HDR output
      if (!value(i, &v)) break;
      const std::string b = on_off(v);
      if (b.empty()) { p.error = a + " needs on or off, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_HDR_PHOTOS", b});
    } else if (a == "--shiny") {   // rare shiny Pokemon (runtime/nvn/nvn_shiny.cpp): off, on (1 in 512) or 1 in <n>; --shiny 1 forces every Pokemon (test)
      if (!value(i, &v)) break;
      if (v == "off" || v == "0" || v == "no") p.env.push_back({"SNAP_SHINY", "0"});
      else if (v == "on" || v == "yes") p.env.push_back({"SNAP_SHINY", "1"});
      else if (is_number(v) && std::atol(v.c_str()) >= 1) { p.env.push_back({"SNAP_SHINY", "1"}); p.env.push_back({"SNAP_SHINY_ODDS", v}); }
      else { p.error = a + " needs off, on or a whole number n for 1 in n, got \"" + v + "\""; break; }
    } else if (a == "--vsync") {
      if (!value(i, &v)) break;
      if (v != "on" && v != "adaptive" && v != "fast" && v != "off") { p.error = a + " needs on, adaptive, fast or off, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_VSYNC", v});
    } else if (a == "--gpl") {
      if (!value(i, &v)) break;
      if (v == "1" || v == "yes") v = "on";
      if (v == "0" || v == "no") v = "off";
      if (v != "auto" && v != "on" && v != "off") { p.error = a + " needs auto, on or off, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_GPL", v});
    } else if (a == "--distant-scenery") {
      if (!value(i, &v)) break;
      if (v == "0" || v == "original" || v == "no") v = "off";
      if (v != "off" && v != "high" && v != "ultra") { p.error = a + " needs off, high or ultra, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_DISTANT_SCENERY", v});
    } else if (a == "--shader-workers") {   // runtime/nvn/nvn_shader.cpp XlateWorkers: background shader translation on the first launch
      if (!value(i, &v)) break;
      if (v == "auto") v = "0";
      if (!is_number(v) || std::atol(v.c_str()) > 64) { p.error = a + " needs auto or a number of translator processes from 1 to 64, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_XLATE_WORKERS", v});
    } else if (a == "--aniso") {
      if (!value(i, &v)) break;
      if (v == "off") v = "0";
      if (v != "0" && v != "1" && v != "2" && v != "4" && v != "8" && v != "16") { p.error = a + " needs off, 2, 4, 8 or 16, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_ANISO", v});
    } else if (a == "--backup") {
      if (!value(i, &v)) break;
      const std::string b = on_off(v);
      if (b.empty()) { p.error = a + " needs on or off, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_SYNC_BACKUP", b});
    } else if (a == "--backup-folder") {
      if (!value(i, &v)) break;
      if (v.empty()) { p.error = a + " needs a folder"; break; }
      p.env.push_back({"SNAP_SYNC_BACKUP_FOLDER", v});
    } else if (a == "--backup-keep") {
      if (!value(i, &v)) break;
      if (!is_number(v) || std::atol(v.c_str()) < 1 || std::atol(v.c_str()) > 100) { p.error = a + " needs a whole number from 1 to 100, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_SYNC_BACKUP_KEEP", v});
    } else if (a == "--backup-include") {
      if (!value(i, &v)) break;
      const std::string inc = backup_include(v);
      if (inc.empty()) { p.error = a + " needs some of saves, settings and photos (for example saves,settings), got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_SYNC_BACKUP_INCLUDE", inc});
    } else if (a == "--update-channel") {
      if (!value(i, &v)) break;
      if (v != "stable" && v != "testing") { p.error = a + " needs stable or testing, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_UPDATE_CHANNEL", v});
    }
    else if (a == "--monitor" || a == "--width" || a == "--height") {
      if (!value(i, &v)) break;
      if (!is_number(v)) { p.error = a + " needs a whole number, got \"" + v + "\""; break; }
      p.env.push_back({a == "--monitor" ? "SNAP_WINDOW_MONITOR" : a == "--width" ? "SNAP_WINDOW_W" : "SNAP_WINDOW_H", v});
    } else if (a == "--profile") {
      if (!value(i, &p.profile)) break;
    } else if (a == "--data-dir") {
      if (!value(i, &p.data_dir)) break;
      if (p.data_dir.empty()) p.error = "--data-dir needs a folder";
    } else if (a == "--set") {
      if (!value(i, &v)) break;
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) { p.error = "--set needs key=value, got \"" + v + "\""; break; }
      const std::string k = v.substr(0, eq), val = v.substr(eq + 1);
      if (k.compare(0, 5, "SNAP_") == 0) p.env.push_back({k, val});
      else if (const char* e = set_key_env(k)) p.env.push_back({e, val});
      else p.sets.push_back({k, val});
    } else {
      bool act = false;
      if (a.compare(0, 2, "--") == 0) for (const std::string& n : action_names()) if (a.compare(2, std::string::npos, n) == 0) act = true;
      if (!act) { p.rest.push_back(a); continue; }
      if (!p.action.empty()) { p.error = "only one of --" + p.action + " and " + a + " at a time"; break; }
      p.action = a.substr(2);
      if (action_takes_args(p.action)) {
        p.action_args.assign(args.begin() + (std::ptrdiff_t)i + 1, args.end());
        break;
      }
    }
  }
  if (p.error.empty() && !p.action.empty() && p.play) p.error = "--" + p.action + " does not start the game; leave out --play";
  if (p.error.empty() && !p.action.empty() && !p.rest.empty()) p.error = "--" + p.action + " cannot be used with " + p.rest[0];
  return p;
}

// The first "--" option in `rest` that no handler knows, or "". The lists are the options host/boot.cpp, host/benchmark.cpp and
// host/shaderprep.cpp read; an option added there must be added here.
inline std::string first_unknown(const std::vector<std::string>& rest) {
  static const char* const kTakesValue[] = {"--import-save", "--export-save", "--benchmark-view", "--courses", "--main", "--sdk", "--probe",
                                            "--iters", "--trace", "--timeout", "--stub"};
  static const char* const kPlain[] = {"--benchmark", "--benchmark-capped", "--benchmark-uncapped", "--prepare-shaders", "--force", "--visible",
                                       "--list", "--stop", "--libc", "--stop-after-init"};
  for (size_t i = 0; i < rest.size(); i++) {
    const std::string& a = rest[i];
    if (a.compare(0, 2, "--") != 0) continue;
    bool known = false;
    for (const char* k : kTakesValue) if (a == k) { known = true; i++; break; }
    if (!known) for (const char* k : kPlain) if (a == k) { known = true; break; }
    if (!known) return a;
  }
  return "";
}

// "builder_version": "<x>" out of config.json text, or "" (a development build has no config.json).
inline std::string version_from_config(const std::string& text) {
  const char* key = "\"builder_version\"";
  size_t k = text.find(key);
  if (k == std::string::npos) return "";
  size_t a = text.find('"', text.find(':', k + 17));
  if (a == std::string::npos) return "";
  size_t b = text.find('"', a + 1);
  if (b == std::string::npos) return "";
  return text.substr(a + 1, b - a - 1);
}

inline const char* help_text() {
  return "Snap Recomp\n"
         "\n"
         "  SnapRecomp.exe                    open the launcher\n"
         "  SnapRecomp.exe --play [options]   start the game without the launcher window\n"
         "\n"
         "Options:\n"
         "  --play                       skip the launcher window; the process ends when the game closes\n"
         "  --safe-mode                  mods, HD packs, enhanced effects and the shader cache off for this start\n"
         "  --fullscreen, --borderless   borderless full-screen window\n"
         "  --windowed                   normal window\n"
         "  --monitor <n>                monitor number, 0 is the first\n"
         "  --width <px> --height <px>   window size\n"
         "  --pause-in-background on|off  pause the game while its window is in the background\n"
         "  --mute-in-background on|off   mute the game while its window is in the background\n"
         "  --fps <n>|unlocked|monitor   frame rate target: any rate from 61 to 1000 (real frames, the game keeps its speed), unlocked,\n"
         "                               the monitor's refresh rate, or default (the game's own 60)\n"
         "  --frame-cap <fps>            frame rate limit, 0 = no extra limit\n"
         "  --vsync on|adaptive|fast|off present mode (FIFO, FIFO_RELAXED, MAILBOX, IMMEDIATE); a missing mode falls back to FIFO\n"
         "  --gpl auto|on|off            Vulkan graphics pipeline libraries (fast-linked pipelines); auto = on where the driver has fast linking\n"
         "  --aniso off|2|4|8|16         anisotropic texture filtering, off = the game's own 1x\n"
         "  --hdr-photos on|off          also save <name>.hdr.png (HDR) beside HD photo copies and screenshots; needs HDR output\n"
         "  --mods-readahead on|off      read the HD texture files of the course that is loading ahead of the game\n"
         "  --distant-scenery off|high|ultra  farther model detail and draw distance (display only), off = the game's own\n"
         "  --shader-workers <n|auto>    shader translator processes on a first launch (auto = half the cores, at most 8; 1 = one at a time)\n"
         "  --update-check on|off        check for updates at launcher start\n"
         "  --update-channel stable|testing\n"
         "                               which update channel the check follows\n"
         "  --backup on|off              copy the profile to the backup folder when the game closes\n"
         "  --backup-folder <folder>     where the backup zips go (a OneDrive or Dropbox folder, for example)\n"
         "  --backup-keep <n>            backup zips kept per profile, 1 to 100 (default 3)\n"
         "  --backup-include <list>      what goes in: any of saves, settings, photos (default all three)\n"
         "  --profile <name>             profile to play or to import to / export from\n"
         "  --set <key>=<value>          one setting for this start only, repeatable (keys: docs/command-line.md);\n"
         "                               a name that starts with SNAP_ sets that environment variable\n"
         "  --data-dir <folder>          keep saves, settings, mods, caches and logs in this folder\n"
         "  --no-prompt                  never open a question window; a folder the game cannot write to exits with code 3\n"
         "\n"
         "Launcher actions without the launcher window (the game does not start):\n"
         "  --update                     install the newest version; it runs in its own window (exit 0 = started)\n"
         "  --check-install              check the game files and print the result (exit 0 = all good, 1 = something is wrong)\n"
         "  --report                     make the problem report zip and print where it is; nothing is uploaded\n"
         "  --restore-backup [latest|<n>]\n"
         "                               list the save backups, or put one back (no window)\n"
         "  --backups [list | now | verify [<n>] | restore <n|latest> [--into-current --yes]]\n"
         "                               the profile backups: list them, back up now, check one, or restore one into a new profile (no window)\n"
         "  --upscalers [<name> --accept-license <name>]\n"
         "                               list DLSS / FSR / XeSS, or download one whose license you accept (no window)\n"
         "  --hdtex [options]            HD texture state, or the HD texture tool with its own options (no window)\n"
         "  --mods [list|conflicts [--files] [<id>]|order <id>...|move <id> up|down|top|bottom|<n>|enable <id>...|disable <id>...]\n"
         "                               mod packs in priority order, which pack wins each shared file, or change the order (no window)\n"
         "\n"
         "Other commands (no game window unless noted):\n"
         "  --import-save <folder or zip>, --export-save <folder>\n"
         "  --benchmark [--benchmark-capped | --benchmark-uncapped] [--benchmark-view <file>]   plays the benchmark ride\n"
         "  --prepare-shaders [--force] [--visible] [--list] [--stop] [--courses <list>]\n"
         "  --help, --version\n";
}

}  // namespace snap_cli
