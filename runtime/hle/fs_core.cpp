// SPDX-License-Identifier: MIT
#include "fs_core.h"
#include "os/perf.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "hle_config.h"
#include "save_gen.h"

// runtime/os releases the caller's virtual core around blocking reads. These no-op weak defaults keep builds without
// runtime/os (tests/hle) linking; snap_boot links the real ones from os_kernel.cpp.
extern "C" __attribute__((weak)) void os_io_begin(void) {}
extern "C" __attribute__((weak)) void os_io_end(void) {}

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace hle::fs {

namespace stdfs = std::filesystem;

static stdfs::path to_path(const std::string& s) { return stdfs::u8path(s); }
static std::string from_path(const stdfs::path& p) {
  auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

// ---------------------------------------------------------------------------------------------------------------
// host file with positioned I/O
class HostFile {
 public:
  ~HostFile() { close(); }
  bool open(const stdfs::path& p, bool write) {
#ifdef _WIN32
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    h_ = CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    return h_ != INVALID_HANDLE_VALUE;
#else
    fd_ = ::open(p.c_str(), write ? O_RDWR : O_RDONLY);
    return fd_ >= 0;
#endif
  }
  void close() {
#ifdef _WIN32
    if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
#else
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
  }
  int64_t size() {
#ifdef _WIN32
    LARGE_INTEGER s;
    return GetFileSizeEx(h_, &s) ? s.QuadPart : -1;
#else
    off_t cur = lseek(fd_, 0, SEEK_CUR); (void)cur;
    return lseek(fd_, 0, SEEK_END);
#endif
  }
  // returns bytes read, -1 on error
  int64_t read_at(int64_t off, void* buf, size_t n) {
    size_t done = 0;
    char* b = static_cast<char*>(buf);
    while (done < n) {
#ifdef _WIN32
      DWORD chunk = (DWORD)std::min<size_t>(n - done, 1u << 30), got = 0;
      OVERLAPPED ov{};
      ov.Offset = (DWORD)((off + (int64_t)done) & 0xffffffff);
      ov.OffsetHigh = (DWORD)((off + (int64_t)done) >> 32);
      if (!ReadFile(h_, b + done, chunk, &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) break;
        return -1;
      }
      if (got == 0) break;
      // [perf] pass 5: mark the filled pages written from user mode, so GetWriteWatch (dirty-page buffer sync, runtime\nvn\nvn_vk.cpp)
      // reports them after the data is final. A test (perf pass 5, results.md) showed buffered reads are seen anyway but
      // unbuffered/DMA reads racing a watch reset leave pages stale; an atomic OR with 0 per page changes no byte.
      for (uintptr_t p = (uintptr_t)(b + done) & ~(uintptr_t)3, e = (uintptr_t)(b + done + got); p < e; p = (p | 4095) + 1)   // one aligned dword per page (same page as the buffer bytes)
        InterlockedOr((volatile LONG*)p, 0);
      done += got;
#else
      ssize_t got = pread(fd_, b + done, n - done, off + (off_t)done);
      if (got < 0) return -1;
      if (got == 0) break;
      done += (size_t)got;
#endif
    }
    return (int64_t)done;
  }
  bool write_at(int64_t off, const void* buf, size_t n) {
    const char* b = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < n) {
#ifdef _WIN32
      DWORD chunk = (DWORD)std::min<size_t>(n - done, 1u << 30), got = 0;
      OVERLAPPED ov{};
      ov.Offset = (DWORD)((off + (int64_t)done) & 0xffffffff);
      ov.OffsetHigh = (DWORD)((off + (int64_t)done) >> 32);
      if (!WriteFile(h_, b + done, chunk, &got, &ov)) return false;
      done += got;
#else
      ssize_t got = pwrite(fd_, b + done, n - done, off + (off_t)done);
      if (got <= 0) return false;
      done += (size_t)got;
#endif
    }
    return true;
  }
  bool set_size(int64_t size) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = size;
    return SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) && SetEndOfFile(h_);
#else
    return ftruncate(fd_, size) == 0;
#endif
  }
  bool flush() {
#ifdef _WIN32
    return FlushFileBuffers(h_) != 0;
#else
    return fsync(fd_) == 0;
#endif
  }

 private:
#ifdef _WIN32
  HANDLE h_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

// Flushes a file to disk by path (used for commit durability). Best effort.
static void flush_path(const stdfs::path& p) {
  HostFile f;
  if (f.open(p, true)) { f.flush(); }
}

// Atomically replaces `to` with `from` (same directory/volume), writing through.
static bool replace_file(const stdfs::path& from, const stdfs::path& to) {
#ifdef _WIN32
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  stdfs::rename(from, to, ec);
  return !ec;
#endif
}

// True when nothing exists at `p` (not even a dangling link). An unreadable status counts as present.
static bool gone(const stdfs::path& p) {
  std::error_code ec;
  return stdfs::symlink_status(p, ec).type() == stdfs::file_type::not_found;
}
// Removes a file or tree and proves it is gone. remove_all can stop part-way (a file held open without FILE_SHARE_DELETE).
static bool remove_checked(const stdfs::path& p) {
  std::error_code ec;
  stdfs::remove_all(p, ec);
  return !ec && gone(p);
}

// The result a failed commit returns, by cause. Only a full disk is the game's "not enough space" error; every other cause (a
// file held by another program, a permission or I/O error) returns TargetLocked, which the game's result mapper also handles
// (main 0x60362c, game error 0x11), instead of telling the player the disk is full.
static uint32_t commit_result(const std::error_code& ec) {
#ifdef _WIN32
  if (ec.category() == std::system_category() && (ec.value() == ERROR_DISK_FULL || ec.value() == ERROR_HANDLE_DISK_FULL)) return kResultUsableSpaceNotEnough;
#endif
  if (ec == std::errc::no_space_on_device || ec == std::errc::file_too_large) return kResultUsableSpaceNotEnough;
  return kResultTargetLocked;
}
static std::error_code last_os_error() {
#ifdef _WIN32
  return std::error_code((int)GetLastError(), std::system_category());
#else
  return std::error_code(errno, std::generic_category());
#endif
}

