// SPDX-License-Identifier: MIT
// NVN program objects: GLSLC program binaries -> SPIR-V -> VkShaderModule + descriptor layouts.
//
// Translation path (docs/nvn-vulkan-design.md, "Shader pipeline"): the game's shaders are Maxwell SASS inside the
// GLSLC binary (the NVN driver never sees GLSL at run time). The SASS is translated by the Ryujinx shader
// translator (MIT), driven by scripts/shader_harness (dotnet) in --nvn mode, and the result is cached on disk keyed
// by SHA-256 of the program's data section. Nothing of the game is stored in the repository: the cache lives under
// the layer's cache directory (NVN_CACHE_DIR, default <exe dir>\nvn_cache).
//
// Control/data section facts used here come from analysis/shaders/nushdb_layout.md (derived from the game's own
// files): control+0x6F8 program size, +0x700 constant-data offset, +0x704 data size, +0x708 header size (0x30);
// the data section starts with 0x12345678 and, for graphics stages, the Maxwell SPH at +0x30. Compute programs
// have neither.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <mutex>
#include <unordered_set>

#include "nvn_aa.h"
#include "nvn_internal.h"
#include "nvn_pipeline.h"
#include "os/perf.h"   // SNAP_PERF instrumentation

namespace nvn {
namespace {

// SNAP_XLATE_REV=<n> switches to translator revision n (patched Ryujinx translator, scripts/xlate_patch.sh): the revision joins the cache salt, so
// old and new outputs never share a key, and the nvn_cache and shadercache folders get a _r<n> suffix (nvn_vk.cpp, nvn_pipeline.cpp).
// Unset or 0 = the original translation, unchanged.
const std::string& TranslatorSaltStr() {
  static const std::string s = [] {
    std::string b = "nvn-xlate-v2|binding-model-1|interlock-off|clip7";
    const char* r = getenv("SNAP_XLATE_REV");
    if (r && *r && strcmp(r, "0")) b += std::string("|xlate-r") + r;
    return b;
  }();
  return s;
}

// Shader modules whose SPIR-V uses subgroup operations (GroupNonUniform*, SubgroupLocalInvocationId and the lane masks): their lane math
// assumes 32 lanes, so the pipeline builders pin them to a required subgroup size of 32 (nvn_draw.cpp, g_sgPinStages).
std::mutex g_sgMu;
std::unordered_set<uint64_t> g_sgMods;
bool SpvUsesSubgroup(const std::vector<uint32_t>& w) {
  for (size_t i = 5; i < w.size();) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (!wc || i + wc > w.size()) return false;
    if (op >= 333 && op <= 366) return true;   // OpGroupNonUniform*
    if (op == 71 && wc >= 4 && w[i + 2] == 11 && (w[i + 3] == 41 || (w[i + 3] >= 4416 && w[i + 3] <= 4420))) return true;   // BuiltIn SubgroupLocalInvocationId, SubgroupEq/Ge/Gt/Le/LtMask
    i += wc;
  }
  return false;
}
void NoteSubgroup(VkShaderModule m, const std::vector<uint32_t>& w) {
  if (!SpvUsesSubgroup(w)) return;
  std::lock_guard<std::mutex> l(g_sgMu);
  g_sgMods.insert((uint64_t)(uintptr_t)m);
}

// ---- SHA-256 (BCrypt) ---------------------------------------------------------------------------------------------
std::string Sha256Hex(const uint8_t* d, size_t n, const std::string& extra) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  std::string out;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return out;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
    BCryptHashData(h, const_cast<PUCHAR>(d), (ULONG)n, 0);
    BCryptHashData(h, (PUCHAR)extra.data(), (ULONG)extra.size(), 0);
    uint8_t dig[32] = {};
    BCryptFinishHash(h, dig, 32, 0);
    char hx[65];
    for (int i = 0; i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", dig[i]);
    out.assign(hx, 64);
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return out;
}

// ---- tiny JSON reader (the manifest is machine-written, numbers only) ---------------------------------------------
struct J {
  char type = 0;  // 'n' number, 'a' array, 'o' object
  double num = 0;
  std::vector<J> arr;
  std::vector<std::pair<std::string, J>> obj;
  const J* get(const char* k) const {
    for (auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  int i(const char* k, int def = 0) const { auto* v = get(k); return v ? (int)v->num : def; }
};
struct JP {
  const char* p; const char* e;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool parse(J& out) {
    ws();
    if (p >= e) return false;
    if (*p == '{') {
      p++; out.type = 'o';
      for (;;) {
        ws();
        if (p < e && *p == '}') { p++; return true; }
        if (p < e && *p == ',') { p++; continue; }
        if (p >= e || *p != '"') return false;
        const char* s = ++p;
        while (p < e && *p != '"') p++;
        std::string k(s, p); p++;
        ws();
        if (p >= e || *p != ':') return false;
        p++;
        J v;
        if (!parse(v)) return false;
        out.obj.emplace_back(k, std::move(v));
      }
    }
    if (*p == '[') {
      p++; out.type = 'a';
      for (;;) {
        ws();
        if (p < e && *p == ']') { p++; return true; }
        if (p < e && *p == ',') { p++; continue; }
        J v;
        if (!parse(v)) return false;
        out.arr.push_back(std::move(v));
      }
    }
    char* end = nullptr;
    out.type = 'n';
    out.num = strtod(p, &end);
    if (end == p) return false;
    p = end;
    return true;
  }
};

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}
bool WriteFile(const std::string& path, const void* d, size_t n) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(static_cast<const char*>(d), (std::streamsize)n);
  return f.good();
}

// Run the dotnet shader harness (BelowNormal priority, 10 minute cap). Returns true when the manifest exists.
bool RunHarness(const std::string& bin, const std::string& spv, const std::string& json, int stage, uint32_t constOff,
                uint32_t codeOff) {
  std::string dll;
#ifdef NVN_HARNESS_DLL
  dll = NVN_HARNESS_DLL;
#endif
  if (const char* e = getenv("SNAP_XLATE_REV"); e && *e && strcmp(e, "0")) {   // revision n: build\xlate\r<n>\h (scripts/xlate_build.ps1), under the same project root as the default harness
    size_t k = dll.find("tools\\shader_harness");
    if (k == std::string::npos) k = dll.find("tools/shader_harness");
    if (k != std::string::npos) dll = dll.substr(0, k) + "build\\xlate\\r" + e + "\\h\\bin\\Release\\net10.0\\ShaderHarness.dll";
  }
  if (const char* e = getenv("NVN_SHADER_HARNESS")) dll = e;
  if (dll.empty()) { LogError("shader translation needs the harness: set NVN_SHADER_HARNESS=<ShaderHarness.dll>"); return false; }
  std::ostringstream cl;
  cl << "dotnet \"" << dll << "\" --nvn \"" << bin << "\" \"" << spv << "\" \"" << json << "\" " << stage << " " << constOff << " "
     << codeOff;
  std::string cmd = cl.str();
  STARTUPINFOA si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    LogError("cannot start dotnet (%lu)", GetLastError());
    return false;
  }
  DWORD w = WaitForSingleObject(pi.hProcess, 600000);
  DWORD code = 1;
  if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 1);   // our own child only
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  if (code != 0) { LogError("shader harness failed (exit %lu)", code); return false; }
  return GetFileAttributesA(json.c_str()) != INVALID_FILE_ATTRIBUTES;
}

