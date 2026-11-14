// SPDX-License-Identifier: MIT
// nn::fs host overrides. Mount names and call shapes come from `main`:
//   nnMain 0x4e3f70..0x4e4138: builds the string "rom", calls QueryMountRomCacheSize(&n) (0x4e410c), allocates n bytes and
//     calls MountRom("rom", cache, n) (0x4e4138). Paths are then built as "rom:/" + name (0x4e4150..0x4e41cc).
//   save init 0x4d5300..0x4d9cf4: account::Initialize/GetUserCount/TryOpenPreselectedUser/GetUserId/GetNickname, then
//     GetSaveDataSize(&size,&journal,uid) (0x4d7b00) and MountSaveData("savedata", uid) (0x4d7b10); files are named
//     "savedata:/%08x%08x" (format string at 0xaf02b0). Unmount("savedata") at 0x4e146c.
//   Commit("savedata") at 0x6072f4, 0x60744c, 0x607564, 0x607678, 0x607790, 0x6078ac, 0x609710.
//   GetFreeSpaceSize(&n,"savedata") at 0x609a30 (needs n >> 22 >= 0x19, i.e. 100 MiB) and 0x6107c8, then
//   ExtendSaveData(uid, saveSize + need, journal) at 0x610800 when it is short: the host reports plenty so that
//   path never runs.
#include <algorithm>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "fs_core.h"
#include "hle.h"
#include "hle_config.h"

// Taps for the DRP entry mod loader (runtime/mods/mods.cpp installs them only when mod packs exist): it learns which archive
// a read buffer came from. Null otherwise.
void (*g_snap_fs_tap_open)(uint64_t id, const char* path) = nullptr;
void (*g_snap_fs_tap_close)(uint64_t id) = nullptr;
void (*g_snap_fs_tap_read)(uint64_t id, const void* buf, size_t n) = nullptr;