bool split_path(const std::string& path, std::string* mount, std::string* rel) {
  size_t c = path.find(':');
  if (c == std::string::npos || c == 0 || c > 32) return false;
  *mount = path.substr(0, c);
  std::string r = path.substr(c + 1);
  if (r.empty() || (r[0] != '/' && r[0] != '\\')) return false;
  std::string out, comp;
  auto flush_comp = [&]() -> bool {
    if (comp.empty() || comp == ".") { comp.clear(); return true; }
    if (comp == "..") return false;
    if (!out.empty()) out += '/';
    out += comp;
    comp.clear();
    return true;
  };
  for (char ch : r) {
    if (ch == '/' || ch == '\\') { if (!flush_comp()) return false; }
    else comp += ch;
  }
  if (!flush_comp()) return false;
  *rel = out;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
namespace {

struct OpenFile {
  HostFile hf;
  bool writable = false;
  std::string mount;
  std::string rel;          // save mounts: path inside the mount, so a write after a commit re-marks the file dirty
  struct Save* save = nullptr;  // owned by the mount; kept alive by keep below
  std::shared_ptr<void> keep;   // the owning Mount: a write that outlives unmount/remount still has a valid save for mark_dirty
  std::mutex mu;  // serializes write / resize on this handle
};
struct OpenDir {
  std::vector<DirEntry> entries;
  size_t cursor = 0;
};

constexpr const char* kPointerName = "CURRENT";

// The save mount: committed generations on disk plus a working directory for the uncommitted changes.
//   <root>/gens/<n>/      complete state of generation n (immutable once committed)
//   <root>/gens/<n>.ok    written after generation n is complete
//   <root>/CURRENT        "SNAPSAVE1\n<n>\n<check>\n", replaced atomically at commit
//   <root>/working/       copy-on-write scratch of the running session; wiped at every mount
struct Save {
  stdfs::path root;
  uint64_t cur = 0;
  int keep = 5;
  std::string lock_key;  // key of the folder lock in Fs::Impl::locks (empty: none)
  std::set<std::string> dirty, deleted, dirs;  // dirs: directories created since the last commit
  std::string work_name = "working";           // "working", or "working.<k>" when an old one could not be removed
  std::set<uint64_t> orphans;                  // complete but never published (marker stuck after a failed commit)

  stdfs::path gens() const { return root / "gens"; }
  stdfs::path gen_dir(uint64_t n) const { return gens() / std::to_string(n); }
  stdfs::path gen_ok(uint64_t n) const { return gens() / (std::to_string(n) + ".ok"); }
  // <n>.orphan: written when a failed commit cannot remove <n>.ok; such a generation is not valid (removed with it)
  stdfs::path gen_orphan(uint64_t n) const { return gens() / (std::to_string(n) + ".orphan"); }
  stdfs::path working() const { return root / work_name; }

  // Generation numbers present under gens/ (directories and markers alike).
  std::set<uint64_t> gen_numbers() const {
    std::set<uint64_t> v;
    std::error_code ec;
    for (stdfs::directory_iterator it(gens(), ec), end; !ec && it != end; it.increment(ec)) {
      const std::string stem = from_path(it->path().stem());
      if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
      v.insert(std::strtoull(stem.c_str(), nullptr, 10));
    }
    return v;
  }
  // Removes a generation marker first, then its directory, so a partly removed directory never sits under a valid marker.
  // A marker that cannot be removed gets an orphan mark beside it, so the generation stops counting either way.
  // The orphan mark is written before the marker is touched, so neither a stuck marker nor a crash part-way through leaves a
  // valid-looking generation behind. If the mark itself cannot be written, the in-memory set still covers this session.
  bool mark_orphan(uint64_t g) {
    orphans.insert(g);
    {
      std::ofstream f(gen_orphan(g), std::ios::binary | std::ios::trunc);
      f << "orphan\n";
      f.flush();
      if (!f) { log("fs: save: cannot write the orphan mark %s", from_path(gen_orphan(g)).c_str()); return false; }
    }
    flush_path(gen_orphan(g));
    return true;
  }
  bool drop_generation(uint64_t g) {
    if (!gone(gen_ok(g))) mark_orphan(g);
    if (!remove_checked(gen_ok(g))) {
      log("fs: save: cannot remove %s; generation %llu is kept whole but orphaned", from_path(gen_ok(g)).c_str(), (unsigned long long)g);
      return false;
    }
    if (!remove_checked(gen_dir(g))) { log("fs: save: cannot fully remove %s (in use?); it has no marker and is never loaded", from_path(gen_dir(g)).c_str()); return false; }
    std::error_code ec;
    stdfs::remove(gen_orphan(g), ec);   // last: harmless once the marker is gone (fresh_generation clears a leftover)
    return true;
  }
  // A new, empty scratch directory "working" or "working.<k>" (k from 1) under root; false if none can be created.
  bool fresh_working(std::string* name) const {
    for (int k = 0; k < 64; ++k) {
      std::string nm = k ? "working." + std::to_string(k) : std::string("working");
      std::error_code ec;
      if (stdfs::create_directory(root / nm, ec) && !ec) { *name = nm; return true; }
    }
    return false;
  }
  stdfs::path committed() const { return gen_dir(cur); }

  static uint32_t check_of(uint64_t n) { return (uint32_t)(n * 2654435761u) ^ 0x534e4150u; }

  bool read_pointer(uint64_t* n) const { return savegen::read_pointer_file(root / kPointerName, n); }
  bool write_pointer(uint64_t n, const std::function<void(const char*)>& hook) const {
    stdfs::path tmp = root / "CURRENT.tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      f << "SNAPSAVE1\n" << n << "\n" << check_of(n) << "\n";
    }
    flush_path(tmp);
    if (hook) hook("commit:pointer-tmp");
    if (!replace_file(tmp, root / kPointerName)) return false;
    if (hook) hook("commit:pointer-done");
    return true;
  }
  bool gen_valid(uint64_t n) const { return !orphans.count(n) && savegen::gen_valid(root, n); }   // shared rule (save_gen.h)

  // Finds the newest complete generation at or below `limit` (no limit: any).
  bool scan(uint64_t* out) const { return savegen::newest_valid(root, out, [&](uint64_t n) { return orphans.count(n) != 0; }); }

  // Opens the save: finds the committed generation, drops every uncommitted or half-built leftover.
  bool open(int keep_gens) {
    keep = keep_gens;
    std::error_code ec;
    stdfs::create_directories(gens(), ec);
    uint64_t n = 0;
    // CURRENT, else a valid CURRENT.tmp (the interrupted replace), else the newest valid generation: the rule profile copies use
    bool have = savegen::committed_gen(root, &n, nullptr, [&](uint64_t g) { return orphans.count(g) != 0; });
    if (!have) {
      // no save: an empty generation, built on a number whose leftovers (if any) are gone, never on top of them
      n = 0;
      for (uint64_t g = 0; g < 64; ++g) {
        if (!drop_generation(g)) continue;
        std::error_code e1;
        if (!stdfs::create_directory(gen_dir(g), e1) || e1) continue;
        n = g;
        break;
      }
      orphans.erase(n);
      std::ofstream(gen_ok(n)) << "ok\n";
    }
    cur = n;
    // leftovers newer than the committed generation never became current: an interrupted commit. One that cannot be removed
    // (a file held open) stays without a marker; the next commit skips its number.
    for (uint64_t g : gen_numbers()) if (g > cur) drop_generation(g);
    stdfs::remove(root / "CURRENT.tmp", ec);
    if (!read_pointer(&n) || n != cur) {
      std::function<void(const char*)> none;
      if (!write_pointer(cur, none)) log("fs: save: cannot rewrite %s (generation %llu stays the newest complete one)", from_path(root / kPointerName).c_str(), (unsigned long long)cur);
    }
    // working/ (and working.<k> from earlier runs) hold the last session's uncommitted changes: all are dropped. If one cannot be
    // removed it must not be reused (its files would show up as this session's), so a fresh name is taken.
    std::vector<stdfs::path> old_work;
    for (stdfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string nm = from_path(it->path().filename());
      if (nm == "working" || nm.rfind("working.", 0) == 0) old_work.push_back(it->path());
    }
    for (auto& p : old_work)
      if (!remove_checked(p)) log("fs: save: cannot remove the old scratch folder %s; using a new one", from_path(p).c_str());
    dirty.clear(); deleted.clear(); dirs.clear();
    if (!fresh_working(&work_name)) { log("fs: save: cannot create a scratch folder in %s", from_path(root).c_str()); return false; }
    return true;
  }

  // 0 none, 1 directory, 2 file
  int kind(const std::string& rel) const {
    if (rel.empty()) return 1;
    if (deleted.count(rel)) return 0;
    std::error_code ec;
    stdfs::path w = working() / to_path(rel);
    if (stdfs::exists(w, ec)) return stdfs::is_directory(w, ec) ? 1 : 2;
    stdfs::path c = committed() / to_path(rel);
    if (stdfs::exists(c, ec)) return stdfs::is_directory(c, ec) ? 1 : 2;
    return 0;
  }
  // The host path to read `rel` from (working copy if there is one).
  stdfs::path read_path(const std::string& rel) const {
    std::error_code ec;
    stdfs::path w = working() / to_path(rel);
    if (stdfs::exists(w, ec)) return w;
    return committed() / to_path(rel);
  }
  // Makes sure the file has a working copy and returns it.
  bool ensure_working(const std::string& rel, stdfs::path* out) {
    std::error_code ec;
    stdfs::path w = working() / to_path(rel);
    stdfs::create_directories(w.parent_path(), ec);
    if (!stdfs::exists(w, ec)) {
      stdfs::path c = committed() / to_path(rel);
      if (!deleted.count(rel) && stdfs::exists(c, ec) && stdfs::is_regular_file(c, ec)) {
        stdfs::copy_file(c, w, stdfs::copy_options::overwrite_existing, ec);
        if (ec) return false;
      }
    }
    dirty.insert(rel);
    *out = w;
    return true;
  }

  std::vector<DirEntry> list(const std::string& rel) const {
    std::map<std::string, DirEntry> m;
    std::error_code ec;
    auto add = [&](const stdfs::path& dir) {
      for (auto& e : stdfs::directory_iterator(dir, ec)) {
        std::string name = from_path(e.path().filename());
        std::string child = rel.empty() ? name : rel + "/" + name;
        if (deleted.count(child)) continue;
        DirEntry d;
        d.name = name;
        std::error_code e2;
        d.type = e.is_directory(e2) ? kDirectory : kFile;
        d.size = d.type == kFile ? (int64_t)e.file_size(e2) : 0;
        m[name] = d;  // working (added second) overrides committed
      }
    };
    stdfs::path c = committed() / to_path(rel), w = working() / to_path(rel);
    if (stdfs::is_directory(c, ec)) add(c);
    if (stdfs::is_directory(w, ec)) add(w);
    std::vector<DirEntry> out;
    for (auto& kv : m) out.push_back(kv.second);
    return out;
  }

  // Copies (hard-links where possible) every unchanged file of the committed generation into the new one. Any failure is returned:
  // a generation missing a carried file must never be published.
  bool carry(const stdfs::path& from, const stdfs::path& to, const std::string& rel, std::error_code& why) {
    std::error_code ec;
    stdfs::directory_iterator it(from, ec);
    if (ec) { why = ec; return false; }
    for (const stdfs::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) { why = ec; return false; }
      const auto& e = *it;
      std::string name = from_path(e.path().filename());
      std::string child = rel.empty() ? name : rel + "/" + name;
      if (deleted.count(child)) continue;
      std::error_code e2;
      const bool isdir = e.is_directory(e2);
      if (e2) { why = e2; return false; }
      // replaced this session by an entry of the other type (file -> directory or directory -> file, also below a replaced
      // parent): the session's entry is in working/ and is written by the dirty / dirs passes; the old one is not carried
      const stdfs::file_type wt = stdfs::symlink_status(working() / to_path(child), e2).type();
      if (wt == stdfs::file_type::none || wt == stdfs::file_type::unknown) { why = e2 ? e2 : std::make_error_code(std::errc::io_error); return false; }
      if (wt != stdfs::file_type::not_found && (wt == stdfs::file_type::directory) != isdir) continue;
      if (isdir ? dirty.count(child) != 0 : dirs.count(child) != 0) continue;   // same, from the session's own records
      e2.clear();
      if (isdir) {
        stdfs::create_directories(to / name, e2);
        if (e2) { why = e2; return false; }
        if (!carry(e.path(), to / name, child, why)) return false;
      } else if (!dirty.count(child)) {
        // `to` is a directory this commit created itself, so the name is new; copy_options::none refuses an existing entry
        // instead of writing through it (an existing entry could be a hard link into a committed generation).
        stdfs::create_hard_link(e.path(), to / name, e2);
        if (e2) {
          e2.clear();
          stdfs::copy_file(e.path(), to / name, stdfs::copy_options::none, e2);
          if (e2) { why = e2; return false; }
          flush_path(to / name);   // a real copy must be on disk before the .ok marker, like every written file
        }
      }
    }
    if (ec) why = ec;
    return !ec;
  }

  // Picks the number for the next generation and creates its directory fresh. A leftover gens/<n> (an interrupted or failed
  // commit whose cleanup was blocked, e.g. a file held open by a scanner) is never built on: its entries may be hard links into
  // committed generations (writing to one writes through into CURRENT and the backups) and may hold files deleted since. When
  // it cannot be fully removed, the next unused number is taken; the leftover never has a .ok marker, so it is never loaded.
  bool fresh_generation(uint64_t* out) {
    std::error_code ec;
    stdfs::create_directories(gens(), ec);
    for (uint64_t n = cur + 1, tries = 0; tries < 64; ++n, ++tries) {
      if (!remove_checked(gen_ok(n))) { log("fs: save: cannot remove the stale marker %s; skipping generation %llu", from_path(gen_ok(n)).c_str(), (unsigned long long)n); continue; }
      if (!remove_checked(gen_dir(n))) { log("fs: save: cannot remove the stale %s; skipping generation %llu", from_path(gen_dir(n)).c_str(), (unsigned long long)n); continue; }
      if (!remove_checked(gen_orphan(n))) continue;   // a leftover orphan mark would make the new generation invalid
      orphans.erase(n);
      std::error_code e1;
      if (!stdfs::create_directory(gen_dir(n), e1) || e1) continue;   // false: something re-created it; never reuse an existing directory
      *out = n;
      return true;
    }
    return false;
  }

  uint32_t do_commit(const std::function<void(const char*)>& hook) {
    auto stage = [&](const char* s) { if (hook) hook(s); };
    std::error_code ec;
    stage("commit:begin");
    uint64_t n = 0;
    if (!fresh_generation(&n)) { log("fs: save: no free generation number in %s (stale generations held open)", from_path(gens()).c_str()); return kResultTargetLocked; }
    stdfs::path ng = gen_dir(n);
    // From here a failure removes the half-built generation and returns an error; CURRENT keeps pointing at the old one.
    // If the marker cannot be removed the generation is complete (the marker is written last), so it stays whole rather than
    // torn under a valid marker, but it is marked orphaned: never listed, restored, loaded or counted as a backup.
    // A directory that cannot be removed is skipped by the next commit (fresh_generation).
    auto fail = [&](const std::error_code& why, const char* what) {
      const uint32_t rc = commit_result(why);
      log("fs: save: commit failed at %s: %s (result %#x)", what, why.message().c_str(), rc);
      // drop_generation marks it orphaned first when the .ok was already written; a folder it cannot remove is skipped later
      if (!drop_generation(n)) log("fs: save: the failed generation %llu could not be fully removed; the next commit uses a new number", (unsigned long long)n);
      return rc;
    };
    if (!carry(committed(), ng, "", ec)) return fail(ec, "carry");
    stage("commit:carried");
    for (const std::string& rel : dirty) {
      stdfs::path src = working() / to_path(rel), dst = ng / to_path(rel);
      const bool have = stdfs::exists(src, ec);
      if (ec) return fail(ec, "file status");   // unknown is not "deleted": never drop a file on an I/O error
      if (!have) continue;  // created then deleted in the same session
      stdfs::create_directories(dst.parent_path(), ec);
      if (ec) return fail(ec, "directory");
      // never write through an existing entry (it could be a hard link into a committed generation): remove it, prove it is
      // gone, then create the file new
      if (!gone(dst) && !remove_checked(dst)) return fail(std::make_error_code(std::errc::device_or_resource_busy), "replace");
      stdfs::copy_file(src, dst, stdfs::copy_options::none, ec);
      if (ec) return fail(ec, "file copy");
      flush_path(dst);
      stage("commit:file");
    }
    for (const std::string& rel : dirs) {
      stdfs::create_directories(ng / to_path(rel), ec);
      if (ec) return fail(ec, "directory");
    }
    stage("commit:files-done");
    {
      std::ofstream f(gen_ok(n), std::ios::binary);
      f << "ok\n";
      f.flush();
      if (!f) return fail(std::error_code(errno ? errno : EIO, std::generic_category()), "marker");
    }
    flush_path(gen_ok(n));
    stage("commit:marker");
    if (!write_pointer(n, hook)) return fail(last_os_error(), "pointer");
    orphans.erase(n);
    cur = n;
    dirty.clear(); deleted.clear(); dirs.clear();
    // automatic backups: keep the current generation and the `keep` newest complete ones before it (numbers can have gaps
    // where a stale leftover was skipped); everything else below the current one goes, leftovers included
    if (keep >= 0) {
      std::set<uint64_t> nums, valid;
      for (uint64_t g : gen_numbers()) if (g < cur) { nums.insert(g); if (gen_valid(g)) valid.insert(g); }
      int kept = 0;
      for (auto it = valid.rbegin(); it != valid.rend() && kept < keep; ++it, ++kept) nums.erase(*it);
      for (uint64_t g : nums) drop_generation(g);
    }
    return kSuccess;
  }
};

