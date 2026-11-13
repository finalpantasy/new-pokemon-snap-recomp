// SPDX-License-Identifier: MIT
// Persistent + asynchronous graphics-pipeline compilation for the NVN layer.
//
//  * Every pipeline the game creates is identified by a PipeDesc (stage cache keys + state) and recorded in an append-only key list
//    (<shadercache>\pipeline_keys.bin). The VkPipelineCache is saved next to it every 30 s while new pipelines appear, so killing the
//    process loses at most 30 s (it used to be written only on a clean device teardown, which never happens when the window closes).
//  * At startup the recorded keys are compiled on a worker pool (shaderprecompile_begin / mode NOW or PLAYING) and stored in the ready map,
//    so the first draw that needs them finds a finished VkPipeline.
//  * A pipeline the game needs that is not ready compiles on the pool (hot queue first); the draw is skipped until it is ready
//    (SNAP_PIPELINE_ASYNC=0 compiles inline on the replay thread instead, the old behaviour).
//  * Files are versioned by game build, translator version, descriptor layout and (cache blob only) driver identity; a mismatch discards them.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "nvn_pipeline.h"
#include "os/perf.h"
#include "shaderprecompile.h"

namespace nvn {

namespace {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kMagicKeys = 0x4B504E53;    // 'SNPK'
constexpr uint32_t kMagicCache = 0x43504E53;   // 'SNPC'

struct FileHdr {
  uint32_t magic, version, descSize, pad;
  uint64_t translator, gameBuild;
  uint32_t vendor, device, driver, pad2;
  uint8_t uuid[16];
};

uint64_t Fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
  return h;
}
// In-memory map key only (never written to a file; entries compare the whole desc). Word-wise (opt 2026-11-13): the byte-wise
// FNV over 464 bytes ran once per draw in PipelineAcquire, the hottest loop of the replay thread in a PGO profile.
uint64_t HashDesc(const PipeDesc& pd) {
  static_assert(sizeof(PipeDesc) % 8 == 0, "word hash");
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&pd);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < sizeof pd; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 31; }
  return h;
}

enum EntState { ES_PENDING = 0, ES_READY = 1, ES_FAILED = 2, ES_NEEDS_INLINE = 3 };
struct Ent {
  PipeDesc pd;
  std::atomic<VkPipeline> pipe{VK_NULL_HANDLE};   // atomic: a stale rebuild on the replay thread can race a worker's StoreResult for the same entry
  std::atomic<int> state{ES_PENDING};
  Ent* next = nullptr;   // hash chain
  std::atomic<bool> hot{false};   // a hot-queue job exists (set when the replay thread first asks for an entry that BeginRun queued cold)
  std::atomic<bool> stale{false};  // built from a fragment template that lacked early fragment tests: rebuilt inline on the next acquire
};

struct Tmpl {
  std::once_flag once;
  StageProg sp;      // as loaded from the translation cache; never modified after the load
  // What readers (background compiles, GetLayoutFor) use. PatchTmplEft publishes a patched COPY here instead of editing \sp\ in place: a worker
  // reading mod / spv / fragMods through the old pointer keeps a valid, unchanging object (the old one is never freed while the service runs).
  std::atomic<StageProg*> cur{nullptr};
  bool ok = false;
  std::mutex em;     // guards eft and the module swap in PatchTmplEft
  bool eft = false;  // the module carries layout(early_fragment_tests)
};

struct Job { PipeDesc pd; bool hot; };

struct Svc {
  Host* h = nullptr;
  bool up = false;
  std::string dir;
  uint64_t translatorHash = 0, buildHash = 0;
  bool async = true;
  std::atomic<int> mode{SHADERPRECOMPILE_MODE_UNSET};

