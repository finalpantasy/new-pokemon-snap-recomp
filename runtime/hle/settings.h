// SPDX-License-Identifier: MIT
// Per-profile settings (F1 menu): a flat key=value file, <profiles root>/<profile id>/settings.cfg (or <data root>/settings.cfg when profiles are off).
// Live settings (gyro, rumble) are pushed to input::pad_fx() at once. Settings that env-var mods read at startup (SNAP_LOOK, SNAP_SSAO_QUALITY)
// are exported as those env vars by apply() unless the variable is already set, so they take effect on the next start.
#pragma once
#include <string>

namespace hle::settings {

void load();                                   // after profiles::startup: picks the file, reads it, calls apply(true)
void apply(bool env = false);                  // live input settings; with env also the env-var mods (only when unset), at startup only
void forget_exported_env();                    // restart: drop the env vars this layer exported
std::string get(const std::string& key, const std::string& def = "");
void set(const std::string& key, const std::string& value);   // saves at once (atomic)
bool get_bool(const std::string& key, bool def);
float get_float(const std::string& key, float def);
void set_bool(const std::string& key, bool v);
void set_float(const std::string& key, float v);
std::string file_path();
void use_path_for_test(const std::string& path);   // tests: pick a file without profiles

}  // namespace hle::settings