struct Mount {
  enum Kind { Rom, SaveData, Host } kind = Host;
  bool writable = false;
  std::vector<stdfs::path> layers;  // rom/host
  std::unique_ptr<Save> save;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
struct Fs::Impl {
  std::mutex mu;  // mounts and tables
  std::map<std::string, std::shared_ptr<Mount>> mounts;
  std::unordered_map<uint64_t, std::shared_ptr<OpenFile>> files;
  std::unordered_map<uint64_t, std::shared_ptr<OpenDir>> dirs;
  std::atomic<uint64_t> next{0x1000};
  std::vector<std::string> rom_layers;
  std::string save_root;
  int save_backups = 5;
  std::function<void(const char*)> hook;
  std::map<std::string, void*> locks;  // save root -> open ".snap.lock" handle holding an exclusive byte lock (one running game per save folder)

  ~Impl() {
#ifdef _WIN32
    for (auto& kv : locks) CloseHandle((HANDLE)kv.second);
#endif
  }
  // Drops the folder lock once no save mount uses it any more (unmount, remount).
  void release_lock(const std::string& key) {
#ifdef _WIN32
    if (key.empty()) return;
    for (auto& kv : mounts) if (kv.second->save && kv.second->save->lock_key == key) return;
    if (auto it = locks.find(key); it != locks.end()) { CloseHandle((HANDLE)it->second); locks.erase(it); }
#else
    (void)key;
#endif
  }
  std::shared_ptr<Mount> find_mount(const std::string& name) {
    auto it = mounts.find(name);
    return it == mounts.end() ? nullptr : it->second;
  }
  uint64_t token() { return next.fetch_add(0x10); }
};

Fs::Fs() : p_(new Impl) {
  Config& c = config();
  p_->save_root = c.save_root;
  p_->save_backups = c.save_backups;
  std::vector<std::string> l = c.rom_layers;
  l.push_back(c.romfs_root);
  p_->rom_layers = std::move(l);
}
Fs::~Fs() = default;

void Fs::set_rom_layers(std::vector<std::string> dirs) {
  std::lock_guard<std::mutex> g(p_->mu);
  p_->rom_layers = std::move(dirs);
}
std::vector<std::string> Fs::rom_layers() const {
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->rom_layers;
}
void Fs::set_save_root(const std::string& dir) { std::lock_guard<std::mutex> g(p_->mu); p_->save_root = dir; }
void Fs::set_save_backups(int n) { std::lock_guard<std::mutex> g(p_->mu); p_->save_backups = n; }
void Fs::set_fault_hook(std::function<void(const char*)> hook) { std::lock_guard<std::mutex> g(p_->mu); p_->hook = std::move(hook); }

uint32_t Fs::mount_rom(const std::string& name) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = std::make_shared<Mount>();
  m->kind = Mount::Rom;
  std::error_code ec;
  for (auto& d : p_->rom_layers) {
    stdfs::path p = to_path(d);
    if (stdfs::is_directory(p, ec)) m->layers.push_back(p);
    else log("fs: rom layer %s does not exist; skipped", d.c_str());
  }
  if (m->layers.empty()) { log("fs: no rom layer exists; MountRom(%s) fails", name.c_str()); return kResultPathNotFound; }
  p_->mounts[name] = m;
  if (trace_flags & kTraceFs) log("fs: mounted %s: with %zu layer(s), first %s", name.c_str(), m->layers.size(), from_path(m->layers[0]).c_str());
  return kSuccess;
}

