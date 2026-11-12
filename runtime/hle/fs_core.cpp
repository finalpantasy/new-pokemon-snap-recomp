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
  std::set<std::string> dirty, deleted, dirs;  // dirs: directories created since the last commit

  stdfs::path gens() const { return root / "gens"; }
  stdfs::path gen_dir(uint64_t n) const { return gens() / std::to_string(n); }
  stdfs::path gen_ok(uint64_t n) const { return gens() / (std::to_string(n) + ".ok"); }
  stdfs::path working() const { return root / "working"; }
  stdfs::path committed() const { return gen_dir(cur); }

  static uint32_t check_of(uint64_t n) { return (uint32_t)(n * 2654435761u) ^ 0x534e4150u; }

  bool read_pointer(uint64_t* n) const {
    std::ifstream f(root / kPointerName, std::ios::binary);
    if (!f) return false;
    std::string magic;
    unsigned long long g = 0, chk = 0;
    if (!(f >> magic >> g >> chk) || magic != "SNAPSAVE1") return false;
    if ((uint32_t)chk != check_of(g)) return false;
    *n = g;
    return true;
  }
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
  bool gen_valid(uint64_t n) const {
    std::error_code ec;
    return stdfs::exists(gen_ok(n), ec) && stdfs::is_directory(gen_dir(n), ec);
  }

  // Finds the newest complete generation at or below `limit` (no limit: any).
  bool scan(uint64_t* out) const {
    std::error_code ec;
    bool found = false;
    uint64_t best = 0;
    for (auto& e : stdfs::directory_iterator(gens(), ec)) {
      if (e.path().extension() != ".ok") continue;
      uint64_t n = std::strtoull(e.path().stem().string().c_str(), nullptr, 10);
      if (gen_valid(n) && (!found || n > best)) { best = n; found = true; }
    }
    if (found) *out = best;
    return found;
  }

  // Opens the save: finds the committed generation, drops every uncommitted or half-built leftover.
  bool open(int keep_gens) {
    keep = keep_gens;
    std::error_code ec;
    stdfs::create_directories(gens(), ec);
    uint64_t n = 0;
    bool have = read_pointer(&n) && gen_valid(n);
    if (!have) {
      // the pointer is missing or torn: a CURRENT.tmp that is valid and complete is the interrupted replace
      stdfs::path tmp = root / "CURRENT.tmp";
      if (stdfs::exists(tmp, ec)) {
        std::ifstream f(tmp, std::ios::binary);
        std::string magic; unsigned long long g = 0, chk = 0;
        if (f && (f >> magic >> g >> chk) && magic == "SNAPSAVE1" && (uint32_t)chk == check_of(g) && gen_valid(g)) {
          n = g; have = true;
        }
      }
    }
    if (!have) have = scan(&n);
    if (!have) {
      n = 0;
      stdfs::create_directories(gen_dir(0), ec);
      std::ofstream(gen_ok(0)) << "ok\n";
    }
    cur = n;
    // leftovers newer than the committed generation never became current: an interrupted commit
    for (auto& e : stdfs::directory_iterator(gens(), ec)) {
      uint64_t g = std::strtoull(e.path().stem().string().c_str(), nullptr, 10);
      if (g > cur) stdfs::remove_all(e.path(), ec);
    }
    stdfs::remove(root / "CURRENT.tmp", ec);
    std::error_code ec2;
    if (!read_pointer(&n) || n != cur) {
      std::function<void(const char*)> none;
      write_pointer(cur, none);
    }
    stdfs::remove_all(working(), ec);
    stdfs::create_directories(working(), ec);
    dirty.clear(); deleted.clear(); dirs.clear();
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

  void carry(const stdfs::path& from, const stdfs::path& to, const std::string& rel) {
    std::error_code ec;
    for (auto& e : stdfs::directory_iterator(from, ec)) {
      std::string name = from_path(e.path().filename());
      std::string child = rel.empty() ? name : rel + "/" + name;
      if (deleted.count(child)) continue;
      if (e.is_directory(ec)) {
        stdfs::create_directories(to / name, ec);
        carry(e.path(), to / name, child);
      } else if (!dirty.count(child)) {
        stdfs::create_hard_link(e.path(), to / name, ec);
        if (ec) { ec.clear(); stdfs::copy_file(e.path(), to / name, stdfs::copy_options::overwrite_existing, ec); }
      }
    }
  }

  uint32_t do_commit(const std::function<void(const char*)>& hook) {
    auto stage = [&](const char* s) { if (hook) hook(s); };
    std::error_code ec;
    stage("commit:begin");
    uint64_t n = cur + 1;
    stdfs::path ng = gen_dir(n);
    stdfs::remove_all(ng, ec);
    stdfs::remove(gen_ok(n), ec);
    stdfs::create_directories(ng, ec);
    if (ec) return result(2, 6010);
    carry(committed(), ng, "");
    stage("commit:carried");
    for (const std::string& rel : dirty) {
      stdfs::path src = working() / to_path(rel), dst = ng / to_path(rel);
      if (!stdfs::exists(src, ec)) continue;  // created then deleted in the same session
      stdfs::create_directories(dst.parent_path(), ec);
      stdfs::copy_file(src, dst, stdfs::copy_options::overwrite_existing, ec);
      if (ec) return kResultUsableSpaceNotEnough;
      flush_path(dst);
      stage("commit:file");
    }
    for (const std::string& rel : dirs) stdfs::create_directories(ng / to_path(rel), ec);
    stage("commit:files-done");
    { std::ofstream f(gen_ok(n), std::ios::binary); f << "ok\n"; }
    flush_path(gen_ok(n));
    stage("commit:marker");
    if (!write_pointer(n, hook)) return kResultUsableSpaceNotEnough;
    cur = n;
    dirty.clear(); deleted.clear(); dirs.clear();
    // automatic backups: keep the current generation and `keep` before it
    if (keep >= 0 && cur > (uint64_t)keep) {
      for (auto& e : stdfs::directory_iterator(gens(), ec)) {
        uint64_t g = std::strtoull(e.path().stem().string().c_str(), nullptr, 10);
        if (g + (uint64_t)keep < cur) stdfs::remove_all(e.path(), ec);
      }
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
  stdfs::create_directories(m->save->root, ec);
  if (ec) { log("fs: cannot create save directory %s: %s", from_path(m->save->root).c_str(), ec.message().c_str()); return kResultUsableSpaceNotEnough; }
  m->save->open(p_->save_backups);
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
  p_->mounts.erase(it);  // uncommitted save changes are dropped, as on hardware
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
  if (trace_flags & kTraceFs) log("fs: open %s mode %d -> %s", path.c_str(), mode, from_path(host).c_str());
  return kSuccess;
}

uint32_t Fs::close_file(FileId f) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->files.find(f);
  if (it == p_->files.end()) return kResultInvalidArgument;
  p_->files.erase(it);
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
  if (snap_perf::on) {   // perf pass 3: file reads hold the caller's virtual core (no os_host_block); time them
    const uint64_t t0 = snap_perf::now_ns();
    got = of->hf.read_at(off, buf, n);
    snap_perf::wait_add(snap_perf::WT_FS, snap_perf::now_ns() - t0);
    if (got > 0) snap_perf::g_fs_bytes.fetch_add((uint64_t)got, std::memory_order_relaxed);
  } else {
    got = of->hf.read_at(off, buf, n);
  }
  if (trace_flags & kTraceFs) log("fs: read file %llu off %lld n %zu -> %lld", (unsigned long long)f, (long long)off, n, (long long)got);
  if (got < 0) return kResultTargetLocked;
  if (out) { *out = (size_t)got; return kSuccess; }
  return (size_t)got == n ? kSuccess : kResultOutOfRange;
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
  return of->hf.set_size(size) ? kSuccess : kResultUsableSpaceNotEnough;
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
    s.deleted.erase(r.rel);
    stdfs::create_directories(s.working() / to_path(r.rel), ec);
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
  if (a.m->kind == Mount::SaveData) {
    Save& s = *a.m->save;
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
  std::error_code ec;
  for (auto& e : stdfs::directory_iterator(m->save->gens(), ec)) {
    if (e.path().extension() != ".ok") continue;
    uint64_t n = std::strtoull(e.path().stem().string().c_str(), nullptr, 10);
    if (m->save->gen_valid(n)) v.push_back({n, n == m->save->cur});
  }
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.gen < b.gen; });
  return v;
}

