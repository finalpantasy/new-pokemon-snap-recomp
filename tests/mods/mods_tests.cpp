// Host-only tests for runtime/mods/mods_core (path mapping, index priority, replacement-chunk size override).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "mods/mods_core.h"

namespace fs = std::filesystem;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << s;
}

int main() {
  using namespace mods;
  // ext ids (known values from romfs_census.DRP_EXT)
  CHECK(ext_crc("nutexb") == 0x5C156DBC);
  CHECK(ext_crc("NUTEXB") == 0x5C156DBC);
  CHECK(ext_crc("msbt") == 0xBE1C9ACB);
  CHECK(ext_crc("DC4A8177") == 0xDC4A8177);
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  CHECK(norm_archive("Common\\UI/Master\\a.drp") == "common/ui/master/a.drp");
  CHECK(norm_archive("rom:/common/Text/x.drp") == "common/text/x.drp");

  std::string a, s;
  uint32_t x = 0;
  CHECK(parse_pack_path("common/UI/master/Foo.drp/Icon_A.nutexb", &a, &s, &x));
  CHECK(a == "common/ui/master/foo.drp" && s == "icon_a" && x == 0x5C156DBC);
  CHECK(parse_pack_path("a.drp/foo.nutexb.nutexb", &a, &s, &x) && s == "foo");     // extracted name that repeats its ext
  CHECK(parse_pack_path("a.drp/foo.msbt", &a, &s, &x) && s == "foo" && x == 0xBE1C9ACB);
  CHECK(!parse_pack_path("pack.json", &a, &s, &x));
  CHECK(!parse_pack_path("common/UI/foo.nutexb", &a, &s, &x));            // no .drp component
  CHECK(!parse_pack_path("a.drp/sub/foo.msbt", &a, &s, &x));              // entry must sit directly in the archive folder
  CHECK(!parse_pack_path("a.drp/noext", &a, &s, &x));

  // Index: first (highest priority) pack wins; the game's name may carry its extension
  Index ix;
  ix.packs.resize(2);
  ix.packs[0].id = "hi";
  ix.packs[1].id = "lo";
  ix.add(0, "a.drp", "icon", ext_crc("nutexb"), "C:/hi/icon.nutexb");
  ix.add(1, "a.drp", "icon", ext_crc("nutexb"), "C:/lo/icon.nutexb");
  ix.add(1, "a.drp", "other", ext_crc("msbt"), "C:/lo/other.msbt");
  CHECK(ix.map.size() == 2 && ix.conflicts.size() == 1);
  const Override* o = ix.find("a.drp", "Icon", ext_crc("nutexb"));
  CHECK(o && o->pack == 0 && o->file == "C:/hi/icon.nutexb");
  CHECK(ix.find("a.drp", "Icon.nutexb", ext_crc("nutexb")) == o);          // stored name with extension
  CHECK(!ix.find("a.drp", "Icon", ext_crc("msbt")));                       // same name, other type
  CHECK(!ix.find("b.drp", "icon", ext_crc("nutexb")));                     // other archive
  CHECK(ix.find("a.drp", "OTHER", ext_crc("msbt")) != nullptr);

  // load(): mods.json order and enabled flags, pack.json, ignored files
  fs::path root = fs::temp_directory_path() / "snap_mods_test";
  fs::remove_all(root);
  write(root / "packA" / "pack.json", "{\"name\":\"Pack A\",\"version\":\"1.0\",\"author\":\"t\",\"description\":\"d\"}");
  write(root / "packA" / "common/UI/x.drp/icon.nutexb", "AAAA");
  write(root / "packB" / "pack.json", "{\"name\":\"Pack B\"}");
  write(root / "packB" / "common/UI/x.drp/icon.nutexb", "BBBB");
  write(root / "packB" / "common/UI/x.drp/menu.msbt", "MENU");
  write(root / "packB" / "readme.txt", "ignored");
  write(root / "packC" / "pack.json", "{}");
  write(root / "packC" / "common/UI/x.drp/off.msbt", "OFF");
  write(root / "mods.json", "{\"packs\":[{\"id\":\"packB\"},{\"id\":\"packA\",\"enabled\":true},{\"id\":\"packC\",\"enabled\":false},{\"id\":\"missing\"}]}");
  {
    Index i2;
    std::vector<std::string> w;
    load(root.string(), i2, w);
    CHECK(i2.packs.size() == 3 && i2.packs[0].name == "Pack B" && i2.packs[1].version == "1.0");
    CHECK(i2.map.size() == 2);   // icon (B wins), menu; packC disabled
    const Override* e = i2.find("common/ui/x.drp", "icon", ext_crc("nutexb"));
    CHECK(e && i2.packs[e->pack].id == "packB");
    CHECK(!i2.find("common/ui/x.drp", "off", ext_crc("msbt")));
    CHECK(w.size() == 1);        // "missing" folder
  }
  fs::remove(root / "mods.json");
  {
    Index i3;
    std::vector<std::string> w;
    load(root.string(), i3, w);   // no mods.json: name order, all with pack.json
    const Override* e = i3.find("common/ui/x.drp", "icon", ext_crc("nutexb"));
    CHECK(e && i3.packs[e->pack].id == "packA" && i3.map.size() == 3);
  }
  fs::remove_all(root);

  // Size override: the replacement chunk must pass the loader's raw-entry path (csize == dsize), its CRC check, and
  // may be larger than the original. Name "Icon" -> header 0x18 + 8 bytes.
  CHECK(chunk_header_len(4) == 0x18 + 8);
  CHECK(chunk_header_len(3) == 0x18 + 4);
  CHECK(chunk_header_len(7) == 0x18 + 8 && chunk_header_len(8) == 0x18 + 12);
  uint8_t orig[0x18 + 8 + 16] = {};
  uint32_t w0[6] = {0x1111u, 0x64736572u, 0x40u, ext_crc("nutexb"), 16u + 4u, 100u};   // an Oodle original: csize 20, dsize 100
  std::memcpy(orig, w0, sizeof w0);
  std::memcpy(orig + 0x18, "Icon", 5);
  std::vector<uint8_t> big(400);
  for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7 + 3);   // 4x the original's 100 bytes
  auto ch = build_chunk(orig, chunk_header_len(4), big.data(), big.size());
  uint32_t crc, magic, total, ecrc, csz, dsz;
  std::memcpy(&crc, &ch[0], 4); std::memcpy(&magic, &ch[4], 4); std::memcpy(&total, &ch[8], 4);
  std::memcpy(&ecrc, &ch[12], 4); std::memcpy(&csz, &ch[16], 4); std::memcpy(&dsz, &ch[20], 4);
  CHECK(magic == 0x64736572u && ecrc == ext_crc("nutexb"));
  CHECK(csz == 400 && dsz == 400);                               // raw path: memcpy(dst, payload, csize), dst sized dsize
  CHECK(total == ch.size() && ch.size() == 0x18 + 8 + 400);
  CHECK(crc == crc32(big.data(), big.size()));                   // the loader's check over dsize bytes
  CHECK(std::memcmp(&ch[0x18], "Icon", 5) == 0);                 // name kept
  size_t doff = 0x18 + ((std::strlen((const char*)&ch[0x18]) + 4) & ~(size_t)3);   // how the loader finds the payload
  CHECK(doff == chunk_header_len(4) && std::memcmp(&ch[doff], big.data(), big.size()) == 0);

  printf("%d checks, %d failed\n", g_n, g_fail);
  return g_fail ? 1 : 0;
}