uint32_t Fs::mount_host(const std::string& name, const std::string& dir, bool writable) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = std::make_shared<Mount>();
  m->kind = Mount::Host;
  m->writable = writable;
  std::error_code ec;
  stdfs::path p = to_path(dir);
  if (writable) stdfs::create_directories(p, ec);
  if (!stdfs::is_directory(p, ec)) return kResultPathNotFound;
  m->layers.push_back(p);
  p_->mounts[name] = m;
  return kSuccess;
}

uint32_t Fs::mount_save(const std::string& name, const uint64_t uid[2]) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = std::make_shared<Mount>();
  m->kind = Mount::SaveData;
  m->writable = true;
  m->save = std::make_unique<Save>();
  m->save->root = to_path(p_->save_root) / to_path(uid_dir_name(uid));
  std::error_code ec;
  // Same name mounted again: the old mount stays untouched until the new one can no longer fail (lock, directory); only then is it
  // dropped (its open handles die with it; Save::open wipes working/ anyway). The new mount takes over the same folder lock.
#ifdef _WIN32
  // One running game per save folder. Save::open deletes working/ and every newer generation, which would wreck a second live
  // process on the same folder, so refuse before touching anything. Per save root (parallel test runs use their own roots);
  // the lock dies with the process, so a crash never leaves a stale one.
  {
    stdfs::path root = to_path(p_->save_root);
    stdfs::create_directories(root, ec);
    const std::string key = from_path(root);
    m->save->lock_key = key;
    if (!p_->locks.count(key)) {
      // No FILE_SHARE_DELETE: while a game holds the lock, nothing can delete or rename it away (profile Clear/Delete, cleanup
      // scripts), so a second copy can never create a fresh lock file and mount the same folder.
      HANDLE h = CreateFileW((root / ".snap.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      OVERLAPPED ov{};
      if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();   // no guard, no mount: Save::open would wipe a running copy's working/ and newer generations
        log("fs: cannot open the save lock file in %s (error %lu); not mounting the save folder", key.c_str(), e);
        return (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED) ? kResultTargetLocked : kResultUsableSpaceNotEnough;
      } else if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        log("fs: save folder %s is in use by another running copy of the game (lock error %lu); not mounting it", key.c_str(), e);
        return e == ERROR_LOCK_VIOLATION ? kResultTargetLocked : kResultUsableSpaceNotEnough;
      } else {
        p_->locks[key] = h;
      }
    }
  }