  // ready map
  std::mutex pm;
  std::unordered_map<uint64_t, Ent*> map;
  // stage templates, layouts
  std::mutex tm;
  std::unordered_map<std::string, Tmpl*> tmpls;
  std::unordered_map<std::string, std::string> fullKeys;   // key prefix -> full translation cache key
  std::unordered_set<std::string> eftKeys;                 // key prefixes whose program asks for early fragment tests (guarded by tm)
  std::mutex lm;
  std::unordered_map<std::string, ProgramImpl*> layouts;
  // job queues and workers
  std::mutex qm;
  std::condition_variable qcv;
  std::deque<Job> hot, cold;
  std::vector<std::thread> workers;
  std::atomic<int> activeLimit{2};
  int running = 0;
  bool stop = false;
  // key list
  std::mutex km;
  FILE* keysFile = nullptr;
  std::vector<PipeDesc> known;
  // progress of the current precompile run
  std::atomic<int> state{SHADERPRECOMPILE_IDLE};
  std::atomic<uint32_t> total{0}, done{0}, failed{0};
  std::atomic<uint64_t> runStartNs{0}, busyNs{0};
  std::atomic<bool> cancel{false};
  std::atomic<uint32_t> bgPending{0};
  // cache persistence
  std::atomic<bool> dirty{false};
  std::atomic<uint64_t> lastSaveNs{0};
  std::thread housekeeper;
  double avgMs = 25.0;   // measured average compile time of a precompile run (persisted in stats.txt)
};
Svc* S = nullptr;
std::mutex g_early;                // guards the early requests below
bool g_wantBegin = false;
int g_earlyMode = -1;
std::string g_buildId = "DDB900FF04A912ED42430508389853CFAF1ACCCE";

std::string Dir() {
  const char* e = getenv("SNAP_SHADERCACHE_DIR");
  std::string d;
  if (e && *e) d = e;
  else if ((e = getenv("SNAP_CONFIG_DIR")) && *e) d = std::string(e) + "\\shadercache";
  else {
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    d = (n ? std::string(buf) : std::string(".")) + "\\SnapRecomp\\shadercache";
  }
  if (const char* r = getenv("SNAP_XLATE_REV"); r && *r && strcmp(r, "0")) d += std::string("_r") + r;   // translator revision: its own pipeline keys and VkPipelineCache
  for (size_t i = 3; i < d.size(); i++)
    if (d[i] == '\\' || d[i] == '/') CreateDirectoryA(d.substr(0, i).c_str(), nullptr);
  CreateDirectoryA(d.c_str(), nullptr);
  return d;
}

FileHdr MakeHdr(Host* h, uint32_t magic, bool withDriver) {
  FileHdr x{};
  x.magic = magic; x.version = kFormatVersion; x.descSize = (uint32_t)sizeof(PipeDesc);
  x.translator = S->translatorHash; x.gameBuild = S->buildHash;
  if (withDriver) {
    x.vendor = h->props.vendorID; x.device = h->props.deviceID; x.driver = h->props.driverVersion;
    memcpy(x.uuid, h->props.pipelineCacheUUID, 16);
  }
  return x;
}
bool HdrMatches(const FileHdr& a, const FileHdr& b) { return memcmp(&a, &b, sizeof a) == 0; }

double NowMs() { return (double)snap_perf::now_ns() / 1e6; }

// ---- templates and layouts ------------------------------------------------------------------------------------------------------
// Early fragment tests on background templates. BuildStage adds layout(early_fragment_tests) when the program's control section asks for
// it (EvaluationAggregatePS) and leaves a <key>.efz marker so cache loads can do the same. Test runs mirror the shader cache from
// build\snap\nvn_cache, which has no markers, so the startup precompile could load the Aggregate template before the game rebuilt the
// marker: its pipeline then ran the stencil test after the shader's atomics and every visibility counter read 518400 (all of 1920x1080 / 4),
// so no photo passed. Now BuildStage also tells the service directly (eftKeys); a template loaded too early is patched and the pipelines
// built from it are rebuilt inline. NVN_NO_EFT_FIX=1 restores the old marker-only behavior.
bool EftFixOn() { static const bool on = getenv("NVN_NO_EFT_FIX") == nullptr; return on; }