VkShaderStageFlagBits StageBit(int nvnStage) {
  static const VkShaderStageFlagBits t[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                             VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                             VK_SHADER_STAGE_COMPUTE_BIT};
  return t[nvnStage];
}

uint32_t Rd32(const void* p, size_t o) { uint32_t v; memcpy(&v, static_cast<const uint8_t*>(p) + o, 4); return v; }

void FreeStage(Host* h, StageProg& s) {
  if (s.mod) vkDestroyShaderModule(h->device, s.mod, nullptr);
  // fragMods variants are owned by the host (Host::variantMods): composite programs hold copies of this map
  if (s.constBuf) vkDestroyBuffer(h->device, s.constBuf, nullptr);
  if (s.constMem) vkFreeMemory(h->device, s.constMem, nullptr);
  s = StageProg{};
}

bool BuildStage(Host* h, const ShaderData& sd, StageProg* out) {
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(sd.data, &pool, &off) || !pool->storage) {
    LogError("ProgramSetShaders: shader address 0x%llx is not in a pool", (unsigned long long)sd.data);
    return false;
  }
  if (!sd.control) { LogError("ProgramSetShaders: null control section"); return false; }
  const uint8_t* data = static_cast<const uint8_t*>(pool->storage) + off;
  const uint32_t progSize = Rd32(sd.control, 0x6F8), constOff = Rd32(sd.control, 0x700), dataSize = Rd32(sd.control, 0x704),
                 hdrSize = Rd32(sd.control, 0x708);
  if (dataSize == 0 || dataSize > (16u << 20) || constOff > dataSize || progSize > dataSize) {
    LogError("ProgramSetShaders: bad control section (size 0x%x const 0x%x)", dataSize, constOff);
    return false;
  }
  bool compute = Rd32(data, 0) != 0x12345678u;
  int stage = 5;
  uint32_t codeOff = 0;
  if (!compute) {
    codeOff = hdrSize;
    const uint32_t w0 = Rd32(data, codeOff);
    switch ((w0 >> 10) & 15) {   // SPH stage field [analysis/shaders/nushdb_layout.md]
      case 1: stage = 0; break;
      case 2: stage = 3; break;
      case 3: stage = 4; break;
      case 4: stage = 2; break;
      case 5: stage = 1; break;
      default: LogError("ProgramSetShaders: unknown SPH stage %u", (w0 >> 10) & 15); return false;
    }
  }
  std::string key = Sha256Hex(data, dataSize, TranslatorSaltStr() + "|" + std::to_string(stage) + "|" + std::to_string(codeOff));
  if (key.empty()) return false;
  const std::string base = h->cacheDir + "\\" + key;
  const std::string spvPath = base + ".spv", jsonPath = base + ".json";
  std::vector<uint8_t> spv, js;
  if (!ReadFile(spvPath, &spv) || !ReadFile(jsonPath, &js)) {
    const std::string binPath = base + ".bin";
    WriteFile(binPath, data, dataSize);
    Log("translating shader %s (stage %d)", key.c_str(), stage);
    bool ok = RunHarness(binPath, spvPath, jsonPath, stage, constOff, codeOff);
    DeleteFileA(binPath.c_str());
    if (!ok || !ReadFile(spvPath, &spv) || !ReadFile(jsonPath, &js)) return false;
  }
  J m;
  JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
  if (!jp.parse(m)) { LogError("bad shader manifest %s", jsonPath.c_str()); return false; }
  StageProg sp;
  sp.valid = true; sp.stage = stage; snprintf(sp.key, sizeof sp.key, "%.12s", key.c_str()); sp.fragOutputMap = m.i("fragmentOutputMap");
  snprintf(sp.fkey, sizeof sp.fkey, "%s", key.c_str());
  PipelineRegisterStageKey(sp.key, sp.fkey, stage == 1 && (Rd32(sd.control, 0x718) & 1));
  if (auto* a = m.get("cbuffers")) for (auto& c : a->arr) sp.cb.push_back({c.i("slot"), c.i("set"), c.i("binding")});
  if (auto* a = m.get("sbuffers"))
    for (auto& c : a->arr) sp.sb.push_back({c.i("slot"), c.i("set"), c.i("binding"), c.i("sbCbSlot"), c.i("sbCbOffset"), c.i("write")});
  auto readTex = [](const J* a, std::vector<MTex>& v) {
    if (!a) return;
    for (auto& c : a->arr)
      v.push_back({c.i("set"), c.i("binding"), c.i("handleIndex"), c.i("cbufSlot", -1), c.i("type"), std::max(c.i("array", 1), 1), c.i("separate")});
  };
  readTex(m.get("textures"), sp.tex);
  readTex(m.get("images"), sp.img);
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  if (stage == 5) {   // workgroup size: u32 x, y, z at control+0x748 (checked on the three resource.drp compute programs: 1x1x1, 16x16x1, 16x16x1)
    const uint32_t lx = Rd32(sd.control, 0x748), ly = Rd32(sd.control, 0x74C), lz = Rd32(sd.control, 0x750);
    if (lx && ly && lz && lx <= 1024 && ly <= 1024 && lz <= 64 && lx * ly * lz <= 1024) { SpvSetLocalSize(words, lx, ly, lz); Log("compute shader %s: local size %ux%ux%u", key.c_str(), lx, ly, lz); }
    else LogError("compute shader %s: implausible local size %u %u %u in the control section", key.c_str(), lx, ly, lz);
  }
  if (stage == 1 && (Rd32(sd.control, 0x718) & 1)) {   // layout(early_fragment_tests): see SpvAddEarlyFragmentTests
    if (SpvAddEarlyFragmentTests(words)) Log("fragment shader %s: early fragment tests", key.c_str());
    std::vector<uint8_t> mark;
    if (!ReadFile(base + ".efz", &mark)) WriteFile(base + ".efz", "1", 1);   // the cache key hashes only the data section, so the pipeline service needs this marker
  }
  SpvAddStorageImageCaps(words);   // the translator omits StorageImageRead/WriteWithoutFormat for storage images of unknown format
  if (stage == 1) AaPatchFragment(words, key.c_str());   // SNAP_AA=off/smaa: FXAA tonemap shaders lose their edge test (nvn_aa.cpp)
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp.mod) != VK_SUCCESS) { LogError("vkCreateShaderModule failed for %s", key.c_str()); return false; }
  NoteSubgroup(sp.mod, words);
  if (stage == 0) sp.inputMask = SpvVertexInputMask(words);
  if (stage == 1) { sp.spv = words; sp.outLocs = SpvFragmentOutputs(words); }
  // c[1]: the constant-data blob that follows the code (data section tail)
  sp.constSize = dataSize - constOff;
  bool needC1 = false;
  for (auto& c : sp.cb) if (c.slot == 1) needC1 = true;
  if (needC1 && sp.constSize) {
    void* mp = nullptr;
    if (CreateHostBuffer(h, (sp.constSize + 15) & ~15ull, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &sp.constBuf, &sp.constMem, &mp))
      memcpy(mp, data + constOff, sp.constSize);
  }
  *out = std::move(sp);
  return true;
}