#endif
  stdfs::create_directories(m->save->root, ec);
  if (ec) { log("fs: cannot create save directory %s: %s", from_path(m->save->root).c_str(), ec.message().c_str()); return kResultUsableSpaceNotEnough; }
  if (auto old = p_->mounts.find(name); old != p_->mounts.end()) {
    for (auto f = p_->files.begin(); f != p_->files.end();) { if (f->second->mount == name) f = p_->files.erase(f); else ++f; }
    p_->mounts.erase(old);   // OpenFile::keep holds the Save of a handle a writer still has in flight
  }
  if (!m->save->open(p_->save_backups)) {
    p_->release_lock(m->save->lock_key);
    return kResultUsableSpaceNotEnough;
  }
  p_->mounts[name] = m;
  if (trace_flags & kTraceFs) log("fs: mounted %s: save %s generation %llu", name.c_str(), from_path(m->save->root).c_str(), (unsigned long long)m->save->cur);
  return kSuccess;
}

uint32_t Fs::unmount(const std::string& name) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->mounts.find(name);
  if (it == p_->mounts.end()) return kResultNotMounted;
  for (auto f = p_->files.begin(); f != p_->files.end();) {
    if (f->second->mount == name) f = p_->files.erase(f); else ++f;
  }
  const std::string lock_key = it->second->save ? it->second->save->lock_key : std::string();
  p_->mounts.erase(it);  // uncommitted save changes are dropped, as on hardware
  p_->release_lock(lock_key);
  return kSuccess;
}

bool Fs::is_mounted(const std::string& name) {
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->mounts.count(name) != 0;
}

// ---- path resolution helpers (caller holds the lock)
namespace {
struct Resolved {
  std::shared_ptr<Mount> m;
  std::string rel;
  stdfs::path host;  // for layered mounts: first layer holding rel (or the first layer if none)
  int kind = 0;      // 0 none, 1 dir, 2 file
};
}  // namespace

static uint32_t resolve(Fs::Impl* p, const std::string& path, Resolved* r) {
  std::string mount;
  if (!split_path(path, &mount, &r->rel)) return kResultInvalidArgument;
  r->m = p->find_mount(mount);
  if (!r->m) return kResultNotMounted;
  std::error_code ec;
  if (r->m->kind == Mount::SaveData) {
    r->kind = r->m->save->kind(r->rel);
    r->host = r->m->save->read_path(r->rel);
    return kSuccess;
  }
  r->host = r->m->layers[0] / to_path(r->rel);
  r->kind = 0;
  for (auto& layer : r->m->layers) {
    stdfs::path h = layer / to_path(r->rel);
    if (stdfs::exists(h, ec)) {
      r->host = h;
      r->kind = stdfs::is_directory(h, ec) ? 1 : 2;
      break;
    }
  }
  if (r->rel.empty()) r->kind = 1;
  return kSuccess;
}