void PatchTmplEft(Tmpl* t, const char* key12) {
  {
    std::lock_guard<std::mutex> lk(t->em);
    StageProg* base = t->cur.load(std::memory_order_acquire);
    if (t->eft || !t->ok || !base || base->stage != 1 || base->spv.empty()) return;
    std::vector<uint32_t> words = base->spv;
    if (!SpvAddEarlyFragmentTests(words)) return;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4; ci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(S->h->device, &ci, nullptr, &mod) != VK_SUCCESS) return;
    // Copy field by field (not "*np = *base": copying base->fragMods would race a worker inserting a variant). fragMods starts empty: the old
    // variants were built from the module without early fragment tests. A field added to StageProg must be added here.
    StageProg* np = new StageProg;
    np->valid = base->valid; memcpy(np->key, base->key, sizeof np->key); memcpy(np->fkey, base->fkey, sizeof np->fkey); np->stage = base->stage;
    np->cb = base->cb; np->sb = base->sb; np->tex = base->tex; np->img = base->img; np->fragOutputMap = base->fragOutputMap;
    np->inputMask = base->inputMask; np->outLocs = base->outLocs; np->constBuf = base->constBuf; np->constMem = base->constMem; np->constSize = base->constSize;
    np->mod = mod;   // the old module is left alive on purpose: a worker may be compiling with it right now (one module per patched stage)
    np->spv = std::move(words);
    t->cur.store(np, std::memory_order_release);
    t->eft = true;
  }
  int n = 0;
  {
    std::lock_guard<std::mutex> lk(S->pm);
    for (auto& kv : S->map)
      for (Ent* e = kv.second; e; e = e->next)
        if (!strncmp(e->pd.stage[1], key12, 12)) { e->stale = true; n++; }
  }
  Log("pipeline service: fragment template %.12s patched with early fragment tests, %d pipelines rebuilt on next use", key12, n);
}

Tmpl* GetTmpl(const char* key12) {
  Tmpl* t;
  std::string full;
  {
    std::lock_guard<std::mutex> lk(S->tm);
    auto& slot = S->tmpls[key12];
    if (!slot) slot = new Tmpl;
    t = slot;
    auto it = S->fullKeys.find(key12);
    if (it != S->fullKeys.end()) full = it->second;
  }
  if (full.empty() && S->h) {   // startup precompile: the game has not built this stage yet, find its translation in the shader cache by the 12-hex prefix
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA((S->h->cacheDir + "\\" + key12 + "*.spv").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
      std::string n = fd.cFileName;
      if (n.size() > 4) full = n.substr(0, n.size() - 4);
      FindClose(hf);
    }
  }
  std::call_once(t->once, [&] {
    if (full.empty()) return;
    bool want, applied = false;
    { std::lock_guard<std::mutex> lk(S->tm); want = EftFixOn() && S->eftKeys.count(key12) != 0; }
    t->ok = LoadStageFromCache(S->h, full, &t->sp, want, &applied);
    t->cur.store(&t->sp, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(t->em); t->eft = applied; }
    bool late;   // the game registered the program as early-fragment-tests while this load ran
    { std::lock_guard<std::mutex> lk(S->tm); late = t->ok && EftFixOn() && S->eftKeys.count(key12) != 0; }
    if (late) PatchTmplEft(t, key12);
  });
  return t->ok ? t : nullptr;
}

ProgramImpl* GetLayoutFor(const PipeDesc& pd, StageProg* out[6]) {
  std::string key;
  Tmpl* ts[5] = {};
  for (int s = 0; s < 5; s++) {
    out[s] = nullptr;
    if (!pd.stage[s][0]) continue;
    ts[s] = GetTmpl(pd.stage[s]);
    if (!ts[s]) return nullptr;
    out[s] = ts[s]->cur.load(std::memory_order_acquire);
    key += pd.stage[s]; key += '|';
  }
  out[5] = nullptr;
  std::lock_guard<std::mutex> lk(S->lm);
  auto it = S->layouts.find(key);
  if (it != S->layouts.end()) return it->second;
  auto* p = new ProgramImpl;
  p->host = S->h; p->composite = true;
  for (int s = 0; s < 5; s++) if (ts[s]) p->st[s] = *ts[s]->cur.load(std::memory_order_acquire);
  if (!ProgramBuildLayout(p)) { delete p; S->layouts[key] = nullptr; return nullptr; }
  S->layouts[key] = p;
  return p;
}

