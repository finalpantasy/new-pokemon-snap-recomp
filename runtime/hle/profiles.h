// SPDX-License-Identifier: MIT
// Save profiles: several independent saves, picked at startup or from the F1 overlay.
//   <root>/profiles.json              names, last used profile, timestamps, pending actions (written atomically)
//   <root>/<id>/save/<uid>/...        one save root per profile (the layout the fs save layer uses)
//   <root>/_trash/<name>-<stamp>/     Clear and Delete move data here (meta.json + save/); the newest 10 are kept
// <root> is %APPDATA%\SnapRecomp\profiles (env SNAP_PROFILES_DIR overrides). Env SNAP_SAVE_DIR switches profiles off
// (test runs) unless SNAP_PROFILES_DIR is set too.
// Files of the profile the running game has mounted are never changed here: Clear and Delete of the active profile
// are queued in profiles.json ("pending") and applied at the next startup, before the save mounts.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hle {
namespace profiles {

struct Profile {
  std::string id;
  std::string name;
  int64_t created = 0;       // unix seconds
  int64_t last_played = 0;   // 0 = never
};

struct TrashEntry {
  std::string folder;        // directory name under _trash
  std::string name;          // profile name when it was trashed
  std::string kind;          // "clear" or "delete"
  int64_t when = 0;
  int64_t size = 0;
};

struct PendingOp { std::string op, id; };   // op: "clear" or "delete"

class Store {
 public:
  explicit Store(std::string root);
  const std::string& root() const { return root_; }
  // Reads profiles.json; a missing file is an empty store, an invalid one is kept as profiles.json.invalid and the store is
  // rebuilt from the profile folders. Returns false only when a message should be shown (see `message`).
  bool load();
  bool save();
  const std::string& message() const { return msg_; }

  const std::vector<Profile>& list() const { return profs_; }
  const Profile* find(const std::string& name_or_id) const;
  std::string save_dir(const std::string& id) const;          // <root>/<id>/save
  std::string last_used() const { return last_used_; }
  void set_last_used(const std::string& id);                  // also stamps last_played
  // True while a running game (this process or another) holds the profile's save lock (<save_dir>/.snap.lock). Clear and
  // Delete refuse such a profile, and a pending action on it stays queued.
  bool in_use(const std::string& id) const;
  int64_t size_bytes(const std::string& id) const;            // total size of the profile's save files
  bool has_data(const std::string& id) const;                 // any save file present

  // Each returns the id (or true) on success; on failure an empty string / false with `err` set.
  std::string create(const std::string& name, std::string* err);
  std::string next_free_name() const;                         // "Profile N"
  bool rename(const std::string& id, const std::string& name, std::string* err);
  std::string duplicate(const std::string& id, const std::string& new_name, std::string* err);   // copies one consistent generation per user
  bool clear(const std::string& id, std::string* err);        // save moves to _trash, profile stays (empty)
  bool remove(const std::string& id, std::string* err);       // save moves to _trash, profile entry goes
  std::vector<TrashEntry> trash_list() const;                 // newest first
  std::string restore(const std::string& trash_folder, std::string* err);   // new profile "<name> (restored)"
  void prune_trash(int keep = 10);
  // Imports the save in `src` (a save root holding <uid>/CURRENT, a single uid folder, or a profile folder with save/).
  std::string import_folder(const std::string& src, const std::string& name, std::string* err);
  // Moves <legacy> (the old single save folder) into a new profile "Profile 1": copy, verify, rename the old folder to
  // save.migrated-<date>. Never deletes. Returns false (legacy untouched, `err` set) if the copy cannot be verified.
  bool migrate_legacy(const std::string& legacy, std::string* err);

  // Pending actions for a profile that is mounted right now.
  void queue(const std::string& op, const std::string& id);
  const std::vector<PendingOp>& pending() const { return pending_; }
  bool is_pending(const std::string& id, const char* op = nullptr) const;
  void apply_pending();                                       // call before anything is mounted; ops on an in_use profile stay queued

 private:
  std::string next_id();
  std::string trash_move(const Profile& p, const char* kind, std::string* err);
  std::string root_, last_used_, msg_;
  std::vector<Profile> profs_;
  std::vector<PendingOp> pending_;
  int next_n_ = 1;
};

// Copies <src_save_root> to <dst_save_root> as one consistent generation per user (CURRENT, its gen dir and .ok).
bool copy_consistent(const std::string& src_save_root, const std::string& dst_save_root, std::string* err);
// True when <dir> looks like a save root (a <uid>/CURRENT inside), or a uid folder (CURRENT inside).
bool looks_like_save(const std::string& dir);
std::string sanitize_name(const std::string& s);
std::string format_time(int64_t t);       // "2026-11-11 14:05" local, "never" for 0
std::string format_size(int64_t bytes);   // "12.3 MB"
int64_t now_seconds();

// ---- startup
struct PickItem { const char* name; int64_t last_played; int64_t size; };
// Returns the chosen index; n (== count) means "New profile"; -1 means no choice (use the last used one).
typedef int (*PickerFn)(const PickItem* items, int count);

struct Active {
  bool on = false;           // profiles are in use
  std::string root, id, name, save_dir, note;
};
const Active& active();
void set_active(const Active& a);   // tests
// Resolves the profile for this run and returns the save root to use. Applies pending actions and the legacy migration first.
// `force_picker`: show the picker even with one profile. Without a picker function the last used profile is taken.
Active startup(const std::string& root, const std::string& legacy_save, const std::string& want, PickerFn picker, bool force_picker);

}  // namespace profiles
}  // namespace hle
