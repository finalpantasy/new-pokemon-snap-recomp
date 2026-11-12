// Full-resolution photo export (mod: every photo the game saves to the album is also written as a PNG; project code, ours).
//
// The game encodes each album photo (and its thumbnail) through nn::image::JpegEncoder, which is lifted sdk code. Two host
// overrides wrap it: SetPixelData records the pixel buffer, format and size per encoder object; Encode runs the original, and
// when the pixels are photo-sized (width >= SNAP_PHOTO_MIN_W, default 640) the pixels are copied and a worker thread writes
// <photos>\photo_<date>_<time>_<n>.png (Windows Imaging Component, lossless) plus a .json sidecar with the size, the game's
// JPEG byte size and its CRC32 (the gallery links an export to its album entry through that CRC).
//
//   SNAP_PHOTO_EXPORT=0     disables the export (default on; the overrides are then not registered at all).
//   SNAP_PHOTO_DIR=<dir>    output folder (default %APPDATA%\SnapRecomp\photos; test runs should set it).
//   SNAP_PHOTO_MIN_W=<n>    smallest width that counts as a photo (default 640; thumbnails are smaller).
//   SNAP_PHOTO_LOG=1        logs every encoder call (format, size, stride) to stderr.
#include <windows.h>
#include <wincodec.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader.h"

#pragma comment(lib, "windowscodecs")

using namespace loader;

namespace {

const char *kSetPixel = "_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi";
const char *kEncode = "_ZN2nn5image11JpegEncoder6EncodeEPmPvmS3_mPNS0_11ExifBuilderE";

struct PixInfo { uint64_t data; int fmt, w, h, align; };
struct Job { std::vector<uint8_t> bgra; int w, h; std::string base; uint32_t jpg_size, jpg_crc; int fmt, align; };

std::mutex g_mu;
std::condition_variable g_cv;
std::unordered_map<uint64_t, PixInfo> g_pix;
std::unordered_set<uint32_t> g_seen;
std::deque<Job> g_q;
bool g_log, g_started;
int g_min_w = 640;
long g_count;
std::string g_dir;

uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

bool write_png(const Job &j, const std::string &path) {
    IWICImagingFactory *f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    IPropertyBag2 *pb = nullptr;
    bool ok = false;
    std::wstring tmp = widen(path + ".tmp");
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(j.w, j.h))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&pf);
        if (SUCCEEDED(fr->WritePixels(j.h, j.w * 4, (UINT)j.bgra.size(), (BYTE *)j.bgra.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) ok = true;
    }
    if (pb) pb->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
    if (ok) {
        std::wstring fin = widen(path);
        ok = MoveFileExW(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

void worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_q.empty(); });
            j = std::move(g_q.front());
            g_q.pop_front();
        }
        std::string png = g_dir + "\\" + j.base + ".png";
        bool ok = write_png(j, png);
        if (ok) {
            char js[512];
            snprintf(js, sizeof js, "{\"png\": \"%s.png\", \"width\": %d, \"height\": %d, \"jpeg_bytes\": %u, \"jpeg_crc32\": \"%08x\", \"pixel_format\": %d, \"source\": \"nn::image::JpegEncoder\"}\n",
                     j.base.c_str(), j.w, j.h, j.jpg_size, j.jpg_crc, j.fmt);
            FILE *fp = fopen((g_dir + "\\" + j.base + ".json").c_str(), "wb");
            if (fp) { fputs(js, fp); fclose(fp); }
        }
        fprintf(stderr, "[photo-export] %s %s (%dx%d)\n", ok ? "wrote" : "FAILED", png.c_str(), j.w, j.h);
    }
}

void ensure_started() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_started) return;
    g_started = true;
    CreateDirectoryA(g_dir.c_str(), nullptr);
    std::thread(worker).detach();
}