// ---- key list --------------------------------------------------------------------------------------------------------------------
void LoadKeys() {
  const std::string path = S->dir + "\\pipeline_keys.bin";
  const FileHdr want = MakeHdr(S->h, kMagicKeys, false);
  bool good = false;
  {
    std::ifstream f(path, std::ios::binary);
    FileHdr got{};
    if (f && f.read((char*)&got, sizeof got) && HdrMatches(got, want)) {
      good = true;
      PipeDesc pd;
      std::unordered_set<uint64_t> seen;
      uint64_t nGood = 0;
      // Records are fixed size, so a torn tail (the process was killed mid-write) would shift every record appended after it. Stop at the first
      // short or implausible record and cut the file there before anything is appended.
      while (f.read((char*)&pd, sizeof pd)) {
        bool ok = pd.nattr >= 0 && pd.nattr <= 16 && pd.nstream >= 0 && pd.nstream <= 16 && pd.nColor >= 0 && pd.nColor <= 8;
        for (int s = 0; s < 5 && ok; s++) ok = pd.stage[s][15] == 0;
        if (!ok) break;
        nGood++;
        // [vkval] keys recorded before the compressed-render-target stand-in existed carry a block-compressed colour format (BC1 photo
        // thumbnails): Vulkan cannot render to those (VUID-VkGraphicsPipelineCreateInfo-renderPass-06582 at startup), and the live draw
        // now uses the stand-in format, so the old key is never hit. Do not precompile it.
        bool compressedRt = false;
        for (int i = 0; i < pd.nColor; i++) if (pd.colorFmt[i] >= 131 && pd.colorFmt[i] <= 184) compressedRt = true;   // VK_FORMAT_BC1_RGB..ASTC_12x12
        if (compressedRt) continue;
        if (seen.insert(HashDesc(pd)).second) S->known.push_back(pd);
      }
      f.close();
      const uint64_t goodLen = sizeof(FileHdr) + nGood * sizeof(PipeDesc);
      std::error_code ec;
      const uint64_t have = std::filesystem::file_size(path, ec);
      if (!ec && have != goodLen) {
        std::filesystem::resize_file(path, goodLen, ec);
        Log("pipeline keys: cut %llu stray byte(s) after record %llu%s", (unsigned long long)(have - goodLen), (unsigned long long)nGood, ec ? " (resize failed)" : "");
        if (ec) good = false;   // cannot repair: start a fresh list rather than append after a torn record
      }
    }
  }
  if (!good) {   // missing or from another build / translator / descriptor layout: start a fresh list
    S->known.clear();
    S->keysFile = fopen(path.c_str(), "wb");
    if (S->keysFile) { fwrite(&want, sizeof want, 1, S->keysFile); fflush(S->keysFile); }
  } else {
    S->keysFile = fopen(path.c_str(), "ab");
  }
}
void RecordKey(const PipeDesc& pd) {
  std::lock_guard<std::mutex> lk(S->km);
  if (!S->keysFile) return;
  fwrite(&pd, sizeof pd, 1, S->keysFile);
  fflush(S->keysFile);
}

// ---- pipeline cache persistence ---------------------------------------------------------------------------------------------------
void LoadCache(Host* h) {
  std::ifstream f(S->dir + "\\pipeline_cache.bin", std::ios::binary);
  FileHdr got{};
  const FileHdr want = MakeHdr(h, kMagicCache, true);
  if (!f || !f.read((char*)&got, sizeof got) || !HdrMatches(got, want)) return;   // other driver / build / translator: the blob is useless
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.empty()) return;
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = data.size(); ci.pInitialData = data.data();
  VkPipelineCache pc = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(h->device, &ci, nullptr, &pc) == VK_SUCCESS) {
    if (h->pipelineCache) vkDestroyPipelineCache(h->device, h->pipelineCache, nullptr);
    h->pipelineCache = pc;
    Log("pipeline cache loaded (%zu bytes)", data.size());
  }
}
void SaveCache(bool force) {
  Host* h = S->h;
  if (!h->pipelineCache || (!force && !S->dirty.exchange(false))) return;
  size_t n = 0;
  if (vkGetPipelineCacheData(h->device, h->pipelineCache, &n, nullptr) != VK_SUCCESS || !n) return;
  std::vector<char> data(n);
  if (vkGetPipelineCacheData(h->device, h->pipelineCache, &n, data.data()) != VK_SUCCESS) return;
  // The housekeeper and the thread finishing a precompile run can both get here: each writes its own tmp file, then renames it into place
  // (MoveFileEx replace is atomic), so the cache file is always one writer's complete blob.
  static std::atomic<unsigned> saveSeq{0};
  const std::string path = S->dir + "\\pipeline_cache.bin",
                    tmp = path + "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(saveSeq.fetch_add(1)) + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    const FileHdr hdr = MakeHdr(h, kMagicCache, true);
    f.write((const char*)&hdr, sizeof hdr);
    f.write(data.data(), (std::streamsize)n);
    f.flush();
    if (!f.good()) { f.close(); DeleteFileA(tmp.c_str()); return; }
  }
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp.c_str()); return; }
  S->lastSaveNs = snap_perf::now_ns();
  Log("pipeline cache saved (%zu bytes)", n);
}

