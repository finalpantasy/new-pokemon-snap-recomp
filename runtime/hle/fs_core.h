// SPDX-License-Identifier: MIT
// Host file system behind nn::fs: named mounts over host directories.
//   rom      read-only, an ordered list of directories ("layers"), first match wins (mod loader hook)
//   savedata crash-safe: writes go to a working copy and become visible to the next mount only at commit()
//   host     plain directory (tests, future sdmc)
// No guest types here, so the tests drive it directly. The guest-facing shims are in fs.cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hle.h"

namespace hle::fs {

// ---- result codes: module 2 (fs). Evidence for each is in docs/hle-services.md section "nn::fs result codes".
constexpr uint32_t kResultPathNotFound = result(2, 1);            // main 0x602ca0 / 0x602f6c: desc 1 -> game error 0xd
constexpr uint32_t kResultPathAlreadyExists = result(2, 2);       // 0x602f0c cmp 0x402 after CreateFile; desc 2 -> 0xf
constexpr uint32_t kResultTargetLocked = result(2, 7);            // 0x60362c cmp 0xe00 -> game error 0x11
constexpr uint32_t kResultDirectoryNotEmpty = result(2, 8);       // SDK value; not seen in the game
constexpr uint32_t kResultUsableSpaceNotEnough = result(2, 31);   // 0x603614: desc 30..45 -> game error 0x10
constexpr uint32_t kResultOutOfRange = result(2, 3005);           // SDK value for a short strict read
constexpr uint32_t kResultInvalidArgument = result(2, 6001);      // host-chosen (bad handle, bad path syntax)
constexpr uint32_t kResultNotMounted = result(2, 6002);           // host-chosen
constexpr uint32_t kResultWriteNotPermitted = result(2, 6003);    // host-chosen (write to the read-only rom mount)

enum OpenMode : int { kModeRead = 1, kModeWrite = 2, kModeAppend = 4 };
enum EntryType : int { kDirectory = 0, kFile = 1 };               // 0x8bd348: cbz -> OpenDirectory, cmp 1 -> OpenFile
enum DirMode : int { kOpenDirs = 1, kOpenFiles = 2, kOpenAll = 3 };

using FileId = uint64_t;  // opaque, never 0
using DirId = uint64_t;
struct DirEntry { std::string name; int type; int64_t size; };

struct SimulatedCrash { const char* stage; };  // thrown by test fault hooks

struct SaveGeneration { uint64_t gen; bool current; };

class Fs {
 public:
  Fs();
  ~Fs();
  Fs(const Fs&) = delete;
  Fs& operator=(const Fs&) = delete;

  // ---- mounts
  // Mounts `name` over rom_layers() (first match wins). Fails if no layer exists.
  uint32_t mount_rom(const std::string& name);
  uint32_t mount_save(const std::string& name, const uint64_t uid[2]);
  uint32_t mount_host(const std::string& name, const std::string& dir, bool writable);
  uint32_t unmount(const std::string& name);
  bool is_mounted(const std::string& name);
  // The mod-loader hook: ordered directories, earlier ones win. romfs_root goes last.
  void set_rom_layers(std::vector<std::string> dirs);
  std::vector<std::string> rom_layers() const;
  void set_save_root(const std::string& dir);
  void set_save_backups(int n);

  // ---- files and directories (paths look like "rom:/common/x.drp")
  uint32_t create_file(const std::string& path, int64_t size);
  uint32_t delete_file(const std::string& path);
  uint32_t open_file(FileId* out, const std::string& path, int mode);
  uint32_t close_file(FileId f);
  // out == nullptr: strict form (ReadFile(handle, off, buf, size)): reading past the end fails with kResultOutOfRange.
  // out != nullptr: reads what exists and stores the count.
  uint32_t read_file(FileId f, int64_t off, void* buf, size_t n, size_t* out);
  uint32_t write_file(FileId f, int64_t off, const void* buf, size_t n, bool flush);
  uint32_t flush_file(FileId f);
  uint32_t get_file_size(FileId f, int64_t* out);
  uint32_t set_file_size(FileId f, int64_t size);
  uint32_t get_entry_type(int* out, const std::string& path);
  uint32_t create_directory(const std::string& path);
  uint32_t delete_directory(const std::string& path, bool recursive);
  uint32_t rename_file(const std::string& from, const std::string& to);
  uint32_t open_directory(DirId* out, const std::string& path, int mode);
  uint32_t read_directory(DirId d, DirEntry* out, int64_t max, int64_t* count);  // advances the cursor
  uint32_t get_directory_entry_count(DirId d, int64_t* out);                       // all entries, not the remainder
  uint32_t close_directory(DirId d);

  // ---- save data
  // Makes every change since the last commit durable and visible to the next mount, atomically: the new state is
  // built as a complete generation directory, then a one-line pointer file is replaced by write-temp + rename.
  uint32_t commit(const std::string& mount);
  uint32_t get_free_space(int64_t* out, const std::string& mount);
  std::vector<SaveGeneration> list_generations(const std::string& mount);
  // Makes generation `gen` the current one by committing a copy of it as a new generation.
  uint32_t restore_generation(const std::string& mount, uint64_t gen);

  // Test hook: called at named stages of commit; may throw SimulatedCrash.
  void set_fault_hook(std::function<void(const char* stage)> hook);

 struct Impl;  // implementation detail

 private:
  std::unique_ptr<Impl> p_;
};

Fs& instance();  // the process-wide instance the guest shims use

// "rom:/a/b" -> mount "rom", rel "a/b" (normalized, no leading slash). false on a malformed path.
bool split_path(const std::string& path, std::string* mount, std::string* rel);

}  // namespace hle::fs