uint32_t Fs::open_file(FileId* out, const std::string& path, int mode) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  bool want_write = (mode & (kModeWrite | kModeAppend)) != 0;
  if (want_write && !r.m->writable) return kResultWriteNotPermitted;
  if (r.kind != 2) return kResultPathNotFound;
  auto of = std::make_shared<OpenFile>();
  of->writable = want_write;
  std::string mount, rel;
  split_path(path, &mount, &rel);
  of->mount = mount;
  if (want_write && r.m->kind == Mount::SaveData) { of->rel = r.rel; of->save = r.m->save.get(); of->keep = r.m; }
  stdfs::path host = r.host;
  if (want_write) {
    if (r.m->kind == Mount::SaveData) {
      if (!r.m->save->ensure_working(r.rel, &host)) return kResultUsableSpaceNotEnough;
    }
  }
  if (!of->hf.open(host, want_write)) return kResultTargetLocked;
  uint64_t t = p_->token();
  p_->files[t] = of;
  *out = t;
  if (trace_flags & (kTraceFs | kTraceOpen)) log("fs: open %s mode %d -> %s (file %llu)", path.c_str(), mode, from_path(host).c_str(), (unsigned long long)t);
  return kSuccess;
}

uint32_t Fs::close_file(FileId f) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->files.find(f);
  if (it == p_->files.end()) return kResultInvalidArgument;
  p_->files.erase(it);
  if (trace_flags & kTraceFs) log("fs: close file %llu", (unsigned long long)f);
  return kSuccess;
}

static std::shared_ptr<OpenFile> get_file(Fs::Impl* p, FileId f) {
  std::lock_guard<std::mutex> g(p->mu);
  auto it = p->files.find(f);
  return it == p->files.end() ? nullptr : it->second;
}

uint32_t Fs::read_file(FileId f, int64_t off, void* buf, size_t n, size_t* out) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (off < 0) return kResultOutOfRange;
  if (n == 0) { if (out) *out = 0; return kSuccess; }
  int64_t got;
  os_io_begin();   // the read releases the caller's virtual core (os review finding 9; SNAP_SCHED_LEGACY=hostblock keeps it)
  if (snap_perf::on) {   // perf pass 3: time file reads
    const uint64_t t0 = snap_perf::now_ns();
    got = of->hf.read_at(off, buf, n);
    snap_perf::wait_add(snap_perf::WT_FS, snap_perf::now_ns() - t0);
    if (got > 0) snap_perf::g_fs_bytes.fetch_add((uint64_t)got, std::memory_order_relaxed);
  } else {
    got = of->hf.read_at(off, buf, n);
  }
  os_io_end();
  if (trace_flags & kTraceFs) log("fs: read file %llu off %lld n %zu -> %lld", (unsigned long long)f, (long long)off, n, (long long)got);
  if (got < 0) return kResultTargetLocked;
  if (out) { *out = (size_t)got; return kSuccess; }
  return (size_t)got == n ? kSuccess : kResultOutOfRange;
}

// A handle that stays open across Commit keeps writing to the working copy; do_commit cleared the dirty set, so every write
// re-marks its file (after the write, so a commit that runs in between still sees the file dirty next time). Lock order: of->mu, then p->mu
// (commit holds p->mu and never takes of->mu).
static void mark_dirty(Fs::Impl* p, OpenFile& of) {
  if (!of.save) return;
  std::lock_guard<std::mutex> g(p->mu);
  of.save->dirty.insert(of.rel);
}

uint32_t Fs::write_file(FileId f, int64_t off, const void* buf, size_t n, bool flush) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (!of->writable) return kResultWriteNotPermitted;
  if (off < 0) return kResultOutOfRange;
  std::lock_guard<std::mutex> g(of->mu);
  // Horizon needs the file to be created with its final size (WriteFile does not grow it unless OpenMode_AllowAppend).
  // The host grows it either way: the game only ever writes inside the size it created.
  if (n && !of->hf.write_at(off, buf, n)) return kResultUsableSpaceNotEnough;
  if (flush) of->hf.flush();
  mark_dirty(p_.get(), *of);
  if (trace_flags & kTraceFs) log("fs: write file %llu off %lld n %zu", (unsigned long long)f, (long long)off, n);
  return kSuccess;
}

uint32_t Fs::flush_file(FileId f) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (of->writable) { std::lock_guard<std::mutex> g(of->mu); of->hf.flush(); }
  return kSuccess;
}

uint32_t Fs::get_file_size(FileId f, int64_t* out) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  int64_t s = of->hf.size();
  if (s < 0) return kResultTargetLocked;
  *out = s;
  return kSuccess;
}

uint32_t Fs::set_file_size(FileId f, int64_t size) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (!of->writable) return kResultWriteNotPermitted;
  if (size < 0) return kResultOutOfRange;
  std::lock_guard<std::mutex> g(of->mu);
  if (!of->hf.set_size(size)) return kResultUsableSpaceNotEnough;
  mark_dirty(p_.get(), *of);
  return kSuccess;
}

uint32_t Fs::get_entry_type(int* out, const std::string& path) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (r.kind == 0) return kResultPathNotFound;
  *out = r.kind == 1 ? kDirectory : kFile;
  return kSuccess;
}

uint32_t Fs::create_file(const std::string& path, int64_t size) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (!r.m->writable) return kResultWriteNotPermitted;
  if (r.rel.empty()) return kResultInvalidArgument;
  if (r.kind != 0) return kResultPathAlreadyExists;
  std::error_code ec;
  stdfs::path host;
  if (r.m->kind == Mount::SaveData) {
    std::string parent = r.rel.find('/') == std::string::npos ? "" : r.rel.substr(0, r.rel.rfind('/'));
    if (r.m->save->kind(parent) != 1) return kResultPathNotFound;
    r.m->save->deleted.erase(r.rel);
    r.m->save->dirty.insert(r.rel);
    host = r.m->save->working() / to_path(r.rel);
    stdfs::create_directories(host.parent_path(), ec);
  } else {
    host = r.m->layers[0] / to_path(r.rel);
    if (!stdfs::is_directory(host.parent_path(), ec)) return kResultPathNotFound;
  }
  HostFile tmp;
  {
    std::ofstream f(host, std::ios::binary | std::ios::trunc);
    if (!f) return kResultPathNotFound;
  }
  if (size > 0) {
    if (!tmp.open(host, true) || !tmp.set_size(size)) return kResultUsableSpaceNotEnough;
  }
  if (trace_flags & kTraceFs) log("fs: create %s size %lld", path.c_str(), (long long)size);
  return kSuccess;
}

uint32_t Fs::delete_file(const std::string& path) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (!r.m->writable) return kResultWriteNotPermitted;
  if (r.kind != 2) return kResultPathNotFound;
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    Save& s = *r.m->save;
    stdfs::remove(s.working() / to_path(r.rel), ec);
    s.dirty.erase(r.rel);
    if (stdfs::exists(s.committed() / to_path(r.rel), ec)) s.deleted.insert(r.rel);
  } else {
    stdfs::remove(r.m->layers[0] / to_path(r.rel), ec);
    if (ec) return kResultTargetLocked;
  }
  return kSuccess;
}