// Rebuilds a stage object (module, reflection, no constant buffer) from the translation cache by its full key. Used by the pipeline
// service to compile pipelines without the game having created the program. Compute stages are not supported.
bool LoadStageFromCacheImpl(Host* h, const std::string& key, StageProg* out, bool eft, bool* eftApplied) {
  std::vector<uint8_t> spv, js;
  const std::string base = h->cacheDir + "\\" + key;
  if (!ReadFile(base + ".spv", &spv) || !ReadFile(base + ".json", &js)) return false;
  J m;
  JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
  if (!jp.parse(m)) return false;
  StageProg sp;
  sp.stage = m.i("stage", -1);
  if (sp.stage < 0 || sp.stage > 4) return false;
  sp.valid = true; snprintf(sp.key, sizeof sp.key, "%.12s", key.c_str()); snprintf(sp.fkey, sizeof sp.fkey, "%s", key.c_str());
  sp.fragOutputMap = m.i("fragmentOutputMap");
  if (auto* a = m.get("cbuffers")) for (auto& c : a->arr) sp.cb.push_back({c.i("slot"), c.i("set"), c.i("binding")});
  if (auto* a = m.get("sbuffers"))
    for (auto& c : a->arr) sp.sb.push_back({c.i("slot"), c.i("set"), c.i("binding"), c.i("sbCbSlot"), c.i("sbCbOffset"), c.i("write")});
  auto readTex = [](const J* a, std::vector<MTex>& v) {
    if (!a) return;
    for (auto& c : a->arr)
      v.push_back({c.i("set"), c.i("binding"), c.i("handleIndex"), c.i("cbufSlot", -1), c.i("type"), std::max(c.i("array", 1), 1), c.i("separate")});
  };
  readTex(m.get("textures"), sp.tex);
  readTex(m.get("images"), sp.img);
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  { std::vector<uint8_t> mark; if (sp.stage == 1 && (eft || ReadFile(base + ".efz", &mark)) && SpvAddEarlyFragmentTests(words) && eftApplied) *eftApplied = true; }
  SpvAddStorageImageCaps(words);
  if (sp.stage == 1) AaPatchFragment(words, key.c_str());   // same patch for pipelines built by the pipeline service
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp.mod) != VK_SUCCESS) return false;
  NoteSubgroup(sp.mod, words);
  if (sp.stage == 0) sp.inputMask = SpvVertexInputMask(words);
  if (sp.stage == 1) { sp.spv = words; sp.outLocs = SpvFragmentOutputs(words); }
  *out = std::move(sp);
  return true;
}

}  // namespace