// ---- compile jobs -----------------------------------------------------------------------------------------------------------------
void StoreResult(Ent* e, VkPipeline pipe, int failState) {
  e->pipe = pipe;
  e->state.store(pipe ? ES_READY : failState, std::memory_order_release);
}

Ent* FindOrNull(const PipeDesc& pd, uint64_t hsh) {   // caller holds pm
  auto it = S->map.find(hsh);
  for (Ent* e = it == S->map.end() ? nullptr : it->second; e; e = e->next)
    if (memcmp(&e->pd, &pd, sizeof pd) == 0) return e;
  return nullptr;
}

void RunJob(const Job& j) {
  const uint64_t hsh = HashDesc(j.pd);
  Ent* e;
  { std::lock_guard<std::mutex> lk(S->pm); e = FindOrNull(j.pd, hsh); }
  if (!e || e->state.load() == ES_READY) return;
  StageProg* st[6];
  ProgramImpl* lay = GetLayoutFor(j.pd, st);
  VkPipeline pipe = VK_NULL_HANDLE;
  if (lay) pipe = CreateGraphicsPipeline(S->h, j.pd, st, lay->layout);
  StoreResult(e, pipe, ES_NEEDS_INLINE);   // a failed background build is retried inline by the replay thread (it has the real program)
  if (pipe) S->dirty = true;
}

void Worker() {
  for (;;) {
    Job j;
    bool isHot;
    {
      std::unique_lock<std::mutex> lk(S->qm);
      for (;;) {
        if (S->stop) return;
        const bool canRun = S->running < S->activeLimit.load() || !S->hot.empty();
        if (canRun && (!S->hot.empty() || (!S->cold.empty() && !S->cancel.load()))) break;
        if (S->cancel.load() && !S->cold.empty()) { S->cold.clear(); }
        S->qcv.wait_for(lk, std::chrono::milliseconds(200));
      }
      isHot = !S->hot.empty();
      if (isHot) { j = S->hot.front(); S->hot.pop_front(); }
      else { j = S->cold.front(); S->cold.pop_front(); }
      S->running++;
    }
    SetThreadPriority(GetCurrentThread(), isHot ? THREAD_PRIORITY_NORMAL
                                                : (S->mode.load() == SHADERPRECOMPILE_MODE_NOW ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL));
    const uint64_t t0 = snap_perf::now_ns();
    RunJob(j);
    const uint64_t dt = snap_perf::now_ns() - t0;
    if (isHot) { if (S->bgPending.load()) S->bgPending--; }
    else {
      S->busyNs += dt;
      const uint32_t d = ++S->done;
      if (d >= S->total.load() && S->state.load() == SHADERPRECOMPILE_RUNNING) {
        S->state = SHADERPRECOMPILE_DONE;
        const double wallMs = (double)(snap_perf::now_ns() - S->runStartNs.load()) / 1e6;
        char det[96]; snprintf(det, sizeof det, "%u pipelines", d);
        snap_perf::event("precompile_done", wallMs, det);
        if (d) S->avgMs = (double)S->busyNs.load() / 1e6 / d;
        SaveCache(true);
        std::ofstream st(S->dir + "\\stats.txt", std::ios::trunc);
        st << S->avgMs << "\n";
      }
    }
    { std::lock_guard<std::mutex> lk(S->qm); S->running--; }
    S->qcv.notify_all();
  }
}

int WorkerCountFor(int mode) {
  if (const char* e = getenv("SNAP_PIPELINE_THREADS")) { int n = atoi(e); if (n > 0) return std::min(n, 32); }
  const int hw = (int)std::max(2u, std::thread::hardware_concurrency());
  if (mode == SHADERPRECOMPILE_MODE_NOW) return std::clamp(hw / 2, 2, 16);
  return std::clamp(hw / 8, 2, 4);
}
void EnsureWorkers(int want) {
  std::lock_guard<std::mutex> lk(S->qm);
  S->activeLimit = want;
  while ((int)S->workers.size() < want) S->workers.emplace_back(Worker);
}

