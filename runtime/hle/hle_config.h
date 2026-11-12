// SPDX-License-Identifier: MIT
// HLE configuration: paths, user identity, language, operation mode. Loaded from a JSON file
// (default %APPDATA%\SnapRecomp\config\hle.json; env SNAP_HLE_CONFIG overrides the path) plus env overrides.
// Every key is optional; defaults are listed in docs/hle-services.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hle {

struct Config {
  // ---- file system
  std::string romfs_root = "extracted/romfs-v262144";  // env SNAP_ROMFS overrides. Read only.
  std::vector<std::string> rom_layers;                 // mod overlay directories, first match wins, before romfs_root
  std::string data_root;                               // default %APPDATA%\SnapRecomp
  std::string save_root;                               // default <data_root>\save
  int save_backups = 5;                                // generations kept per user
  int64_t save_data_size = 128ll << 20;                // reported by GetSaveDataSize (not seen in nacp: assumed)
  int64_t save_journal_size = 32ll << 20;
  // ---- user
  std::string user_name = "Default";
  uint64_t uid[2] = {0x534e415052454331ull, 0x0000000000000001ull};  // stable default; "uid": "<32 hex digits>"
  // ---- system
  int language = 1;                                    // nn::settings::Language: 1 = AmericanEnglish (en-US)
  std::string region = "US";
  bool docked = true;                                  // nn::oe::GetOperationMode: Console
  std::string display_version = "2.0.1";
  bool headless = false;                               // nn::err::Show* only logs (tests, CI); env SNAP_HLE_HEADLESS=1
  std::string input_map_path;                          // default %SNAP_CONFIG_DIR% or <data_root>, file input.json
  std::string screenshot_dir;                          // nn::album::SaveScreenshot; default <data_root>\screenshots
  int controller_style = 0;                            // 0 Pro Controller (FullKey, id No1), 1 Handheld (id 0x20)
};

Config& config();                                      // process-wide, filled by load_config / defaults
// Reads the JSON file at `path` (empty: the default path). Returns false and logs if the file exists but is invalid;
// a missing file is fine (defaults). Applies env overrides afterwards and fills the derived paths.
bool load_config(const std::string& path = "");
void apply_defaults();                                 // derived paths from data_root; called by load_config
std::string default_data_root();                       // %APPDATA%\SnapRecomp (HOME/.local/share/SnapRecomp elsewhere)
std::string default_input_path();                      // %SNAP_CONFIG_DIR%\input.json, else <data_root>\input.json
std::string legacy_input_path();                       // <data_root>\config\input_map.json (version 1, migrated on first load)
std::string uid_dir_name(const uint64_t uid[2]);       // 32 lowercase hex digits

}  // namespace hle