namespace hle {
namespace {

using fs::instance;

std::string cstr(uint64_t a) { return a ? std::string(ptr<const char>(a)) : std::string(); }

// ---- mounting
void fs_set_allocator(Cpu* c) { (void)c; ret(c, 0); }  // game allocator for the SDK's internal buffers: not needed

void fs_query_mount_rom_cache_size(Cpu* c) {  // Result QueryMountRomCacheSize(size_t*)
  store<uint64_t>(ptr<void>(arg(c, 0)), 0x10000);
  ret(c, kSuccess);
}

void fs_mount_rom(Cpu* c) {  // Result MountRom(const char* name, void* cache, size_t size)
  ret(c, instance().mount_rom(cstr(arg(c, 0))));
}

void fs_get_save_data_size(Cpu* c) {  // Result GetSaveDataSize(long* size, long* journal, const Uid&)
  store<int64_t>(ptr<void>(arg(c, 0)), config().save_data_size);
  store<int64_t>(ptr<void>(arg(c, 1)), config().save_journal_size);
  ret(c, kSuccess);
}

void fs_mount_save_data(Cpu* c) {  // Result MountSaveData(const char* name, const Uid& uid)
  uint64_t uid[2];
  std::memcpy(uid, ptr<void>(arg(c, 1)), sizeof uid);
  uint32_t rc = instance().mount_save(cstr(arg(c, 0)), uid);
#ifdef _WIN32
  if (rc == fs::kResultTargetLocked) {   // another copy of the game owns this save folder: say so and stop, never touch its files
    const char* msg = "Another copy of New Pokemon Snap is already running with this save folder. Close it, or start this copy with a different profile or save folder (SNAP_SAVE_DIR). This copy will now close without changing any save.";
    log("fs: %s", msg);
    if (!config().headless) MessageBoxA(nullptr, msg, "Pokemon Snap", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    std::fflush(nullptr);
    ExitProcess(3);
  }
#endif
  ret(c, rc);
}

void fs_extend_save_data(Cpu* c) { ret(c, kSuccess); }  // Result ExtendSaveData(const Uid&, long, long): host space is not limited

void fs_unmount(Cpu* c) { instance().unmount(cstr(arg(c, 0))); ret(c, 0); }  // void Unmount(const char*)

void fs_commit(Cpu* c) { ret(c, instance().commit(cstr(arg(c, 0)))); }  // Result Commit(const char* mount) / CommitSaveData

void fs_get_free_space_size(Cpu* c) {  // Result GetFreeSpaceSize(long* out, const char* path)
  std::string path = cstr(arg(c, 1));
  size_t colon = path.find(':');
  int64_t v = 0;
  uint32_t rc = instance().get_free_space(&v, colon == std::string::npos ? path : path.substr(0, colon));
  if (!rc) store<int64_t>(ptr<void>(arg(c, 0)), v);
  ret(c, rc);
}

void fs_set_priority(Cpu* c) { (void)c; }  // void SetPriorityOnCurrentThread(Priority)

// ---- files
void fs_create_file(Cpu* c) { ret(c, instance().create_file(cstr(arg(c, 0)), (int64_t)arg(c, 1))); }  // (path, size[, option])
void fs_delete_file(Cpu* c) { ret(c, instance().delete_file(cstr(arg(c, 0)))); }

void fs_open_file(Cpu* c) {  // Result OpenFile(FileHandle* out, const char* path, int mode)
  fs::FileId id = 0;
  std::string path = cstr(arg(c, 1));
  uint32_t rc = instance().open_file(&id, path, (int)arg32(c, 2));
  if (!rc) { store<uint64_t>(ptr<void>(arg(c, 0)), id); if (g_snap_fs_tap_open) g_snap_fs_tap_open(id, path.c_str()); }
  ret(c, rc);
}
void fs_close_file(Cpu* c) { if (g_snap_fs_tap_close) g_snap_fs_tap_close(arg(c, 0)); instance().close_file(arg(c, 0)); ret(c, 0); }  // void CloseFile(FileHandle)

void fs_read_file(Cpu* c) {  // Result ReadFile(FileHandle, long offset, void* buf, size_t size): strict
  if (g_snap_fs_tap_read) g_snap_fs_tap_read(arg(c, 0), ptr<void>(arg(c, 2)), arg(c, 3));
  ret(c, instance().read_file(arg(c, 0), (int64_t)arg(c, 1), ptr<void>(arg(c, 2)), arg(c, 3), nullptr));
}
void fs_read_file_opt(Cpu* c) {  // Result ReadFile(FileHandle, long, void*, size_t, const ReadOption&)
  if (g_snap_fs_tap_read) g_snap_fs_tap_read(arg(c, 0), ptr<void>(arg(c, 2)), arg(c, 3));
  ret(c, instance().read_file(arg(c, 0), (int64_t)arg(c, 1), ptr<void>(arg(c, 2)), arg(c, 3), nullptr));
}
void fs_read_file_out(Cpu* c) {  // Result ReadFile(size_t* out, FileHandle, long, void*, size_t)
  size_t got = 0;
  if (g_snap_fs_tap_read) g_snap_fs_tap_read(arg(c, 1), ptr<void>(arg(c, 3)), arg(c, 4));
  uint32_t rc = instance().read_file(arg(c, 1), (int64_t)arg(c, 2), ptr<void>(arg(c, 3)), arg(c, 4), &got);
  if (!rc && arg(c, 0)) store<uint64_t>(ptr<void>(arg(c, 0)), got);
  ret(c, rc);
}
void fs_write_file(Cpu* c) {  // Result WriteFile(FileHandle, long, const void*, size_t, const WriteOption&)
  uint32_t flags = arg(c, 4) ? load<uint32_t>(ptr<void>(arg(c, 4))) : 0;
  ret(c, instance().write_file(arg(c, 0), (int64_t)arg(c, 1), ptr<void>(arg(c, 2)), arg(c, 3), (flags & 1) != 0));
}
void fs_flush_file(Cpu* c) { ret(c, instance().flush_file(arg(c, 0))); }
void fs_get_file_size(Cpu* c) {  // Result GetFileSize(long* out, FileHandle)
  int64_t s = 0;
  uint32_t rc = instance().get_file_size(arg(c, 1), &s);
  if (!rc) store<int64_t>(ptr<void>(arg(c, 0)), s);
  ret(c, rc);
}
void fs_set_file_size(Cpu* c) { ret(c, instance().set_file_size(arg(c, 0), (int64_t)arg(c, 1))); }

// ---- directories
void fs_get_entry_type(Cpu* c) {  // Result GetEntryType(DirectoryEntryType*, const char*)
  int t = 0;
  uint32_t rc = instance().get_entry_type(&t, cstr(arg(c, 1)));
  if (!rc) store<int32_t>(ptr<void>(arg(c, 0)), t);
  ret(c, rc);
}
void fs_create_directory(Cpu* c) { ret(c, instance().create_directory(cstr(arg(c, 0)))); }
void fs_delete_directory(Cpu* c) { ret(c, instance().delete_directory(cstr(arg(c, 0)), false)); }
void fs_delete_directory_rec(Cpu* c) { ret(c, instance().delete_directory(cstr(arg(c, 0)), true)); }
void fs_rename_file(Cpu* c) { ret(c, instance().rename_file(cstr(arg(c, 0)), cstr(arg(c, 1)))); }
void fs_open_directory(Cpu* c) {  // Result OpenDirectory(DirectoryHandle*, const char*, int mode)
  fs::DirId id = 0;
  uint32_t rc = instance().open_directory(&id, cstr(arg(c, 1)), (int)arg32(c, 2));
  if (!rc) store<uint64_t>(ptr<void>(arg(c, 0)), id);
  ret(c, rc);
}
void fs_close_directory(Cpu* c) { instance().close_directory(arg(c, 0)); ret(c, 0); }
void fs_get_directory_entry_count(Cpu* c) {  // Result (long* out, DirectoryHandle)
  int64_t n = 0;
  uint32_t rc = instance().get_directory_entry_count(arg(c, 1), &n);
  if (!rc) store<int64_t>(ptr<void>(arg(c, 0)), n);
  ret(c, rc);
}
// nn::fs::DirectoryEntry: char name[0x301]; pad[3]; s8 type @0x304; pad[3]; s64 size @0x308; sizeof 0x310
constexpr size_t kDirEntrySize = 0x310;
void fs_read_directory(Cpu* c) {  // Result ReadDirectory(long* outCount, DirectoryEntry* out, DirectoryHandle, long max)
  int64_t max = (int64_t)arg(c, 3);
  max = std::min<int64_t>(max, 1 << 16);   // a guest-controlled count must not size a host allocation (no game directory is near this)
  std::vector<fs::DirEntry> tmp((size_t)(max > 0 ? max : 0));
  int64_t n = 0;
  uint32_t rc = instance().read_directory(arg(c, 2), tmp.data(), max, &n);
  if (!rc) {
    uint8_t* out = ptr<uint8_t>(arg(c, 1));
    for (int64_t i = 0; i < n; ++i) {
      uint8_t* e = out + (size_t)i * kDirEntrySize;
      std::memset(e, 0, kDirEntrySize);
      std::snprintf(reinterpret_cast<char*>(e), 0x301, "%s", tmp[(size_t)i].name.c_str());
      e[0x304] = (uint8_t)tmp[(size_t)i].type;
      store<int64_t>(e + 0x308, tmp[(size_t)i].size);
    }
    store<int64_t>(ptr<void>(arg(c, 0)), n);
  }
  ret(c, rc);
}

}  // namespace

void register_fs(std::vector<Entry>& out) {
  HLE_NOOP("fs", "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", fs_set_allocator);
  HLE_IMPL("fs", "_ZN2nn2fs22QueryMountRomCacheSizeEPm", fs_query_mount_rom_cache_size);
  HLE_IMPL("fs", "_ZN2nn2fs8MountRomEPKcPvm", fs_mount_rom);
  HLE_IMPL("fs", "_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", fs_get_save_data_size);
  HLE_IMPL("fs", "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", fs_mount_save_data);
  HLE_NOOP("fs", "_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", fs_extend_save_data);
  HLE_IMPL("fs", "_ZN2nn2fs7UnmountEPKc", fs_unmount);
  HLE_IMPL("fs", "_ZN2nn2fs6CommitEPKc", fs_commit);
  HLE_IMPL("fs", "_ZN2nn2fs14CommitSaveDataEPKc", fs_commit);
  HLE_IMPL("fs", "_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE", fs_commit);
  HLE_IMPL("fs", "_ZN2nn2fs16GetFreeSpaceSizeEPlPKc", fs_get_free_space_size);
  HLE_NOOP("fs", "_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", fs_set_priority);
  HLE_IMPL("fs", "_ZN2nn2fs10CreateFileEPKcl", fs_create_file);
  HLE_IMPL("fs", "_ZN2nn2fs10CreateFileEPKcli", fs_create_file);
  HLE_IMPL("fs", "_ZN2nn2fs10DeleteFileEPKc", fs_delete_file);
  HLE_IMPL("fs", "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", fs_open_file);
  HLE_IMPL("fs", "_ZN2nn2fs9CloseFileENS0_10FileHandleE", fs_close_file);
  HLE_IMPL("fs", "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", fs_read_file);
  HLE_IMPL("fs", "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE", fs_read_file_opt);
  HLE_IMPL("fs", "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", fs_read_file_out);
  HLE_IMPL("fs", "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", fs_read_file_out);
  HLE_IMPL("fs", "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", fs_write_file);
  HLE_IMPL("fs", "_ZN2nn2fs9FlushFileENS0_10FileHandleE", fs_flush_file);
  HLE_IMPL("fs", "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", fs_get_file_size);
  HLE_IMPL("fs", "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", fs_set_file_size);
  HLE_IMPL("fs", "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", fs_get_entry_type);
  HLE_IMPL("fs", "_ZN2nn2fs15CreateDirectoryEPKc", fs_create_directory);
  HLE_IMPL("fs", "_ZN2nn2fs15DeleteDirectoryEPKc", fs_delete_directory);
  HLE_IMPL("fs", "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", fs_delete_directory_rec);
  HLE_IMPL("fs", "_ZN2nn2fs10RenameFileEPKcS2_", fs_rename_file);
  HLE_IMPL("fs", "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", fs_open_directory);
  HLE_IMPL("fs", "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", fs_close_directory);
  HLE_IMPL("fs", "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", fs_get_directory_entry_count);
  HLE_IMPL("fs", "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", fs_read_directory);
}

}  // namespace hle

// Window close (host/boot.cpp): let a save commit in progress finish before the process exits. The generation scheme survives a kill
// at any point, but finishing keeps the newest photo / progress instead of falling back one generation.
extern "C" void snap_screenshot_drain_for_exit(int ms);   // runtime/hle/services_sys.cpp
extern "C" void snap_photoexport_drain_for_exit(int ms);   // host/mods/photoexport.cpp (no-op where that file is not linked)
extern "C" void snap_photoexport_drain_none(int) {}
#pragma comment(linker, "/alternatename:snap_photoexport_drain_for_exit=snap_photoexport_drain_none")
extern "C" void snap_fs_drain_for_exit(int ms) { snap_screenshot_drain_for_exit(ms < 2000 ? ms : 2000); snap_photoexport_drain_for_exit(ms < 2000 ? ms : 2000); hle::fs::instance().drain_for_exit(ms); }