void Housekeeping() {
  while (true) {
    for (int i = 0; i < 50; i++) {
      Sleep(100);
      std::lock_guard<std::mutex> lk(S->qm);
      if (S->stop) return;
    }
    if (S->dirty.load() && (snap_perf::now_ns() - S->lastSaveNs.load()) > 30ull * 1000000000ull) SaveCache(false);
  }
}

int BeginRun() {
  if (!S || !S->up) { std::lock_guard<std::mutex> lk(g_early); g_wantBegin = true; return 0; }
  std::vector<PipeDesc> todo, knownCopy;
  { std::lock_guard<std::mutex> lk(S->km); knownCopy = S->known; }
  {
    std::lock_guard<std::mutex> lk(S->pm);
    for (const PipeDesc& pd : knownCopy) {
      const uint64_t hsh = HashDesc(pd);
      if (FindOrNull(pd, hsh)) continue;
      Ent* e = new Ent; memcpy(&e->pd, &pd, sizeof pd);
      e->next = S->map.count(hsh) ? S->map[hsh] : nullptr;
      S->map[hsh] = e;
      todo.push_back(pd);
    }
  }
  S->cancel = false;
  S->done = 0; S->failed = 0; S->busyNs = 0;
  S->total = (uint32_t)todo.size();
  S->runStartNs = snap_perf::now_ns();
  if (todo.empty()) { S->state = SHADERPRECOMPILE_DONE; return 0; }
  S->state = SHADERPRECOMPILE_RUNNING;
  EnsureWorkers(WorkerCountFor(S->mode.load()));
  {
    std::lock_guard<std::mutex> lk(S->qm);
    for (auto& pd : todo) S->cold.push_back({pd, false});
  }
  S->qcv.notify_all();
  return (int)todo.size();
}

}  // namespace

// ---- public service API -----------------------------------------------------------------------------------------------------------
void PipelineRegisterStageKey(const char* key12, const char* fullKey, bool eft) {
  if (!S) return;
  Tmpl* t = nullptr;
  {
    std::lock_guard<std::mutex> lk(S->tm);
    S->fullKeys.emplace(key12, fullKey);
    if (eft && EftFixOn()) {
      S->eftKeys.insert(key12);
      auto it = S->tmpls.find(key12);
      if (it != S->tmpls.end()) t = it->second;
    }
  }
  if (t && t->ok) PatchTmplEft(t, key12);   // loaded before the game built the program (a load still running re-checks eftKeys itself)
}

void PipelineServiceInit(Host* h) {
  if (S) return;
  S = new Svc;
  S->h = h;
  S->dir = Dir();
  S->translatorHash = Fnv(TranslatorVersion(), strlen(TranslatorVersion()));
  const char* b = getenv("SNAP_GAME_BUILD_ID");
  S->buildHash = Fnv(b && *b ? b : g_buildId.c_str(), strlen(b && *b ? b : g_buildId.c_str()));
  const char* as = getenv("SNAP_PIPELINE_ASYNC");
  S->async = !(as && *as == '0');
  // mode: env override, else <dir>\mode.txt
  int mode = SHADERPRECOMPILE_MODE_UNSET;
  if (const char* e = getenv("SNAP_SHADER_PRECOMPILE")) mode = !strcmp(e, "now") ? SHADERPRECOMPILE_MODE_NOW : !strcmp(e, "playing") ? SHADERPRECOMPILE_MODE_PLAYING : mode;
  else {
    std::ifstream f(S->dir + "\\mode.txt");
    std::string w;
    if (f >> w) mode = w == "now" ? SHADERPRECOMPILE_MODE_NOW : w == "playing" ? SHADERPRECOMPILE_MODE_PLAYING : mode;
  }
  { std::lock_guard<std::mutex> lk(g_early); if (g_earlyMode >= 0) mode = g_earlyMode; }
  S->mode = mode;
  { std::ifstream f(S->dir + "\\stats.txt"); double v; if (f >> v && v > 0.1 && v < 5000) S->avgMs = v; }
  // enumerate the translation cache so background jobs can load any stage by its key prefix
  {
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA((h->cacheDir + "\\*.json").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
      do {
        std::string n = fd.cFileName;
        if (n.size() == 64 + 5) S->fullKeys.emplace(n.substr(0, 12), n.substr(0, 64));
      } while (FindNextFileA(hf, &fd));
      FindClose(hf);
    }
  }
  LoadCache(h);
  LoadKeys();
  S->lastSaveNs = snap_perf::now_ns();
  S->housekeeper = std::thread(Housekeeping);
  S->up = true;
  Log("pipeline service: %zu recorded pipelines, mode %d, async %d, dir %s", S->known.size(), mode, (int)S->async, S->dir.c_str());
  bool begin;
  { std::lock_guard<std::mutex> lk(g_early); begin = g_wantBegin; g_wantBegin = false; }
  // NOW and PLAYING both start the run at boot; only NOW holds the game. UNSET waits for the UI's choice (or an explicit begin()).
  if (begin || mode == SHADERPRECOMPILE_MODE_NOW || mode == SHADERPRECOMPILE_MODE_PLAYING) BeginRun();
}