// Source pixel format: the game passes 14 (R,G,B,A bytes); that is the only one converted. Photos are 1080x720, thumbnails 240x200 and 204x136.
void h_setpixel(Cpu *c) {
    uint64_t self = c->x[0];
    PixInfo p{c->x[1], (int)c->x[2], 0, 0, (int)c->x[4]};
    const int *dim = (const int *)c->x[3];
    if (dim) { p.w = dim[0]; p.h = dim[1]; }
    { std::lock_guard<std::mutex> lk(g_mu); g_pix[self] = p; }
    if (g_log) fprintf(stderr, "[photo-export] SetPixelData enc=%#llx data=%#llx fmt=%d %dx%d align=%d\n", (unsigned long long)self, (unsigned long long)p.data, p.fmt, p.w, p.h, p.align);
    uint64_t orig = lookup(kSetPixel);
    if (orig) c->x[0] = call_guest_nested(c, orig, {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]});
}

void h_encode(Cpu *c) {
    uint64_t self = c->x[0], psize = c->x[1], out = c->x[2];
    uint64_t args[7] = {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6]};
    uint64_t orig = lookup(kEncode);
    uint64_t res = orig ? call_guest_nested(c, orig, {args[0], args[1], args[2], args[3], args[4], args[5], args[6]}) : 0;
    PixInfo p{};
    bool have = false;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_pix.find(self); if (it != g_pix.end()) { p = it->second; have = true; } }
    uint64_t jsz = psize ? *(const uint64_t *)psize : 0;
    if (g_log) fprintf(stderr, "[photo-export] Encode enc=%#llx result=%#llx jpeg=%llu have=%d\n", (unsigned long long)self, (unsigned long long)res, (unsigned long long)jsz, have);
    c->x[0] = res;
    if (!have || (uint32_t)res != 0) return;   // failed encode
    if (p.w < g_min_w || p.h < 1 || p.w > 16384 || p.h > 16384 || !p.data) return;
    int bpp = 4;
    if (p.fmt != 14) { fprintf(stderr, "[photo-export] pixel format %d not handled, photo skipped\n", p.fmt); return; }
    size_t stride = (size_t)p.w * bpp;
    if (p.align > 1) stride = (stride + p.align - 1) / p.align * p.align;
    Job j;
    j.w = p.w; j.h = p.h; j.fmt = p.fmt; j.align = p.align;
    j.bgra.resize((size_t)p.w * p.h * 4);
    const uint8_t *src = (const uint8_t *)p.data;
    for (int y = 0; y < p.h; y++) {
        const uint8_t *s = src + y * stride;
        uint8_t *d = &j.bgra[(size_t)y * p.w * 4];
        for (int x = 0; x < p.w; x++) {
            d[4 * x + 0] = s[bpp * x + 2];
            d[4 * x + 1] = s[bpp * x + 1];
            d[4 * x + 2] = s[bpp * x + 0];
            d[4 * x + 3] = 255;   // photos are opaque; the game's alpha is not meaningful in a JPEG
        }
    }
    j.jpg_size = (uint32_t)jsz;
    j.jpg_crc = (out && jsz && jsz < (1u << 26)) ? crc32((const uint8_t *)out, (size_t)jsz) : 0;
    if (j.jpg_crc) {   // the game encodes some photos twice; write each distinct JPEG once per session
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_seen.insert(j.jpg_crc).second) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char nm[96];
    long n;
    { std::lock_guard<std::mutex> lk(g_mu); n = ++g_count; }
    snprintf(nm, sizeof nm, "photo_%04d%02d%02d_%02d%02d%02d_%03ld", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
    j.base = nm;
    ensure_started();
    { std::lock_guard<std::mutex> lk(g_mu); g_q.push_back(std::move(j)); }
    g_cv.notify_one();
}

}  // namespace

namespace loader {
void snap_photoexport_install() {
    const char *e = getenv("SNAP_PHOTO_EXPORT");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F')) return;
    g_log = getenv("SNAP_PHOTO_LOG") && getenv("SNAP_PHOTO_LOG")[0] != '0';
    if (const char *m = getenv("SNAP_PHOTO_MIN_W")) { int v = atoi(m); if (v > 0) g_min_w = v; }
    if (const char *d = getenv("SNAP_PHOTO_DIR")) g_dir = d;
    else if (getenv("SNAP_SAVE_DIR")) return;   // test runs (they set SNAP_SAVE_DIR) never write into the player's photo folder
    else {
        const char *a = getenv("APPDATA");
        g_dir = std::string(a ? a : ".") + "\\SnapRecomp\\photos";
    }
    add_override(kSetPixel, h_setpixel);
    add_override(kEncode, h_encode);
}
}  // namespace loader