uint32_t Fs::create_directory(const std::string& path) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (!r.m->writable) return kResultWriteNotPermitted;
  if (r.kind != 0) return kResultPathAlreadyExists;
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    Save& s = *r.m->save;
    std::string parent = r.rel.find('/') == std::string::npos ? "" : r.rel.substr(0, r.rel.rfind('/'));
    if (s.kind(parent) != 1) return kResultPathNotFound;
    stdfs::create_directories(s.working() / to_path(r.rel), ec);
    if (ec) return kResultTargetLocked;
    // un-deleting the path is fine even if a committed *file* had this name: carry() sees the type change (working/ holds a
    // directory, dirs lists it) and does not carry the old file
    s.deleted.erase(r.rel);
    s.dirs.insert(r.rel);
  } else {
    stdfs::path h = r.m->layers[0] / to_path(r.rel);
    if (!stdfs::is_directory(h.parent_path(), ec)) return kResultPathNotFound;
    stdfs::create_directory(h, ec);
    if (ec) return kResultPathNotFound;
  }
  return kSuccess;
}

uint32_t Fs::delete_directory(const std::string& path, bool recursive) {
  std::unique_lock<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (!r.m->writable) return kResultWriteNotPermitted;
  if (r.kind != 1 || r.rel.empty()) return kResultPathNotFound;
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    Save& s = *r.m->save;
    std::function<uint32_t(const std::string&)> rm = [&](const std::string& rel) -> uint32_t {
      for (auto& e : s.list(rel)) {
        std::string child = rel + "/" + e.name;
        if (!recursive) return kResultDirectoryNotEmpty;
        if (e.type == kDirectory) { uint32_t x = rm(child); if (x) return x; }
        else {
          stdfs::remove(s.working() / to_path(child), ec);
          s.dirty.erase(child);
          if (stdfs::exists(s.committed() / to_path(child), ec)) s.deleted.insert(child);
        }
      }
      stdfs::remove(s.working() / to_path(rel), ec);
      s.dirs.erase(rel);
      if (stdfs::exists(s.committed() / to_path(rel), ec)) s.deleted.insert(rel);
      return kSuccess;
    };
    return rm(r.rel);
  }
  stdfs::path h = r.m->layers[0] / to_path(r.rel);
  if (recursive) stdfs::remove_all(h, ec);
  else {
    if (!stdfs::is_empty(h, ec)) return kResultDirectoryNotEmpty;
    stdfs::remove(h, ec);
  }
  return ec ? kResultTargetLocked : kSuccess;
}

uint32_t Fs::rename_file(const std::string& from, const std::string& to) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved a, b;
  uint32_t rc = resolve(p_.get(), from, &a);
  if (rc) return rc;
  rc = resolve(p_.get(), to, &b);
  if (rc) return rc;
  if (a.m != b.m) return kResultInvalidArgument;
  if (!a.m->writable) return kResultWriteNotPermitted;
  if (a.kind != 2) return kResultPathNotFound;
  if (b.kind != 0) return kResultPathAlreadyExists;
  std::error_code ec;
  const std::string to_parent = b.rel.find('/') == std::string::npos ? "" : b.rel.substr(0, b.rel.rfind('/'));
  if (a.m->kind == Mount::SaveData) {
    Save& s = *a.m->save;
    if (s.kind(to_parent) != 1) return kResultPathNotFound;   // as on hardware: the destination folder must exist (and be a folder)
    stdfs::path src;
    if (!s.ensure_working(a.rel, &src)) return kResultUsableSpaceNotEnough;
    stdfs::path dst = s.working() / to_path(b.rel);
    stdfs::create_directories(dst.parent_path(), ec);
    stdfs::rename(src, dst, ec);
    if (ec) return kResultTargetLocked;
    s.dirty.erase(a.rel);
    if (stdfs::exists(s.committed() / to_path(a.rel), ec)) s.deleted.insert(a.rel);
    s.deleted.erase(b.rel);
    s.dirty.insert(b.rel);
  } else {
    if (!stdfs::is_directory(a.m->layers[0] / to_path(to_parent), ec)) return kResultPathNotFound;
    stdfs::rename(a.m->layers[0] / to_path(a.rel), a.m->layers[0] / to_path(b.rel), ec);
    if (ec) return kResultTargetLocked;
  }
  return kSuccess;
}

uint32_t Fs::open_directory(DirId* out, const std::string& path, int mode) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (r.kind != 1) return kResultPathNotFound;
  auto od = std::make_shared<OpenDir>();
  std::map<std::string, DirEntry> merged;
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    for (auto& e : r.m->save->list(r.rel)) merged[e.name] = e;
  } else {
    // layered union: the first layer that has the name decides its type and size
    for (auto it = r.m->layers.rbegin(); it != r.m->layers.rend(); ++it) {
      stdfs::path d = *it / to_path(r.rel);
      if (!stdfs::is_directory(d, ec)) continue;
      for (auto& e : stdfs::directory_iterator(d, ec)) {
        DirEntry de;
        de.name = from_path(e.path().filename());
        std::error_code e2;
        de.type = e.is_directory(e2) ? kDirectory : kFile;
        de.size = de.type == kFile ? (int64_t)e.file_size(e2) : 0;
        merged[de.name] = de;
      }
    }
  }
  for (auto& kv : merged) {
    bool is_dir = kv.second.type == kDirectory;
    if ((is_dir && (mode & kOpenDirs)) || (!is_dir && (mode & kOpenFiles))) od->entries.push_back(kv.second);
  }
  uint64_t t = p_->token();
  p_->dirs[t] = od;
  *out = t;
  return kSuccess;
}

uint32_t Fs::read_directory(DirId d, DirEntry* out, int64_t max, int64_t* count) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->dirs.find(d);
  if (it == p_->dirs.end()) return kResultInvalidArgument;
  OpenDir& od = *it->second;
  int64_t n = 0;
  while (n < max && od.cursor < od.entries.size()) out[n++] = od.entries[od.cursor++];
  *count = n;
  return kSuccess;
}

uint32_t Fs::get_directory_entry_count(DirId d, int64_t* out) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->dirs.find(d);
  if (it == p_->dirs.end()) return kResultInvalidArgument;
  *out = (int64_t)it->second->entries.size();
  return kSuccess;
}

uint32_t Fs::close_directory(DirId d) {
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->dirs.erase(d) ? kSuccess : kResultInvalidArgument;
}