void PipelineServiceShutdown(Host* h) {
  if (!S) return;
  { std::lock_guard<std::mutex> lk(S->qm); S->stop = true; S->cancel = true; }
  S->qcv.notify_all();
  for (auto& t : S->workers) if (t.joinable()) t.join();
  if (S->housekeeper.joinable()) S->housekeeper.join();
  SaveCache(true);
  for (auto& kv : S->map)
    for (Ent* e = kv.second; e;) { Ent* n = e->next; if (e->pipe) vkDestroyPipeline(h->device, e->pipe, nullptr); delete e; e = n; }
  S->map.clear();
  for (auto& kv : S->layouts) if (kv.second) { ProgramDestroy(kv.second); delete kv.second; }
  for (auto& kv : S->tmpls) {
    Tmpl* t = kv.second;
    StageProg* c = t->cur.load();
    if (t->ok && t->sp.mod) vkDestroyShaderModule(h->device, t->sp.mod, nullptr);
    if (c && c != &t->sp) { if (c->mod) vkDestroyShaderModule(h->device, c->mod, nullptr); delete c; }   // the PatchTmplEft copy
    delete t;
  }
  if (S->keysFile) fclose(S->keysFile);
  S->up = false;
}

VkPipeline PipelineAcquire(Host* h, const PipeDesc& pd, ProgramImpl* p, bool* pending, bool mustHave) {
  if (!S || !S->up) return VK_NULL_HANDLE;
  const uint64_t hsh = HashDesc(pd);
  Ent* e;
  bool isNew = false;
  {
    std::lock_guard<std::mutex> lk(S->pm);
    e = FindOrNull(pd, hsh);
    if (!e) {
      e = new Ent; memcpy(&e->pd, &pd, sizeof pd);
      auto it = S->map.find(hsh);
      e->next = it == S->map.end() ? nullptr : it->second;
      S->map[hsh] = e;
      isNew = true;
    }
  }
  if (!isNew) {
    int st = e->state.load(std::memory_order_acquire);
    if (st == ES_PENDING && mustHave && S->async) {   // wait for the background compile (hot jobs run first); give up after 10 s
      if (!e->hot.exchange(true)) {
        S->bgPending++;
        { std::lock_guard<std::mutex> lk(S->qm); S->hot.push_back({pd, true}); }
        S->qcv.notify_one();
      }
      for (int i = 0; i < 10000 && (st = e->state.load(std::memory_order_acquire)) == ES_PENDING; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (st == ES_READY && e->stale.load(std::memory_order_acquire) && p) {   // see PatchTmplEft: rebuild with the game's own (patched) stages
      StageProg* sts[6] = {};
      for (int s = 0; s < 5; s++) sts[s] = p->st[s].valid ? &p->st[s] : nullptr;
      if (VkPipeline pipe = CreateGraphicsPipeline(h, pd, sts, p->layout)) { e->stale = false; e->pipe = pipe; S->dirty = true; }   // old pipeline leaked: may be in flight
      return e->pipe;
    }
    if (st == ES_READY) return e->pipe;
    if (st == ES_PENDING) {
      if (S->async && !e->hot.exchange(true)) {   // queued cold by the precompile run, but the game needs it now: add a hot job
        S->bgPending++;
        { std::lock_guard<std::mutex> lk(S->qm); S->hot.push_back({pd, true}); }
        S->qcv.notify_one();
      }
      *pending = true;
      return VK_NULL_HANDLE;
    }
    if (st == ES_FAILED) return VK_NULL_HANDLE;
    // ES_NEEDS_INLINE: the background build failed (e.g. a stage missing from the translation cache): build on this thread with the real program
    StageProg* sts[6] = {};
    for (int s = 0; s < 5; s++) sts[s] = p->st[s].valid ? &p->st[s] : nullptr;
    VkPipeline pipe = CreateGraphicsPipeline(h, pd, sts, p->layout);
    StoreResult(e, pipe, ES_FAILED);
    if (pipe) S->dirty = true;
    return pipe;
  }
  // first request of this pipeline in this run
  bool known = false;
  {
    // recorded earlier (a previous run) pipelines are already in the map from BeginRun; reaching here means it is genuinely new
    (void)known;
    RecordKey(pd);
    std::lock_guard<std::mutex> lk(S->km);
    S->known.push_back(pd);
  }
  if (S->async && !mustHave) {
    EnsureWorkers(std::max(S->activeLimit.load(), WorkerCountFor(S->mode.load())));
    e->hot = true;
    S->bgPending++;
    { std::lock_guard<std::mutex> lk(S->qm); S->hot.push_back({pd, true}); }
    S->qcv.notify_one();
    *pending = true;
    return VK_NULL_HANDLE;
  }
  StageProg* sts[6] = {};
  for (int s = 0; s < 5; s++) sts[s] = p->st[s].valid ? &p->st[s] : nullptr;
  VkPipeline pipe = CreateGraphicsPipeline(h, pd, sts, p->layout);
  StoreResult(e, pipe, ES_FAILED);
  if (pipe) S->dirty = true;
  return pipe;
}

bool PipelineHoldGame() {
  return S && S->up && S->mode.load() == SHADERPRECOMPILE_MODE_NOW && S->state.load() == SHADERPRECOMPILE_RUNNING && !S->cancel.load();
}

}  // namespace nvn

// ---- C API ----------------------------------------------------------------------------------------------------------------------
using namespace nvn;
extern "C" {

int shaderprecompile_begin(void) { return BeginRun(); }
int shaderprecompile_state(void) { return S ? S->state.load() : SHADERPRECOMPILE_IDLE; }
void shaderprecompile_progress(uint32_t* done, uint32_t* total, double* eta) {
  const uint32_t d = S ? S->done.load() : 0, t = S ? S->total.load() : 0;
  if (done) *done = d;
  if (total) *total = t;
  if (eta) {
    *eta = -1;
    if (S && d > 0 && S->state.load() == SHADERPRECOMPILE_RUNNING) {
      const double el = (double)(snap_perf::now_ns() - S->runStartNs.load()) / 1e9;
      *eta = el / d * (t - d);
    } else if (S && S->state.load() == SHADERPRECOMPILE_DONE) *eta = 0;
  }
}
void shaderprecompile_cancel(void) {
  if (!S) return;
  S->cancel = true;
  if (S->state.load() == SHADERPRECOMPILE_RUNNING) S->state = SHADERPRECOMPILE_CANCELLED;
  S->qcv.notify_all();
}
int shaderprecompile_get_mode(void) { return S ? S->mode.load() : SHADERPRECOMPILE_MODE_UNSET; }
void shaderprecompile_set_mode(int mode) {
  if (mode < 0 || mode > 2) return;
  if (!S || !S->up) { std::lock_guard<std::mutex> lk(g_early); g_earlyMode = mode; return; }
  S->mode = mode;
  std::ofstream f(S->dir + "\\mode.txt", std::ios::trunc);
  f << (mode == SHADERPRECOMPILE_MODE_NOW ? "now" : mode == SHADERPRECOMPILE_MODE_PLAYING ? "playing" : "unset") << "\n";
  if (mode == SHADERPRECOMPILE_MODE_NOW && S->state.load() != SHADERPRECOMPILE_RUNNING) BeginRun();
}
double shaderprecompile_estimate_seconds(void) {
  if (!S) return -1;
  const int thr = WorkerCountFor(SHADERPRECOMPILE_MODE_NOW);
  size_t todo = S->total.load() ? (S->state.load() == SHADERPRECOMPILE_RUNNING ? S->total.load() - S->done.load() : S->total.load()) : S->known.size();
  return (double)todo * S->avgMs / 1000.0 / std::max(thr, 1);
}
int shaderprecompile_is_holding_game(void) { return PipelineHoldGame() ? 1 : 0; }
uint32_t shaderprecompile_background_pending(void) { return S ? S->bgPending.load() : 0; }

}  // extern "C"