bool LoadStageFromCache(Host* h, const std::string& fullKey, StageProg* out, bool eft, bool* eftApplied) { return LoadStageFromCacheImpl(h, fullKey, out, eft, eftApplied); }
const char* TranslatorVersion() { return TranslatorSaltStr().c_str(); }
bool ModuleUsesSubgroup(VkShaderModule m) { std::lock_guard<std::mutex> l(g_sgMu); return g_sgMods.count((uint64_t)(uintptr_t)m) != 0; }

bool CreateHostBuffer(Host* h, uint64_t size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = usage;
  if (vkCreateBuffer(h->device, &bi, nullptr, buf) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(h->device, *buf, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(h->device, &mai, nullptr, mem) != VK_SUCCESS) {
    vkDestroyBuffer(h->device, *buf, nullptr);
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(h->device, *buf, *mem, 0);
  vkMapMemory(h->device, *mem, 0, VK_WHOLE_SIZE, 0, mapped);
  return true;
}

void ProgramDestroy(ProgramImpl* p) {
  if (!p) return;
  Host* h = p->host;
  if (h && h->device) {
    // pipelines built from this program stay in the host map (keyed by program id, ids are never reused) and are
    // destroyed at shutdown
    if (!p->composite && p->id) {   // composites built from this program hold copies of its stage objects
      std::lock_guard<std::mutex> lk(h->compMutex);
      for (auto it = h->composites.begin(); it != h->composites.end();) {
        bool uses = false;
        for (uint64_t k : it->first) uses |= k == p->id;
        if (uses) { ProgramDestroy(it->second); delete it->second; it = h->composites.erase(it); } else ++it;
      }
    }
    if (!p->composite) for (auto& s : p->st) FreeStage(h, s);
    for (auto l : p->setLayout) if (l) vkDestroyDescriptorSetLayout(h->device, l, nullptr);
    if (p->layout) vkDestroyPipelineLayout(h->device, p->layout, nullptr);
  }
  p->layout = VK_NULL_HANDLE;
  for (auto& l : p->setLayout) l = VK_NULL_HANDLE;
  for (auto& u : p->setUsed) u = false;
}

// Binding model (see the harness): set 0 = support buffer (0) + c[s] of stage at 1 + stage*16 + s; set 1 = storage
// buffers stage*16 + n; set 2 = combined image samplers stage*32 + n; set 3 = storage images stage*8 + n.
bool ProgramSetShadersImpl(ProgramImpl* p, int n, const ShaderData* sd) {
  Host* h = p->host;
  snap_perf::Scope perfShader(snap_perf::C_SHADER); snap_perf::count(snap_perf::V_SHADERS);
  ProgramDestroy(p);
  if (n < 1 || n > 6 || !sd) return false;
  for (int i = 0; i < n; i++) {
    StageProg sp;
    if (!BuildStage(h, sd[i], &sp)) { ProgramDestroy(p); return false; }
    if (p->st[sp.stage].valid) {
      LogError("ProgramSetShaders: two shaders for stage %d", sp.stage);
      FreeStage(h, sp); ProgramDestroy(p);
      return false;
    }
    p->st[sp.stage] = std::move(sp);
  }
  return ProgramBuildLayout(p);
}

bool ProgramBuildLayout(ProgramImpl* p) {
  Host* h = p->host;
  struct B { VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; uint32_t count = 0; VkShaderStageFlags stages = 0; };
  std::map<std::pair<int, int>, B> binds;
  VkShaderStageFlags all = 0;
  for (auto& s : p->st) if (s.valid) all |= StageBit(s.stage);
  binds[{0, 0}] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all};
  auto add = [&](int set, int binding, VkDescriptorType t, uint32_t cnt, VkShaderStageFlags st) {
    auto& b = binds[{set, binding}];
    b.type = t; b.count = std::max(b.count, cnt); b.stages |= st;
  };
  for (auto& s : p->st) {
    if (!s.valid) continue;
    VkShaderStageFlags sb = StageBit(s.stage);
    for (auto& c : s.cb) add(c.set, c.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sb);
    for (auto& c : s.sb) add(c.set, c.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sb);
    for (auto& t : s.tex) {
      if (t.separate) { LogError("separate texture/sampler bindings are not implemented yet"); continue; }
      add(t.set, t.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)t.array, sb);
    }
    for (auto& t : s.img) add(t.set, t.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)t.array, sb);
  }
  for (int set = 0; set < 4; set++) {
    std::vector<VkDescriptorSetLayoutBinding> v;
    for (auto& kv : binds)
      if (kv.first.first == set) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = (uint32_t)kv.first.second; b.descriptorType = kv.second.type; b.descriptorCount = kv.second.count;
        b.stageFlags = kv.second.stages;
        v.push_back(b);
      }
    p->setUsed[set] = !v.empty();
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = (uint32_t)v.size(); lci.pBindings = v.data();
    if (vkCreateDescriptorSetLayout(h->device, &lci, nullptr, &p->setLayout[set]) != VK_SUCCESS) { ProgramDestroy(p); return false; }
  }
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 4; pl.pSetLayouts = p->setLayout;
  if (vkCreatePipelineLayout(h->device, &pl, nullptr, &p->layout) != VK_SUCCESS) { ProgramDestroy(p); return false; }
  {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    p->id = h->nextProgramId++;
  }
  return true;
}

