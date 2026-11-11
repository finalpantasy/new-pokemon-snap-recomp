#!/usr/bin/env python3
"""nso2elf.py - convert a Nintendo Switch NSO0 module to an ELF64 AArch64 shared object.

Pure Python (MIT-style, no game data embedded). Compressed NSOs need `lz4` (pip install lz4);
uncompressed NSOs need nothing. Output is deterministic: same input -> byte-identical output.

Segment bytes are copied verbatim (no relocation applied, dynamic tables untouched), so the ELF
loads with the same module-relative layout the NSO has (image base 0).

Usage: python -I nso2elf.py <main.nso> <out.elf> [--json info.json]
"""
import argparse
import os, sys as _s
_s.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "pylib"))  # local pip --target dir (python -I ignores user site)
import json
import struct
import sys

PAGE = 0x1000
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4
PT_LOAD, PT_DYNAMIC, PT_NOTE, PT_GNU_EH_FRAME, PT_GNU_STACK = 1, 2, 4, 0x6474E550, 0x6474E551
DT = {1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH", 5: "STRTAB", 6: "SYMTAB", 7: "RELA",
      8: "RELASZ", 9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI",
      20: "PLTREL", 21: "DEBUG", 23: "JMPREL", 25: "INIT_ARRAY", 26: "FINI_ARRAY",
      27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ", 0x6ffffef5: "GNU_HASH", 0x6ffffff9: "RELACOUNT"}


def align(x, a):
    return (x + a - 1) // a * a


def parse_nso(path):
    d = open(path, "rb").read()
    if d[:4] != b"NSO0":
        raise SystemExit("not an NSO0 file")
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = []
    for i, name in enumerate((".text", ".rodata", ".data")):
        file_off, mem_off, size = struct.unpack_from("<3I", d, 0x10 + 0x10 * i)
        comp = struct.unpack_from("<I", d, 0x60 + 4 * i)[0]
        raw = d[file_off:file_off + (comp if flags >> i & 1 else size)]
        if flags >> i & 1:
            import lz4.block
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        if len(raw) != size:
            raise SystemExit("segment %s size mismatch" % name)
        segs.append(dict(name=name, vaddr=mem_off, data=raw))
    bss_size = struct.unpack_from("<I", d, 0x3C)[0]
    build_id = d[0x40:0x60]
    dynstr_ro, dynstr_sz = struct.unpack_from("<2I", d, 0x90)
    dynsym_ro, dynsym_sz = struct.unpack_from("<2I", d, 0x98)
    return dict(segs=segs, bss_size=bss_size, build_id=build_id, flags=flags,
                hdr_dynstr=(dynstr_ro, dynstr_sz), hdr_dynsym=(dynsym_ro, dynsym_sz))


def read_mem(segs, addr, n):
    for s in segs:
        if s["vaddr"] <= addr and addr + n <= s["vaddr"] + len(s["data"]):
            o = addr - s["vaddr"]
            return s["data"][o:o + n]
    raise SystemExit("address 0x%x+0x%x not in a segment" % (addr, n))


def build(nso):
    segs = nso["segs"]
    text, ro, data = segs
    # ---- MOD0 ----
    mod_off = struct.unpack_from("<I", text["data"], 4)[0]
    if text["data"][mod_off:mod_off + 4] != b"MOD0":
        raise SystemExit("MOD0 not found")
    dyn_o, bss_s, bss_e, ehh_s, ehh_e, rt = struct.unpack_from("<6i", text["data"], mod_off + 4)
    dynamic = mod_off + dyn_o
    mod0 = dict(mod0=mod_off, dynamic=dynamic, bss_start=mod_off + bss_s, bss_end=mod_off + bss_e,
                eh_frame_hdr=mod_off + ehh_s, eh_frame_hdr_end=mod_off + ehh_e,
                runtime_module=mod_off + rt)
    # ---- dynamic table ----
    dyn = {}
    dyn_entries = []
    a = dynamic
    while True:
        t, v = struct.unpack("<qQ", read_mem(segs, a, 16))
        dyn_entries.append((t, v))
        a += 16
        if t == 0:
            break
        dyn.setdefault(t, v)
    dynamic_size = a - dynamic
    # ---- memory layout ----
    data_end = data["vaddr"] + len(data["data"])
    bss_start = align(data_end, PAGE)
    if mod0["bss_start"] != bss_start:
        bss_start = mod0["bss_start"]
    bss_size = nso["bss_size"]
    seg_end = {".text": align(len(text["data"]), PAGE), ".rodata": align(len(ro["data"]), PAGE)}
    data_memsz = bss_start + bss_size - data["vaddr"]

    # ---- derived sections (addr, size, name, type, flags, link, info, entsize, align) ----
    der = []

    def add(name, addr, size, typ, flags, entsize=0, al=8, link=None, info=0):
        if size > 0:
            der.append(dict(name=name, addr=addr, size=size, type=typ, flags=flags,
                            entsize=entsize, align=al, link=link, info=info))

    nsym = struct.unpack_from("<2I", read_mem(segs, dyn[4], 8))[1] if 4 in dyn else \
        nso["hdr_dynsym"][1] // 24
    add(".dynsym", dyn[6], nsym * 24, 11, SHF_ALLOC, 24, 8, link=".dynstr", info=1)
    add(".dynstr", dyn[5], dyn[10], 3, SHF_ALLOC, 0, 1)
    if 4 in dyn:  # DT_HASH: 2 words + nbucket + nchain words
        nb, nc = struct.unpack_from("<2I", read_mem(segs, dyn[4], 8))
        add(".hash", dyn[4], 8 + 4 * (nb + nc), 5, SHF_ALLOC, 4, 8, link=".dynsym")
    if 0x6ffffef5 in dyn:  # DT_GNU_HASH: nbuckets, symoffset, bloom_size, bloom_shift
        nb, so, bs, _ = struct.unpack_from("<4I", read_mem(segs, dyn[0x6ffffef5], 16))
        add(".gnu.hash", dyn[0x6ffffef5], align(16 + 8 * bs + 4 * nb + 4 * (nsym - so), 8),
            0x6ffffff6, SHF_ALLOC, 0, 8, link=".dynsym")
    add(".rela.dyn", dyn[7], dyn[8], 4, SHF_ALLOC, 24, 8, link=".dynsym")
    if 23 in dyn:
        add(".rela.plt", dyn[23], dyn[2], 4, SHF_ALLOC, 24, 8, link=".dynsym")
    nplt = dyn.get(2, 0) // 24
    if nplt:
        # Heuristic (same split the Adubbz loader makes): PLT = 16 bytes per JMPREL entry, ending at
        # the end of .text; .got.plt = 3 reserved words + one 8-byte slot per entry; .got follows
        # it up to .init_array/.dynamic end.
        add(".plt", len(text["data"]) - 16 * nplt, 16 * nplt, 1, SHF_ALLOC | SHF_EXEC, 16, 16)
        add(".got.plt", dyn[3], 8 * (nplt + 3), 1, SHF_ALLOC | SHF_WRITE, 8, 8)
        if 25 in dyn:
            g = dyn[3] + 8 * (nplt + 3)
            add(".got", g, dyn[25] - g, 1, SHF_ALLOC | SHF_WRITE, 8, 8)
    add(".dynamic", dynamic, dynamic_size, 6, SHF_ALLOC | SHF_WRITE, 16, 8, link=".dynstr")
    if 25 in dyn:
        add(".init_array", dyn[25], dyn[27], 14, SHF_ALLOC | SHF_WRITE, 8, 8)
    if 26 in dyn:
        add(".fini_array", dyn[26], dyn[28], 15, SHF_ALLOC | SHF_WRITE, 8, 8)
    # eh_frame_hdr / eh_frame
    ehh = mod0["eh_frame_hdr"]
    hdr = read_mem(segs, ehh, 8)
    eh_frame = None
    if hdr[0] == 1 and hdr[1] == 0x1B:
        eh_frame = ehh + 4 + struct.unpack_from("<i", hdr, 4)[0]
        add(".eh_frame_hdr", ehh, mod0["eh_frame_hdr_end"] - ehh, 1, SHF_ALLOC, 0, 4)
        # walk CIE/FDE records
        p, ro_end = eh_frame, ro["vaddr"] + len(ro["data"])
        while p + 4 <= ro_end:
            ln = struct.unpack("<I", read_mem(segs, p, 4))[0]
            if ln == 0:
                p += 4
                break
            if ln == 0xFFFFFFFF:
                ln = struct.unpack("<Q", read_mem(segs, p + 4, 8))[0] + 12
            else:
                ln += 4
            p += ln
        add(".eh_frame", eh_frame, p - eh_frame, 1, SHF_ALLOC, 0, 8)
    der.sort(key=lambda s: s["addr"])
    for x, y in zip(der, der[1:]):
        if x["addr"] + x["size"] > y["addr"]:
            raise SystemExit("derived sections overlap: %s / %s" % (x["name"], y["name"]))

    # ---- carve segments into non-overlapping sections ----
    sections = []

    def carve(seg_name, base, size, typ, flags, al):
        cur = base
        for s in [s for s in der if base <= s["addr"] < base + size]:
            if s["addr"] > cur:
                sections.append(dict(name=seg_name, addr=cur, size=s["addr"] - cur, type=typ,
                                     flags=flags, entsize=0, align=al, link=None, info=0))
            sections.append(s)
            cur = s["addr"] + s["size"]
        if cur < base + size:
            sections.append(dict(name=seg_name, addr=cur, size=base + size - cur, type=typ,
                                 flags=flags, entsize=0, align=al, link=None, info=0))

    carve(".text", text["vaddr"], len(text["data"]), 1, SHF_ALLOC | SHF_EXEC, 0x1000)
    carve(".rodata", ro["vaddr"], len(ro["data"]), 1, SHF_ALLOC, 0x1000)
    carve(".data", data["vaddr"], len(data["data"]), 1, SHF_ALLOC | SHF_WRITE, 0x1000)
    sections.append(dict(name=".bss", addr=bss_start, size=bss_size, type=8,
                         flags=SHF_ALLOC | SHF_WRITE, entsize=0, align=0x1000, link=None, info=0))
    for s in der:
        if s not in sections:
            raise SystemExit("derived section outside segments: " + s["name"])
    for s in sections:
        s["off"] = None

    # ---- file layout (file offset = vaddr + PAGE keeps offset == vaddr mod PAGE) ----
    seg_list = [dict(p_type=PT_LOAD, flags=5, vaddr=text["vaddr"], data=text["data"],
                     memsz=len(text["data"])),
                dict(p_type=PT_LOAD, flags=4, vaddr=ro["vaddr"], data=ro["data"],
                     memsz=len(ro["data"])),
                dict(p_type=PT_LOAD, flags=6, vaddr=data["vaddr"], data=data["data"],
                     memsz=data_memsz)]
    for s in seg_list:
        s["off"] = s["vaddr"] + PAGE
    last = seg_list[-1]
    tail = last["off"] + len(last["data"])
    for s in sections:
        if s["type"] == 8:
            s["off"] = last["off"] + (s["addr"] - last["vaddr"])  # NOBITS, no file data
        else:
            for g in seg_list:
                if g["vaddr"] <= s["addr"] < g["vaddr"] + len(g["data"]):
                    s["off"] = g["off"] + s["addr"] - g["vaddr"]
    # build-id note
    bid = nso["build_id"][:20]
    note = struct.pack("<III", 4, len(bid), 3) + b"GNU\0" + bid
    note_off = align(tail, 4)
    note_sec = dict(name=".note.gnu.build-id", addr=0, size=len(note), type=7, flags=0,
                    entsize=0, align=4, link=None, info=0, off=note_off)
    shstr_sec = dict(name=".shstrtab", addr=0, type=3, flags=0, entsize=0, align=1, link=None,
                     info=0)
    all_secs = [dict(name="", addr=0, size=0, type=0, flags=0, entsize=0, align=0, link=None,
                     info=0, off=0)] + sections + [note_sec, shstr_sec]
    names = b"\0"
    for s in all_secs[1:]:
        s["name_off"] = len(names)
        names += s["name"].encode() + b"\0"
    all_secs[0]["name_off"] = 0
    shstr_sec["off"] = note_off + len(note)
    shstr_sec["size"] = len(names)
    shoff = align(shstr_sec["off"] + len(names), 8)
    idx = {s["name"]: i for i, s in enumerate(all_secs)}

    phdrs = []
    for g in seg_list:
        phdrs.append(struct.pack("<IIQQQQQQ", PT_LOAD, g["flags"], g["off"], g["vaddr"], g["vaddr"],
                                 len(g["data"]), g["memsz"], PAGE))
    phdrs.append(struct.pack("<IIQQQQQQ", PT_DYNAMIC, 6, dynamic + PAGE, dynamic, dynamic,
                             dynamic_size, dynamic_size, 8))
    if eh_frame is not None:
        phdrs.append(struct.pack("<IIQQQQQQ", PT_GNU_EH_FRAME, 4, ehh + PAGE, ehh, ehh,
                                 mod0["eh_frame_hdr_end"] - ehh, mod0["eh_frame_hdr_end"] - ehh, 4))
    phdrs.append(struct.pack("<IIQQQQQQ", PT_NOTE, 4, note_off, 0, 0, len(note), len(note), 4))
    phdrs.append(struct.pack("<IIQQQQQQ", PT_GNU_STACK, 6, 0, 0, 0, 0, 0, 16))

    out = bytearray(shoff + 64 * len(all_secs))
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    out[0:64] = ident + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, shoff, 0, 64, 56,
                                    len(phdrs), 64, len(all_secs), idx[".shstrtab"])
    assert 64 + 56 * len(phdrs) <= PAGE
    for i, p in enumerate(phdrs):
        out[64 + 56 * i:64 + 56 * (i + 1)] = p
    for g in seg_list:
        out[g["off"]:g["off"] + len(g["data"])] = g["data"]
    out[note_off:note_off + len(note)] = note
    out[shstr_sec["off"]:shstr_sec["off"] + len(names)] = names
    for i, s in enumerate(all_secs):
        link = idx[s["link"]] if s["link"] else 0
        # name duplicates (.text pieces etc.) are fine
        out[shoff + 64 * i:shoff + 64 * (i + 1)] = struct.pack(
            "<IIQQQQIIQQ", s["name_off"], s["type"], s["flags"], s["addr"], s["off"] or 0,
            s["size"], link, s["info"], s["align"], s["entsize"])
    info = dict(
        segments=[dict(name=n, vaddr=g["vaddr"], filesz=len(g["data"]), memsz=g["memsz"], flags=g["flags"])
                  for n, g in zip((".text", ".rodata", ".data+.bss"), seg_list)],
        bss=dict(start=bss_start, size=bss_size, mod0_bss_end=mod0["bss_end"]),
        mod0={k: hex(v) for k, v in mod0.items()},
        dynamic={DT.get(t, hex(t)): hex(v) for t, v in dyn_entries},
        dynsym_count=nsym, dynsym_hdr_bytes=nso["hdr_dynsym"][1],
        rela_dyn_count=dyn[8] // 24, rela_plt_count=dyn.get(2, 0) // 24,
        build_id=bid.hex().upper(),
        sections=[dict(name=s["name"], addr=hex(s["addr"]), size=hex(s["size"])) for s in sections])
    return bytes(out), info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("elf")
    ap.add_argument("--json")
    a = ap.parse_args()
    elf, info = build(parse_nso(a.nso))
    with open(a.elf, "wb") as f:
        f.write(elf)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(info, f, indent=1, sort_keys=True)
    print("wrote %s (%d bytes)" % (a.elf, len(elf)))


if __name__ == "__main__":
    sys.exit(main())