uint32_t Fs::restore_generation(const std::string& mount, uint64_t gen) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m || m->kind != Mount::SaveData) return kResultNotMounted;
  Save& s = *m->save;
  if (!s.gen_valid(gen)) return kResultPathNotFound;
  // working := a copy of the chosen generation; every file counts as dirty so the commit writes it all
  std::error_code ec;
  stdfs::remove_all(s.working(), ec);
  stdfs::create_directories(s.working(), ec);
  s.dirty.clear(); s.deleted.clear(); s.dirs.clear();
  stdfs::path src = s.gen_dir(gen);
  for (auto& e : stdfs::recursive_directory_iterator(src, ec)) {
    std::string rel = stdfs::relative(e.path(), src, ec).generic_string();
    if (e.is_directory(ec)) { stdfs::create_directories(s.working() / to_path(rel), ec); s.dirs.insert(rel); }
    else {
      stdfs::create_directories((s.working() / to_path(rel)).parent_path(), ec);
      stdfs::copy_file(e.path(), s.working() / to_path(rel), stdfs::copy_options::overwrite_existing, ec);
      s.dirty.insert(rel);
    }
  }
  // files of the current generation that the restored one lacks must disappear
  std::function<void(const stdfs::path&, const std::string&)> walk = [&](const stdfs::path& d, const std::string& rel) {
    for (auto& e : stdfs::directory_iterator(d, ec)) {
      std::string name = from_path(e.path().filename());
      std::string child = rel.empty() ? name : rel + "/" + name;
      if (e.is_directory(ec)) walk(e.path(), child);
      if (!stdfs::exists(src / to_path(child), ec)) s.deleted.insert(child);
    }
  };
  walk(s.committed(), "");
  return s.do_commit(p_->hook);
}

Fs& instance() {
  static Fs fs;
  return fs;
}

}  // namespace hle::fs