ProgramImpl* ProgramResolve(Host* h, ProgramImpl* const src[6]) {
  ProgramImpl* first = nullptr;
  int bound = 0;
  std::array<uint64_t, 6> key{};
  for (int s = 0; s < 6; s++) {
    if (!src[s]) continue;
    if (!first) first = src[s];
    if (!src[s]->st[s].valid) continue;
    bound |= 1 << s;
    key[s] = src[s]->id;
  }
  if (!bound) return first;   // nothing usable: hand back any bound program so the draw reports it
  ProgramImpl* one = nullptr;
  bool single = true;
  for (int s = 0; s < 6; s++) if (bound & (1 << s)) { if (!one) one = src[s]; else if (one != src[s]) single = false; }
  if (single) for (int s = 0; s < 6; s++) if (one->st[s].valid && !(bound & (1 << s))) single = false;
  if (single) return one;
  std::lock_guard<std::mutex> lk(h->compMutex);
  auto it = h->composites.find(key);
  if (it != h->composites.end()) return it->second;
  auto* c = new ProgramImpl;
  c->host = h; c->composite = true;
  for (int s = 0; s < 6; s++) if (bound & (1 << s)) c->st[s] = src[s]->st[s];
  if (!ProgramBuildLayout(c)) { ProgramDestroy(c); delete c; return one; }
  h->composites[key] = c;
  return c;
}

}  // namespace nvn