uint32_t Fs::commit(const std::string& mount) {
  std::vector<std::shared_ptr<OpenFile>> to_flush;
  std::unique_lock<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m) return kResultNotMounted;
  if (m->kind != Mount::SaveData) return kSuccess;  // nothing to commit on other mounts
  for (auto& kv : p_->files) if (kv.second->mount == mount && kv.second->writable) to_flush.push_back(kv.second);
  for (auto& f : to_flush) f->hf.flush();
  auto hook = p_->hook;
  uint32_t rc = m->save->do_commit(hook);
  if (trace_flags & kTraceFs) log("fs: commit %s -> generation %llu (result %#x)", mount.c_str(), (unsigned long long)m->save->cur, rc);
  return rc;
}

bool Fs::drain_for_exit(int ms) {
#ifdef _WIN32
  const ULONGLONG end = GetTickCount64() + (ULONGLONG)(ms > 0 ? ms : 0);
  while (!p_->mu.try_lock()) {
    if (GetTickCount64() >= end) return false;
    Sleep(5);
  }
  return true;
#else
  (void)ms;
  return true;
#endif
}

uint32_t Fs::get_free_space(int64_t* out, const std::string& mount) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m) return kResultNotMounted;
  std::error_code ec;
  stdfs::path where = m->kind == Mount::SaveData ? m->save->root : m->layers[0];
  auto sp = stdfs::space(where, ec);
  int64_t avail = ec ? (int64_t)1 << 40 : (int64_t)sp.available;
  // the game extends the save when it sees under 100 MiB free (main 0x609a3c); report at least 4 GiB
  *out = std::clamp<int64_t>(avail, (int64_t)4 << 30, (int64_t)1 << 40);
  return kSuccess;
}

std::vector<SaveGeneration> Fs::list_generations(const std::string& mount) {
  std::lock_guard<std::mutex> g(p_->mu);
  std::vector<SaveGeneration> v;
  auto m = p_->find_mount(mount);
  if (!m || m->kind != Mount::SaveData) return v;
  for (uint64_t n : m->save->gen_numbers())   // digits-only stems, each number once (a stray "1a.ok" is not generation 1)
    if (n <= m->save->cur && m->save->gen_valid(n)) v.push_back({n, n == m->save->cur});   // above CURRENT: never published
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.gen < b.gen; });
  return v;
}

uint32_t Fs::restore_generation(const std::string& mount, uint64_t gen) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m || m->kind != Mount::SaveData) return kResultNotMounted;
  Save& s = *m->save;
  if (gen > s.cur || !s.gen_valid(gen)) return kResultPathNotFound;
  // The restore is staged in a new scratch folder: a copy of the chosen generation where every file counts as dirty, so the
  // commit writes it all. Any copy or listing error drops the stage and leaves the session (working folder, pending changes)
  // as it was; a partial restore is never committed. The session's old scratch folder goes only after the commit succeeded.
  std::string stage_name;
  if (!s.fresh_working(&stage_name)) return kResultTargetLocked;
  const stdfs::path stage = s.root / stage_name, src = s.gen_dir(gen);
  std::set<std::string> dirty, deleted, dirs;
  std::error_code why;   // first failure; decides the result code
  {
    std::error_code ec;
    stdfs::recursive_directory_iterator it(src, ec), end;
    for (; !why && !ec && it != end; it.increment(ec)) {
      std::error_code e1;
      const auto u = it->path().lexically_relative(src).generic_u8string();
      const std::string rel(u.begin(), u.end());
      const bool isdir = it->is_directory(e1);
      if (!e1 && (rel.empty() || rel == ".")) e1 = std::make_error_code(std::errc::io_error);
      if (!e1 && isdir) {
        stdfs::create_directories(stage / to_path(rel), e1);
        dirs.insert(rel);
      } else if (!e1) {
        stdfs::create_directories((stage / to_path(rel)).parent_path(), e1);
        if (!e1) stdfs::copy_file(it->path(), stage / to_path(rel), stdfs::copy_options::none, e1);
        dirty.insert(rel);
      }
      if (e1) why = e1;
    }
    if (ec && !why) why = ec;
  }
  // entries of the current generation that the restored one lacks must disappear. One the restored generation has with the
  // other type (file vs directory) is left to carry(), which skips type-changed entries; nothing below it is walked, so no path
  // runs through a file.
  std::function<bool(const stdfs::path&, const std::string&)> walk = [&](const stdfs::path& d, const std::string& rel) -> bool {
    std::error_code e1;
    for (stdfs::directory_iterator it(d, e1), end; it != end; it.increment(e1)) {
      if (e1) { why = e1; return false; }
      std::string name = from_path(it->path().filename());
      std::string child = rel.empty() ? name : rel + "/" + name;
      std::error_code e2;
      const bool isdir = it->is_directory(e2);
      if (e2) { why = e2; return false; }
      const stdfs::file_type t = stdfs::symlink_status(src / to_path(child), e2).type();
      if (t == stdfs::file_type::not_found) { deleted.insert(child); continue; }
      if (t == stdfs::file_type::none || t == stdfs::file_type::unknown) { why = e2 ? e2 : std::make_error_code(std::errc::io_error); return false; }
      if (isdir && t == stdfs::file_type::directory && !walk(it->path(), child)) return false;
    }
    if (e1) why = e1;
    return !e1;
  };
  if (!why) walk(s.committed(), "");
  if (why) {
    if (!remove_checked(stage)) log("fs: save: cannot remove the restore stage %s", from_path(stage).c_str());
    log("fs: save: restore of generation %llu failed while copying (%s); nothing changed", (unsigned long long)gen, why.message().c_str());
    return commit_result(why);
  }
  // swap the stage in; a failed commit swaps the session back
  std::string old_name = s.work_name;
  std::set<std::string> old_dirty = std::move(s.dirty), old_deleted = std::move(s.deleted), old_dirs = std::move(s.dirs);
  s.work_name = stage_name;
  s.dirty = std::move(dirty); s.deleted = std::move(deleted); s.dirs = std::move(dirs);
  uint32_t rc = s.do_commit(p_->hook);
  if (rc != kSuccess) {
    s.work_name = old_name;
    s.dirty = std::move(old_dirty); s.deleted = std::move(old_deleted); s.dirs = std::move(old_dirs);
    if (!remove_checked(stage)) log("fs: save: cannot remove the restore stage %s", from_path(stage).c_str());
    return rc;
  }
  if (!remove_checked(s.root / old_name)) log("fs: save: cannot remove the old scratch folder %s (dropped at the next mount)", from_path(s.root / old_name).c_str());
  return rc;
}

Fs& instance() {
  static Fs fs;
  return fs;
}

}  // namespace hle::fs
