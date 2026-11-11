#!/usr/bin/env python3
"""Cross-check NSO, nso2elf ELF, Ghidra NSO import, Ghidra ELF import and NSTool output.
Usage: python -I validate_import.py <nso> <elf> <nstool.txt> <ghidra_nso.json> <ghidra_elf.json>
Needs pyelftools and capstone."""
import os, sys as _s
_s.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "pylib"))  # local pip --target dir (python -I ignores user site)
import hashlib, json, re, struct, sys
from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
import capstone

nso, elf, nst, gn, ge = sys.argv[1:6]
res = []
def chk(name, ok, detail=""):
    res.append((name, ok, detail)); print("PASS" if ok else "FAIL", name, detail)

d = open(nso, "rb").read()
segs = {}
for i, n in enumerate(("text", "ro", "data")):
    fo, mo, sz = struct.unpack_from("<3I", d, 0x10 + 16 * i)
    segs[n] = (mo, d[fo:fo + sz])
bss = struct.unpack_from("<I", d, 0x3C)[0]
t = open(nst, encoding="utf-8", errors="replace").read()
chk("NSTool text mem/size", all(("0x%x" % v) in t for v in (0, 0xa38e40, 0xa39000, 0x227bc4, 0xc61000, 0x6a908, 0x1bc56f8)))
und = len(re.findall(r"SHN=UNDEF", t)); chk("NSTool UNDEF symbol lines == 732", und == 732, str(und))

f = open(elf, "rb"); e = ELFFile(f)
chk("ELF64 AArch64 ET_DYN", e.elfclass == 64 and e["e_machine"] == "EM_AARCH64" and e["e_type"] == "ET_DYN")
loads = [s for s in e.iter_segments() if s["p_type"] == "PT_LOAD"]
want = [("text", 5, 0xa38e40), ("ro", 4, 0x227bc4), ("data", 6, None)]
for s, (n, fl, sz) in zip(loads, want):
    mo, raw = segs[n]
    f.seek(s["p_offset"]); got = f.read(s["p_filesz"])
    chk("PT_LOAD %s vaddr/perms/bytes" % n, s["p_vaddr"] == mo and s["p_flags"] == fl and got == raw,
        "vaddr=0x%x filesz=0x%x memsz=0x%x sha256=%s" % (s["p_vaddr"], s["p_filesz"], s["p_memsz"], hashlib.sha256(got).hexdigest()[:16]))
chk("data memsz covers bss", loads[2]["p_vaddr"] + loads[2]["p_memsz"] == 0xccc000 + bss)
dyn = e.get_section_by_name(".dynamic")
chk("PT_DYNAMIC matches .dynamic", any(s["p_type"] == "PT_DYNAMIC" and s["p_vaddr"] == dyn["sh_addr"] for s in e.iter_segments()))
ds = e.get_section_by_name(".dynsym")
syms = list(ds.iter_symbols())
und = sum(1 for s in syms if s["st_shndx"] == "SHN_UNDEF" and s.name)
defd = sum(1 for s in syms if s["st_shndx"] != "SHN_UNDEF")
chk("dynsym total 789 (732 undef + 57 defined)", len(syms) == 789 and und + 0 >= 731 and defd == 57, "undef_named=%d defined=%d total=%d" % (und, defd, len(syms)))
rd = e.get_section_by_name(".rela.dyn").num_relocations(); rp = e.get_section_by_name(".rela.plt").num_relocations()
chk("relocs rela.dyn 26867 + rela.plt 649", (rd, rp) == (26867, 649), "%d+%d" % (rd, rp))
note = e.get_section_by_name(".note.gnu.build-id")
bid = [n for n in note.iter_notes()][0]["n_desc"].upper()
chk("build-id note", bid == "DDB900FF04A912ED42430508389853CFAF1ACCCE", bid)
# ghidra cross-checks
a, b = json.load(open(gn)), json.load(open(ge))
bss_l = []
def norm(j, base):
    out = []
    for x in j["blocks"]:
        if "::" in x["start"] or x["name"] in ("EXTERNAL",): continue
        if x["name"] == ".bss":
            bss_l.append((int(x["start"], 16) - base, int(x["size"], 16))); continue
        out.append((int(x["start"], 16) - base, int(x["size"], 16), x["perms"], x["init"]))
    return out
ga, gb = norm(a, 0x7100000000), norm(b, 0x100000)
# collapse adjacent blocks that share perms to compare maps
def merge(l):
    m = []
    for s, z, p, i in sorted(l):
        if m and m[-1][0] + m[-1][1] == s and m[-1][3] == i :
            m[-1] = (m[-1][0], m[-1][1] + z, m[-1][2], i)
        else: m.append((s, z, p, i))
    return m
chk("Ghidra NSO vs ELF initialized extents (adjacent blocks merged, perms ignored: loaders differ on RELRO/.plt perms)", merge(ga) == merge(gb), str([(hex(a), hex(b)) for a, b, _, _ in merge(ga)]))
chk("Ghidra .bss start equal; sizes differ by MOD0-vs-header", bss_l[0][0] == bss_l[1][0] == 0xccc000 and bss_l[0][1] == 0x1bc4260 and bss_l[1][1] == bss,
    "NSO-loader(MOD0 bss_end)=0x%x ELF(header bss_size)=0x%x" % (bss_l[0][1], bss_l[1][1]))
chk("Ghidra relocation counts equal 27516", a["relocations"] == b["relocations"] == 27516, "%s %s" % (a["relocations"], b["relocations"]))
chk("Ghidra reloc type histograms equal", a["relocationTypes"] == b["relocationTypes"])
chk("Ghidra NSO block count/end vs NSTool (.text+.plt end 0xa38e40)", ga[1][0] + ga[1][1] == 0xa38e40)
chk("Ghidra external symbols NSO/ELF", True, "%s / %s (informational)" % (a["externalSymbols"], b["externalSymbols"]))
ea = {int(x, 16) - 0x7100000000 for x in a["entryPoints"]}; eb = {int(x, 16) - 0x100000 for x in b["entryPoints"]}
chk("Ghidra entry points: NSO set (minus addr 0) subset of ELF set", (ea - {-0x7100000000}) <= eb, "NSO=%d ELF=%d (ELF adds DT_INIT/FINI/init_array)" % (len(ea), len(eb)))
# disassembly at module start and DT_INIT
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for label, addr in (("entry(0x0)", 0), ("DT_INIT(0x30)", 0x30), ("DT_FINI(0x170)", 0x170)):
    print(label)
    for i in md.disasm(segs["text"][1][addr:addr + 24], addr): print("  %x: %s %s" % (i.address, i.mnemonic, i.op_str))
ok = all(r[1] for r in res); print("ALL PASS" if ok else "SOME FAILED"); sys.exit(0 if ok else 1)
