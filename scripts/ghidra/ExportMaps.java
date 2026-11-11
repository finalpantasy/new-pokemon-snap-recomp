// Deterministic export of analysis maps from an analyzed v262144 main program.
// Usage (headless): -process <prog> -noanalysis -readOnly -postScript ExportMaps.java <outdir> <fde.tsv> <input_sha256> <build_id>
// Writes functions.tsv, functions_disagreements.tsv, imports.tsv, relocations.tsv, relocations_summary.md,
// rtti.tsv, vtables_candidate.tsv, render_passes.tsv, render_passes.md, nvn_procs.tsv.
// All data rows are sorted; headers carry build id / sha256 / tool versions; no timestamps.
// Offsets are module-relative (image base 0x7100000000 in the Ghidra NSO route).
import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.app.util.demangler.DemangledObject;
import ghidra.app.util.demangler.DemanglerUtil;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.regex.*;

public class ExportMaps extends GhidraScript {
    Memory mem; Address base; FunctionManager fm;
    byte[] img; long imgEnd;
    File outDir; String sha, buildId, ghidraVer;
    final Map<String, String> demCache = new HashMap<>();

    // ---------- helpers ----------
    static String hx(long v) { return "0x" + Long.toHexString(v); }
    long u8(long o) { return img[(int) o] & 0xffL; }
    long u32(long o) { return (u8(o) | u8(o + 1) << 8 | u8(o + 2) << 16 | u8(o + 3) << 24); }
    long u64(long o) { return u32(o) | (u32(o + 4) << 32); }
    Address A(long off) { return base.add(off); }
    long blkStart(String n) { MemoryBlock b = mem.getBlock(n); return b == null ? -1 : b.getStart().getOffset() - base.getOffset(); }
    long blkSize(String n) { MemoryBlock b = mem.getBlock(n); return b == null ? 0 : b.getSize(); }
    String seg(long off) { MemoryBlock b = mem.getBlock(A(off)); return b == null ? "none" : b.getName(); }
    String esc(String s) {
        StringBuilder sb = new StringBuilder();
        for (char c : s.toCharArray()) {
            if (c == '\t') sb.append("\\t"); else if (c == '\n') sb.append("\\n"); else if (c == '\r') sb.append("\\r");
            else if (c == '\\') sb.append("\\\\"); else sb.append(c);
        }
        return sb.toString();
    }
    String demangle(String m) {
        if (m == null || !m.startsWith("_Z")) return "";
        String c = demCache.get(m);
        if (c != null) return c;
        String r = "";
        try {
            DemangledObject d = DemanglerUtil.demangle(currentProgram, m);
            if (d != null) { r = d.getSignature(false); if (r != null && r.startsWith("undefined ")) r = r.substring(10); }
        } catch (Throwable t) { r = ""; }
        if (r == null) r = "";
        demCache.put(m, r);
        return r;
    }
    PrintWriter open(String name, String desc, String cols) throws Exception {
        PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(outDir, name)), StandardCharsets.UTF_8), false);
        w.print("# file: " + name + "\n# " + desc + "\n# build_id: " + buildId + "\n# input_sha256: " + sha + "\n");
        w.print("# tools: Ghidra " + ghidraVer + "; SwitchLoader 1.6.1-0f38957; export script ExportMaps.java\n");
        w.print("# offsets are module-relative; Ghidra address = 0x7100000000 + offset\n");
        if (cols != null) w.print(cols + "\n");
        return w;
    }
    String funcName(Function f) { return f == null ? "" : f.getName(); }
    Function funcAt(long off) { return fm.getFunctionContaining(A(off)); }
    long entryOf(Function f) { return f.getEntryPoint().getOffset() - base.getOffset(); }
    String fcell(long siteOff) {
        Function f = funcAt(siteOff);
        return f == null ? "\t" : hx(entryOf(f)) + "\t" + f.getName();
    }

    // ---------- data classes ----------
    static class Sym { String name; int info, shndx; long value, size; int idx; }
    static class Rel { long off, info, addend; int type, sym; long target = -1; boolean toImport; }
    static class Str { long off; String s; String cat; }

    List<Sym> dynsym = new ArrayList<>();
    List<Rel> rels = new ArrayList<>();
    TreeMap<Long, Long> fde = new TreeMap<>();
    long textStart, textEnd, pltStart, pltEnd;

    // xref arrays: key = target<<24 | site
    long[] xr = new long[1 << 20]; int xrN = 0;
    void addXr(long target, long site) {
        if (xrN == xr.length) xr = Arrays.copyOf(xr, xr.length * 2);
        xr[xrN++] = (target << 24) | site;
    }
    // returns sites whose target in [lo,hi)
    List<Long> xrefsIn(long lo, long hi) {
        List<Long> out = new ArrayList<>();
        int a = lowerBound(lo << 24), b = lowerBound(hi << 24);
        for (int i = a; i < b; i++) out.add(xr[i] & 0xffffffL);
        return out;
    }
    int lowerBound(long key) {
        int lo = 0, hi = xrN;
        while (lo < hi) { int m = (lo + hi) >>> 1; if (xr[m] < key) lo = m + 1; else hi = m; }
        return lo;
    }
    Map<Long, int[]> pltCalls = new TreeMap<>(); // stub offset -> {bl, b}

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        outDir = new File(args[0]);
        File fdeFile = new File(args[1]);
        sha = args[2]; buildId = args[3];
        ghidraVer = Application.getApplicationVersion();
        mem = currentProgram.getMemory(); base = currentProgram.getImageBase(); fm = currentProgram.getFunctionManager();
        outDir.mkdirs();

        // image bytes
        long maxEnd = 0;
        for (MemoryBlock b : mem.getBlocks())
            if (b.isInitialized() && !b.isExternalBlock()) maxEnd = Math.max(maxEnd, b.getEnd().getOffset() - base.getOffset() + 1);
        imgEnd = maxEnd;
        img = new byte[(int) maxEnd + 16];
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExternalBlock()) continue;
            long s = b.getStart().getOffset() - base.getOffset();
            byte[] tmp = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), tmp);
            System.arraycopy(tmp, 0, img, (int) s, tmp.length);
        }
        textStart = blkStart(".text"); textEnd = textStart + blkSize(".text");
        pltStart = blkStart(".plt"); pltEnd = pltStart + blkSize(".plt");

        // FDEs
        try (BufferedReader r = new BufferedReader(new FileReader(fdeFile))) {
            String l;
            while ((l = r.readLine()) != null) {
                if (l.startsWith("#") || l.isEmpty()) continue;
                String[] p = l.split("\t");
                fde.put(Long.decode(p[0]), Long.decode(p[1]));
            }
        }
        // dynsym
        long dsO = blkStart(".dynsym"), dsN = blkSize(".dynsym") / 24, dstr = blkStart(".dynstr");
        for (int i = 0; i < dsN; i++) {
            long o = dsO + 24L * i;
            Sym s = new Sym(); s.idx = i;
            long nameOff = u32(o); s.info = (int) u8(o + 4); s.shndx = (int) (u8(o + 6) | u8(o + 7) << 8);
            s.value = u64(o + 8); s.size = u64(o + 16);
            StringBuilder sb = new StringBuilder();
            for (long q = dstr + nameOff; u8(q) != 0; q++) sb.append((char) u8(q));
            s.name = sb.toString();
            dynsym.add(s);
        }
        // relocations
        for (String bn : new String[]{".rela.dyn", ".rela.plt"}) {
            long o0 = blkStart(bn), n = blkSize(bn) / 24;
            for (long i = 0; i < n; i++) {
                long o = o0 + 24 * i;
                Rel r = new Rel(); r.off = u64(o); r.info = u64(o + 8); r.addend = u64(o + 16);
                r.type = (int) (r.info & 0xffffffffL); r.sym = (int) (r.info >>> 32);
                if (r.sym != 0) {
                    Sym s = dynsym.get(r.sym);
                    if (s.shndx == 0) r.toImport = true; else r.target = s.value + r.addend;
                    if (r.type == 1025 || r.type == 1026) { if (!r.toImport) r.target = s.value; }
                } else r.target = r.addend;
                if (r.type == 1026 || r.type == 1025) { /* import GOT slots: target stays -1 when import */ }
                rels.add(r);
            }
        }
        rels.sort((a, b) -> a.off != b.off ? Long.compare(a.off, b.off) : Integer.compare(a.type, b.type));
        println("fde=" + fde.size() + " dynsym=" + dynsym.size() + " rels=" + rels.size());

        scanCode();
        exportFunctionsAndDisagreements();
        exportImports();
        exportRelocations();
        exportRtti();
        List<long[]> vts = exportVtables();
        exportRenderPasses(vts);
        println("export done");
    }

    // ---------- code scan: adrp+add/ldr refs and bl/b to plt ----------
    void scanCode() {
        for (long o = textStart; o + 4 <= textEnd; o += 4) {
            long w = u32(o);
            if ((w & 0x7c000000L) == 0x14000000L) {
                long imm = w & 0x3ffffffL; if ((imm & 0x2000000L) != 0) imm -= 0x4000000L;
                long t = o + imm * 4;
                if (t >= pltStart && t < pltEnd) {
                    int[] c = pltCalls.computeIfAbsent(t, k -> new int[2]);
                    if ((w & 0x80000000L) != 0) c[0]++; else c[1]++;
                }
            } else if ((w & 0x9f000000L) == 0x90000000L) {
                int rd = (int) (w & 31);
                long imm = (((w >> 5) & 0x7ffffL) << 2) | ((w >> 29) & 3);
                if ((imm & 0x100000L) != 0) imm -= 0x200000L;
                long page = (o & ~0xfffL) + (imm << 12);
                for (int j = 1; j <= 8 && o + 4L * (j + 1) <= textEnd; j++) {
                    long w2 = u32(o + 4L * j);
                    int rn = (int) ((w2 >> 5) & 31), rt = (int) (w2 & 31);
                    if ((w2 & 0xff800000L) == 0x91000000L && rn == rd) {
                        long im = (w2 >> 10) & 0xfff; if (((w2 >> 22) & 1) != 0) im <<= 12;
                        long t = page + im;
                        if (t >= 0xa39000L) addXr(t, o + 4L * j);
                        if (rt == rd) break;
                    } else if ((w2 & 0x3f000000L) == 0x39000000L && rn == rd) {
                        int size = (int) (w2 >> 30);
                        long t = page + (((w2 >> 10) & 0xfff) << size);
                        if (t >= 0xa39000L) addXr(t, o + 4L * j);
                        if (rt == rd && ((w2 >> 22) & 3) != 0) break;
                    } else if ((w2 & 0x7c000000L) == 0x14000000L || (w2 & 0x9f000000L) == 0x90000000L && rt == rd) {
                        break;
                    } else if (rt == rd && (w2 & 0x1e000000L) != 0x08000000L && (w2 & 0x3b000000L) != 0x38000000L) {
                        break;
                    }
                }
            }
        }
        Arrays.sort(xr, 0, xrN);
        println("code xrefs=" + xrN + " plt-called stubs=" + pltCalls.size());
    }

    // ---------- functions ----------
    String mangledFor(long off, Map<Long, Sym> defByAddr) {
        Sym s = defByAddr.get(off);
        if (s != null) return s.name;
        for (Symbol sy : currentProgram.getSymbolTable().getSymbols(A(off)))
            if (sy.getName().startsWith("_Z")) return sy.getName();
        return "";
    }

    void exportFunctionsAndDisagreements() throws Exception {
        Map<Long, Sym> defFunc = new TreeMap<>();
        for (Sym s : dynsym) if (s.shndx != 0 && (s.info & 0xf) == 2) defFunc.put(s.value, s);
        // address-taken
        Set<Long> addrTaken = new TreeSet<>();
        for (Rel r : rels) if ((r.type == 1027 || r.type == 257) && r.target >= 0) addrTaken.add(r.target);
        // init/fini arrays and DT_INIT/FINI
        Set<Long> initArr = new TreeSet<>(), finiArr = new TreeSet<>();
        long ia = blkStart(".init_array"), ias = blkSize(".init_array"), fa = blkStart(".fini_array"), fas = blkSize(".fini_array");
        for (Rel r : rels) {
            if (r.type != 1027) continue;
            if (r.off >= ia && r.off < ia + ias) initArr.add(r.target);
            if (r.off >= fa && r.off < fa + fas) finiArr.add(r.target);
        }
        initArr.add(0x30L); // DT_INIT
        finiArr.add(0x170L); // DT_FINI
        TreeMap<Long, Function> gf = new TreeMap<>();
        for (Function f : fm.getFunctions(true)) {
            if (f.isExternal()) continue;
            gf.put(entryOf(f), f);
        }
        TreeSet<Long> all = new TreeSet<>(gf.keySet()); all.addAll(fde.keySet());
        try (PrintWriter w = open("functions.tsv",
                "functions = union of Ghidra functions and .eh_frame FDE starts; tags: fde dynsym ghidra thunk init_array(+DT_INIT) fini_array(+DT_FINI) address_taken(RELATIVE/ABS64 target); size=bytes in the contiguous range holding the entry",
                "entry_offset\tghidra_address\tsize\tbody_range\tfde_len\tname\tmangled\tdemangled\ttags")) {
            for (long e : all) {
                Function f = gf.get(e);
                List<String> tags = new ArrayList<>();
                if (fde.containsKey(e)) tags.add("fde");
                if (defFunc.containsKey(e)) tags.add("dynsym");
                if (f != null) tags.add("ghidra");
                if (f != null && f.isThunk()) tags.add("thunk");
                if (initArr.contains(e)) tags.add("init_array");
                if (finiArr.contains(e)) tags.add("fini_array");
                if (addrTaken.contains(e)) tags.add("address_taken");
                String name = f != null ? f.getName() : "";
                String mang = mangledFor(e, defFunc);
                String dem = demangle(mang);
                long size = 0; String range = "";
                if (f != null) {
                    size = contigSize(f);
                    AddressRange[] rs = null;
                    long mn = f.getBody().getMinAddress().getOffset() - base.getOffset();
                    long mx = f.getBody().getMaxAddress().getOffset() - base.getOffset();
                    int nr = f.getBody().getNumAddressRanges();
                    range = hx(mn) + "-" + hx(mx) + (nr > 1 ? "(" + nr + " ranges)" : "");
                }
                w.print(hx(e) + "\t" + hx(base.getOffset() + e) + "\t" + (f != null ? hx(size) : "") + "\t" + range + "\t"
                        + (fde.containsKey(e) ? hx(fde.get(e)) : "") + "\t" + esc(name) + "\t" + mang + "\t" + esc(dem) + "\t" + String.join(",", tags) + "\n");
            }
        }
        // disagreements
        int nFdeOnly = 0, nGhOnly = 0, nSize = 0;
        try (PrintWriter w = open("functions_disagreements.tsv",
                "kinds: FDE_NO_FUNCTION (FDE start not a Ghidra function entry; containing function shown), FUNCTION_NO_FDE (Ghidra function, non-thunk, no FDE start), SIZE_MISMATCH (ghidra_size != fde_len)",
                "kind\toffset\tghidra_size\tfde_len\tdelta\tname\tcontaining_function")) {
            for (long e : all) {
                Function f = gf.get(e);
                boolean hasFde = fde.containsKey(e);
                if (hasFde && f == null) {
                    Function c = funcAt(e);
                    w.print("FDE_NO_FUNCTION\t" + hx(e) + "\t\t" + hx(fde.get(e)) + "\t\t\t" + (c == null ? "" : hx(entryOf(c)) + ":" + c.getName()) + "\n");
                    nFdeOnly++;
                } else if (!hasFde && f != null && !f.isThunk()) {
                    boolean inText = e >= textStart && e < textEnd;
                    w.print("FUNCTION_NO_FDE\t" + hx(e) + "\t" + hx(contigSize(f)) + "\t\t\t" + esc(f.getName()) + "\t" + (inText ? "" : "outside_.text") + "\n");
                    nGhOnly++;
                } else if (hasFde && f != null) {
                    long gs = contigSize(f), fl = fde.get(e);
                    if (gs != fl) {
                        w.print("SIZE_MISMATCH\t" + hx(e) + "\t" + hx(gs) + "\t" + hx(fl) + "\t" + (gs - fl) + "\t" + esc(f.getName()) + "\t\n");
                        nSize++;
                    }
                }
            }
        }
        println("functions: ghidra=" + gf.size() + " fde=" + fde.size() + " union=" + all.size() + " fdeNoFunc=" + nFdeOnly + " funcNoFde=" + nGhOnly + " sizeMismatch=" + nSize);
        int thunks = 0, overlap = 0;
        for (Function f : gf.values()) { if (f.isThunk()) thunks++; if (fde.containsKey(entryOf(f))) overlap++; }
        println("thunks=" + thunks + " overlap=" + overlap + " defFunc=" + defFunc.size());
    }
    long contigSize(Function f) {
        for (AddressRange r : f.getBody().getAddressRanges())
            if (r.contains(f.getEntryPoint())) return r.getLength();
        return f.getBody().getNumAddresses();
    }

    // ---------- imports ----------
    String group(String n) {
        if (n.startsWith("nvn") || n.matches("^nv[A-Z].*")) return "nvn";
        if (n.startsWith("_ZN2nn") || n.startsWith("_ZNK2nn") || n.startsWith("_ZN3nn") || n.matches("^nn[a-z]+[A-Z].*") || n.startsWith("nnResult")) return "nn";
        if (n.startsWith("_ZNSt") || n.startsWith("_ZNKSt") || n.startsWith("_ZSt") || n.startsWith("_ZTVN10__cxxabiv") || n.startsWith("_ZTVSt") || n.startsWith("_ZTVNSt")
            || n.startsWith("_ZTISt") || n.startsWith("_ZTSSt") || n.startsWith("__cxa") || n.startsWith("__gxx") || n.startsWith("_Unwind") || n.matches("^_Z[nd][wa]..*") || n.matches("^_Zd[la].*"))
            return "libc++";
        if (n.startsWith("_Z")) return "other";
        return "libc";
    }

    void exportImports() throws Exception {
        // plt stub -> got slot
        Map<Long, Long> slotToStub = new TreeMap<>();
        int bad = 0;
        for (long p = pltStart; p + 16 <= pltEnd; p += 16) {
            long w0 = u32(p), w1 = u32(p + 4);
            if ((w0 & 0x9f000000L) == 0x90000000L && (w1 & 0xffc00000L) == 0xf9400000L) {
                long imm = (((w0 >> 5) & 0x7ffffL) << 2) | ((w0 >> 29) & 3);
                if ((imm & 0x100000L) != 0) imm -= 0x200000L;
                long slot = ((p & ~0xfffL) + (imm << 12)) + (((w1 >> 10) & 0xfff) << 3);
                slotToStub.put(slot, p);
            } else bad++;
        }
        println("plt stubs decoded=" + slotToStub.size() + " undecodable=" + bad);
        Map<Integer, List<Rel>> bySym = new TreeMap<>();
        for (Rel r : rels) if (r.sym != 0 && r.toImport) bySym.computeIfAbsent(r.sym, k -> new ArrayList<>()).add(r);
        Map<String, Integer> groupCnt = new TreeMap<>();
        int totCalls = 0;
        try (PrintWriter w = open("imports.tsv",
                "one row per undefined dynsym symbol, in dynsym index order; call_sites = bl_to_plt + b_to_plt + got_loads (adrp+ldr of the GOT slot); abs64_relocs = data pointers to the import",
                "dynsym_index\tname\tdemangled\tgroup\treloc_types\tgot_slots\tplt_stubs\tcall_sites\tbl_to_plt\tb_to_plt\tgot_loads\tabs64_relocs")) {
            for (Sym s : dynsym) {
                if (s.shndx != 0 || s.idx == 0 || s.name.isEmpty()) continue;
                List<Rel> rl = bySym.getOrDefault(s.idx, new ArrayList<>());
                TreeSet<String> types = new TreeSet<>(); TreeSet<Long> slots = new TreeSet<>(), stubs = new TreeSet<>();
                int bl = 0, b = 0, gl = 0, a64 = 0;
                for (Rel r : rl) {
                    types.add(relName(r.type));
                    if (r.type == 1025 || r.type == 1026) {
                        slots.add(r.off);
                        Long st = slotToStub.get(r.off);
                        if (st != null) stubs.add(st);
                        gl += xrefsIn(r.off, r.off + 1).size();
                    } else if (r.type == 257) a64++;
                }
                for (long st : stubs) { int[] c = pltCalls.get(st); if (c != null) { bl += c[0]; b += c[1]; } }
                // got loads for non-plt GLOB_DAT slots counted above (stub slots are loaded only inside the stub itself)
                int cs = bl + b + gl;
                totCalls += cs;
                String g = group(s.name);
                groupCnt.merge(g, 1, Integer::sum);
                w.print(s.idx + "\t" + s.name + "\t" + esc(demangle(s.name)) + "\t" + g + "\t" + String.join(",", types) + "\t" + joinHex(slots) + "\t" + joinHex(stubs)
                        + "\t" + cs + "\t" + bl + "\t" + b + "\t" + gl + "\t" + a64 + "\n");
            }
        }
        println("imports by group " + groupCnt + " totalCallSites=" + totCalls);
    }
    String capList(List<String> l, int n) { return l.size() <= n ? String.join(", ", l) : String.join(", ", l.subList(0, n)) + " (+" + (l.size() - n) + " more)"; }
    String joinHex(Collection<Long> c) { StringBuilder sb = new StringBuilder(); for (long v : c) { if (sb.length() > 0) sb.append(';'); sb.append(hx(v)); } return sb.toString(); }
    String relName(int t) {
        switch (t) { case 1027: return "RELATIVE"; case 257: return "ABS64"; case 1025: return "GLOB_DAT"; case 1026: return "JUMP_SLOT"; default: return "TYPE_" + t; }
    }

    // ---------- relocations ----------
    void exportRelocations() throws Exception {
        Map<String, Integer> byType = new TreeMap<>();
        Map<String, Integer> byTypeSeg = new TreeMap<>();
        Map<String, Integer> tgtKind = new TreeMap<>();
        try (PrintWriter w = open("relocations.tsv",
                "from .rela.dyn + .rela.plt sorted by offset; target = addend (RELATIVE), sym value+addend (ABS64 defined), import => target blank",
                "offset\ttype\tsymbol\taddend\tsrc_segment\ttarget\ttarget_segment\ttarget_function\ttarget_function_name\ttarget_is_function_start")) {
            for (Rel r : rels) {
                String t = relName(r.type);
                byType.merge(t, 1, Integer::sum);
                String sym = r.sym == 0 ? "" : dynsym.get(r.sym).name;
                String tseg = "", tf = "", tfn = "", fs = "";
                if (r.toImport) tseg = "UNDEF";
                else if (r.target >= 0) {
                    tseg = seg(r.target);
                    if (r.target < textEnd) {
                        Function f = funcAt(r.target);
                        if (f != null) { tf = hx(entryOf(f)); tfn = f.getName(); fs = entryOf(f) == r.target ? "1" : "0"; }
                        else fs = fde.containsKey(r.target) ? "1" : "0";
                    }
                }
                byTypeSeg.merge(t + "\t" + (tseg.isEmpty() ? "-" : tseg), 1, Integer::sum);
                if (fs.equals("1")) tgtKind.merge(t, 1, Integer::sum);
                w.print(hx(r.off) + "\t" + t + "\t" + sym + "\t" + (r.addend == 0 ? "0" : hx(r.addend)) + "\t" + seg(r.off) + "\t" + (r.target >= 0 ? hx(r.target) : "") + "\t" + tseg
                        + "\t" + tf + "\t" + esc(tfn) + "\t" + fs + "\n");
            }
        }
        try (PrintWriter w = open("relocations_summary.md", "summary", null)) {
            // md: convert header lines (already '# ...') into a code-free preamble
            w.print("\n# Relocation summary\n\nTotal: " + rels.size() + "\n\n| type | count | targeting a function start |\n|---|---|---|\n");
            for (Map.Entry<String, Integer> e : byType.entrySet()) w.print("| " + e.getKey() + " | " + e.getValue() + " | " + tgtKind.getOrDefault(e.getKey(), 0) + " |\n");
            w.print("\n## By type and target segment\n\n| type | target segment | count |\n|---|---|---|\n");
            for (Map.Entry<String, Integer> e : byTypeSeg.entrySet()) { String[] p = e.getKey().split("\t"); w.print("| " + p[0] + " | " + p[1] + " | " + e.getValue() + " |\n"); }
        }
        println("relocs by type " + byType);
    }

    // ---------- RTTI ----------
    void exportRtti() throws Exception {
        TreeMap<String, String[]> rows = new TreeMap<>();
        for (Sym s : dynsym) {
            if (s.name.startsWith("_ZTI") || s.name.startsWith("_ZTS") || s.name.startsWith("_ZTV")) {
                String k = s.shndx != 0 ? hx(s.value) : "UNDEF";
                rows.put(String.format("%016x|%s", s.shndx != 0 ? s.value : -1L, s.name), new String[]{k, s.name.substring(0, 4), s.name, esc(demangle(s.name)), s.shndx != 0 ? hx(s.size) : "", s.shndx != 0 ? "yes" : "no", "dynsym", s.shndx != 0 ? seg(s.value) : "UNDEF"});
            }
        }
        for (Symbol sy : currentProgram.getSymbolTable().getAllSymbols(false)) {
            if (sy.isExternal()) continue;
            String n = sy.getName();
            if (n.startsWith("_ZTI") || n.startsWith("_ZTS") || n.startsWith("_ZTV")) {
                long off = sy.getAddress().getOffset() - base.getOffset();
                String key = String.format("%016x|%s", off, n);
                if (!rows.containsKey(key)) rows.put(key, new String[]{hx(off), n.substring(0, 4), n, esc(demangle(n)), "", "yes", "ghidra", seg(off)});
            }
        }
        try (PrintWriter w = open("rtti.tsv", "typeinfo/typeinfo-name/vtable symbols (_ZTI/_ZTS/_ZTV) from dynsym and Ghidra symbols; defined=no means imported from the runtime",
                "offset\tkind\tmangled\tdemangled\tsize\tdefined\tsource\tsegment")) {
            for (String[] r : rows.values()) w.print(String.join("\t", r) + "\n");
        }
        println("rtti rows=" + rows.size());
    }

    // ---------- vtable candidates ----------
    List<long[]> exportVtables() throws Exception {
        Set<Long> funcStarts = new TreeSet<>(fde.keySet());
        for (Function f : fm.getFunctions(true)) if (!f.isExternal()) funcStarts.add(entryOf(f));
        TreeMap<Long, Rel> relAt = new TreeMap<>();
        for (Rel r : rels) if (r.type == 1027 || r.type == 257) relAt.put(r.off, r);
        Set<String> okSeg = new HashSet<>(Arrays.asList(".data", ".data.1", ".data.rel.ro", ".rodata", ".rodata.1"));
        List<long[]> out = new ArrayList<>(); // {start, count, runEnd}
        List<List<String>> entries = new ArrayList<>();
        long runStart = -1; long prev = -2; List<String> cur = new ArrayList<>(); int fnCount = 0;
        List<Long> offs = new ArrayList<>(relAt.keySet());
        offs.add(Long.MAX_VALUE);
        for (long o : offs) {
            boolean good = false; String cell = "";
            if (o != Long.MAX_VALUE) {
                Rel r = relAt.get(o);
                if (okSeg.contains(seg(o))) {
                    if (r.type == 1027 && r.target >= 0 && funcStarts.contains(r.target)) { good = true; cell = hx(r.target); }
                    else if (r.type == 257 && r.toImport) { good = true; cell = "@" + dynsym.get(r.sym).name; }
                    else if (r.type == 257 && r.target >= 0 && funcStarts.contains(r.target)) { good = true; cell = hx(r.target); }
                }
            }
            if (good && o == prev + 8 && runStart >= 0) { cur.add(cell); if (!cell.startsWith("@")) fnCount++; prev = o; }
            else {
                if (runStart >= 0 && fnCount >= 3 && cur.size() >= 3) { out.add(new long[]{runStart, cur.size(), fnCount}); entries.add(cur); }
                if (good) { runStart = o; cur = new ArrayList<>(); cur.add(cell); fnCount = cell.startsWith("@") ? 0 : 1; prev = o; }
                else { runStart = -1; cur = new ArrayList<>(); fnCount = 0; prev = -2; }
            }
        }
        // function -> vtable membership
        Map<Long, List<String>> member = new TreeMap<>();
        for (int i = 0; i < out.size(); i++) {
            int slot = 0;
            for (String c : entries.get(i)) {
                if (!c.startsWith("@")) member.computeIfAbsent(Long.decode(c), k -> new ArrayList<>()).add(hx(out.get(i)[0]) + "#" + slot);
                slot++;
            }
        }
        vtMember = member;
        Map<Long, List<String>> ctorOf = new TreeMap<>();
        int withRefs = 0;
        try (PrintWriter w = open("vtables_candidate.tsv",
                "runs of >=3 consecutive RELATIVE (or ABS64 import, shown @sym) pointers in data/rodata whose function targets are function starts; ref_functions = functions with an adrp+add/ldr into [start-16, end) (cap 16) plus Ghidra refs",
                "start_offset\tentries\tfunction_entries\tprefix_words\tprefix_has_reloc\tn_ref_functions\tn_data_refs\tref_functions\tslots")) {
            for (int i = 0; i < out.size(); i++) {
                long s = out.get(i)[0], n = out.get(i)[1];
                long end = s + 8 * n;
                String prefix = "";
                if (s >= 16) prefix = hx(u64(s - 16)) + "," + hx(u64(s - 8));
                boolean pr = s >= 16 && (relAt.containsKey(s - 16) || relAt.containsKey(s - 8));
                TreeSet<Long> refFns = new TreeSet<>();
                for (long site : xrefsIn(s - 16, end)) {
                    Function f = funcAt(site);
                    if (f != null) refFns.add(entryOf(f));
                }
                // Ghidra references
                for (long a = s - 16; a < end; a += 8) {
                    for (Reference ref : currentProgram.getReferenceManager().getReferencesTo(A(a))) {
                        if (!ref.getReferenceType().isData()) continue;
                        Function f = fm.getFunctionContaining(ref.getFromAddress());
                        if (f != null) refFns.add(entryOf(f));
                    }
                }
                for (long fe : refFns) ctorOf.computeIfAbsent(fe, k -> new ArrayList<>()).add(hx(s));
                if (!refFns.isEmpty()) withRefs++;
                // data refs: RELATIVE relocs targeting [s-16,end)
                int dataRefs = dataRefCount(s - 16, end);
                StringBuilder rf = new StringBuilder(); int c = 0;
                for (long fe : refFns) { if (c++ >= 16) break; if (rf.length() > 0) rf.append(';'); rf.append(hx(fe)); }
                w.print(hx(s) + "\t" + n + "\t" + out.get(i)[2] + "\t" + prefix + "\t" + (pr ? "1" : "0") + "\t" + refFns.size() + "\t" + dataRefs + "\t" + rf + "\t" + String.join(";", entries.get(i)) + "\n");
            }
        }
        vtCtor = ctorOf;
        println("vtable candidates=" + out.size() + " with referencing functions=" + withRefs);
        return out;
    }
    Map<Long, List<String>> vtMember = new TreeMap<>(), vtCtor = new TreeMap<>();
    long[] relTargetsSorted;
    int dataRefCount(long lo, long hi) {
        if (relTargetsSorted == null) {
            List<Long> t = new ArrayList<>();
            for (Rel r : rels) if (r.type == 1027 && r.target >= 0) t.add(r.target);
            Collections.sort(t);
            relTargetsSorted = new long[t.size()];
            for (int i = 0; i < t.size(); i++) relTargetsSorted[i] = t.get(i);
        }
        int a = Arrays.binarySearch(relTargetsSorted, lo), b;
        a = lb(relTargetsSorted, lo); b = lb(relTargetsSorted, hi);
        return b - a;
    }
    int lb(long[] arr, long key) { int lo = 0, hi = arr.length; while (lo < hi) { int m = (lo + hi) >>> 1; if (arr[m] < key) lo = m + 1; else hi = m; } return lo; }
    List<Long> dataRefSites(long lo, long hi) {
        List<Long> out = new ArrayList<>();
        for (Rel r : rels) if (r.type == 1027 && r.target >= lo && r.target < hi) out.add(r.off);
        return out;
    }

    String callersOf(long target) {
        TreeSet<String> c = new TreeSet<>();
        for (long o = textStart; o + 4 <= textEnd; o += 4) {
            long wd = u32(o);
            if ((wd & 0x7c000000L) == 0x14000000L) {
                long imm = wd & 0x3ffffffL; if ((imm & 0x2000000L) != 0) imm -= 0x4000000L;
                if (o + imm * 4 == target) { Function f = funcAt(o); c.add(hx(o) + (f == null ? "" : " in " + hx(entryOf(f)))); }
            }
        }
        return c.isEmpty() ? "none (address-taken or indirect)" : String.join("; ", c);
    }

    // ---------- render passes / strings / NVN ----------
    static final Pattern PASS = Pattern.compile("(?i)(nu4|lumen|renderpass|^nu::|forward|deferred|zprepass|shadow|ssao|bloom|tonemap|fxaa|\\bdof|photoedit|photo edit|gbuffer|swapchain|composite|lentil|distortion)");
    String category(String s) {
        String l = s.toLowerCase();
        if (s.contains("nu::RenderPassBase") || l.contains("renderpass")) return "RenderPass";
        if (l.contains("nu4/") || s.startsWith("nu::")) return "nu4";
        if (l.contains("lumen")) return "lumen";
        if (l.contains("photoedit") || l.contains("photo edit")) return "photo_edit";
        if (l.contains("ssao")) return "ssao";
        if (l.contains("bloom")) return "bloom";
        if (l.contains("tonemap")) return "tonemap";
        if (l.contains("fxaa")) return "fxaa";
        if (l.contains("shadow")) return "shadow";
        if (l.contains("dof")) return "dof";
        if (l.contains("forward") || l.contains("deferred") || l.contains("zprepass") || l.contains("gbuffer")) return "pass_name";
        return "other";
    }

    void exportRenderPasses(List<long[]> vts) throws Exception {
        // string scan
        List<Str> strs = new ArrayList<>(), nvns = new ArrayList<>();
        int totalStrings = 0;
        for (String bn : new String[]{".rodata", ".rodata.1"}) {
            long s0 = blkStart(bn), e0 = s0 + blkSize(bn);
            long o = s0;
            while (o < e0) {
                if (o > s0 && u8(o - 1) != 0) { o++; continue; }
                long p = o;
                while (p < e0) { long c = u8(p); if ((c >= 0x20 && c < 0x7f) || c == 9 || c == 10 || c == 13) p++; else break; }
                if (p < e0 && u8(p) == 0 && p - o >= 4) {
                    totalStrings++;
                    String s = new String(img, (int) o, (int) (p - o), StandardCharsets.ISO_8859_1);
                    if (s.length() <= 2000) {
                        Str st = new Str(); st.off = o; st.s = s;
                        if (s.matches("^nvn[A-Z][A-Za-z0-9]*$")) nvns.add(st);
                        else if (PASS.matcher(s).find()) { st.cat = category(s); strs.add(st); }
                    }
                    o = p + 1;
                } else o = Math.max(o + 1, p);
            }
        }
        println("strings scanned=" + totalStrings + " pass-matched=" + strs.size() + " nvn=" + nvns.size());
        // data xrefs (RELATIVE relocs into strings)
        List<long[]> rp = new ArrayList<>();
        for (Rel r : rels) if (r.type == 1027 && r.target >= 0) rp.add(new long[]{r.target, r.off});
        rp.sort((a, b) -> a[0] != b[0] ? Long.compare(a[0], b[0]) : Long.compare(a[1], b[1]));
        long[] rpt = new long[rp.size()]; for (int i = 0; i < rpt.length; i++) rpt[i] = rp.get(i)[0];
        Map<Long, Integer> fnStrCount = new TreeMap<>();
        Map<Long, List<String>> fnStrs = new TreeMap<>();
        int noXref = 0, rows = 0;
        Map<String, Integer> catCnt = new TreeMap<>();
        try (PrintWriter w = open("render_passes.tsv",
                "strings (NU4/Lumen/RenderPassBase/pass names) in .rodata with xrefs (code = adrp+add/ldr site, data = RELATIVE reloc slot); vtable_member = vtable#slot where the referencing function is a method; vtable_ctor_of = vtables the function references",
                "string_offset\tstring\tcategory\txref_kind\txref_site\tfunction\tfunction_name\tvtable_member\tvtable_ctor_of")) {
            strs.sort((a, b) -> Long.compare(a.off, b.off));
            for (Str st : strs) {
                catCnt.merge(st.cat, 1, Integer::sum);
                long lo = st.off, hi = st.off + st.s.length() + 1;
                List<long[]> refs = new ArrayList<>();
                for (long site : xrefsIn(lo, hi)) refs.add(new long[]{0, site});
                for (int i = lb(rpt, lo); i < rpt.length && rpt[i] < hi; i++) refs.add(new long[]{1, rp.get(i)[1]});
                refs.sort((a, b) -> a[0] != b[0] ? Long.compare(a[0], b[0]) : Long.compare(a[1], b[1]));
                if (refs.isEmpty()) { noXref++; w.print(hx(st.off) + "\t" + esc(st.s) + "\t" + st.cat + "\t\t\t\t\t\t\n"); rows++; continue; }
                for (long[] rf : refs) {
                    String fn = "\t", vm = "", vc = "";
                    if (rf[0] == 0) {
                        Function f = funcAt(rf[1]);
                        if (f != null) {
                            long e = entryOf(f);
                            fn = hx(e) + "\t" + esc(f.getName());
                            vm = String.join(",", vtMember.getOrDefault(e, Collections.emptyList()));
                            vc = String.join(",", vtCtor.getOrDefault(e, Collections.emptyList()));
                            fnStrCount.merge(e, 1, Integer::sum);
                            fnStrs.computeIfAbsent(e, k -> new ArrayList<>()).add(st.s.length() > 40 ? st.s.substring(0, 40) + "..." : st.s);
                        }
                    }
                    w.print(hx(st.off) + "\t" + esc(st.s) + "\t" + st.cat + "\t" + (rf[0] == 0 ? "code" : "data") + "\t" + hx(rf[1]) + "\t" + fn + "\t" + vm + "\t" + vc + "\n");
                    rows++;
                }
            }
        }
        // NVN strings
        Map<Long, Integer> nvnFn = new TreeMap<>();
        int nvnNoRef = 0, nvnData = 0, nvnCode = 0; boolean contiguous = true; long prevEnd = -1;
        List<Long> nvnSites = new ArrayList<>();
        nvns.sort((a, b) -> Long.compare(a.off, b.off));
        try (PrintWriter w = open("nvn_procs.tsv", "strings 'nvn[A-Z]*' (NVN proc names) with xrefs; order = position in .rodata",
                "string_offset\tstring\txref_kind\txref_site\tfunction\tfunction_name")) {
            for (Str st : nvns) {
                if (prevEnd >= 0 && st.off != prevEnd) contiguous = false;
                prevEnd = st.off + st.s.length() + 1;
                long lo = st.off, hi = st.off + st.s.length() + 1;
                List<Long> sites = xrefsIn(lo, hi);
                int dr = 0; for (int i = lb(rpt, lo); i < rpt.length && rpt[i] < hi; i++) dr++;
                if (sites.isEmpty() && dr == 0) { nvnNoRef++; w.print(hx(st.off) + "\t" + st.s + "\t\t\t\t\n"); continue; }
                for (long site : sites) {
                    nvnCode++;
                    Function f = funcAt(site);
                    nvnSites.add(site);
                    if (f != null) nvnFn.merge(entryOf(f), 1, Integer::sum);
                    w.print(hx(st.off) + "\t" + st.s + "\tcode\t" + hx(site) + "\t" + fcell(site) + "\n");
                }
                for (int i = lb(rpt, lo); i < rpt.length && rpt[i] < hi; i++) { nvnData++; w.print(hx(st.off) + "\t" + st.s + "\tdata\t" + hx(rp.get(i)[1]) + "\t\t\n"); }
            }
        }
        // bootstrap loader import
        TreeSet<String> boot = new TreeSet<>();
        for (Sym s : dynsym) {
            if (!s.name.equals("nvnBootstrapLoader")) continue;
            for (Rel r : rels) if (r.sym == s.idx && (r.type == 1025 || r.type == 1026)) {
                Long stub = null;
                for (long p = pltStart; p + 16 <= pltEnd; p += 16) {
                    long w0 = u32(p), w1 = u32(p + 4);
                    if ((w0 & 0x9f000000L) == 0x90000000L && (w1 & 0xffc00000L) == 0xf9400000L) {
                        long imm = (((w0 >> 5) & 0x7ffffL) << 2) | ((w0 >> 29) & 3); if ((imm & 0x100000L) != 0) imm -= 0x200000L;
                        if (((p & ~0xfffL) + (imm << 12)) + (((w1 >> 10) & 0xfff) << 3) == r.off) stub = p;
                    }
                }
                boot.add("slot " + hx(r.off) + " (" + relName(r.type) + ")" + (stub != null ? " plt " + hx(stub) : ""));
                for (long site : xrefsIn(r.off, r.off + 1)) boot.add("got-load site " + hx(site) + " in " + fcell(site).replace('\t', ' '));
                if (stub != null) {
                    // find bl/b sites to the stub
                    for (long o = textStart; o + 4 <= textEnd; o += 4) {
                        long wd = u32(o);
                        if ((wd & 0x7c000000L) == 0x14000000L) {
                            long imm = wd & 0x3ffffffL; if ((imm & 0x2000000L) != 0) imm -= 0x4000000L;
                            if (o + imm * 4 == stub) boot.add("call site " + hx(o) + " in " + fcell(o).replace('\t', ' '));
                        }
                    }
                }
            }
        }
        // ---- md ----
        try (PrintWriter w = open("render_passes.md", "render pass / NVN summary (generated)", null)) {
            w.print("\n# Render passes and NVN bootstrap\n\n");
            w.print("Strings scanned in .rodata/.rodata.1: " + totalStrings + ". Pass-related strings matched: " + strs.size() + " (rows in render_passes.tsv: " + rows + ", strings without xrefs: " + noXref + ").\n\n");
            w.print("| category | strings |\n|---|---|\n");
            for (Map.Entry<String, Integer> e : catCnt.entrySet()) w.print("| " + e.getKey() + " | " + e.getValue() + " |\n");
            w.print("\n## Functions referencing the most pass strings\n\n| function | name | #strings | vtable member | vtable ctor of | sample strings |\n|---|---|---|---|---|---|\n");
            List<Map.Entry<Long, Integer>> top = new ArrayList<>(fnStrCount.entrySet());
            top.sort((a, b) -> a.getValue() != b.getValue().intValue() ? Integer.compare(b.getValue(), a.getValue()) : Long.compare(a.getKey(), b.getKey()));
            for (int i = 0; i < Math.min(60, top.size()); i++) {
                long e = top.get(i).getKey();
                Function f = funcAt(e);
                List<String> ss = fnStrs.get(e); List<String> sm = new ArrayList<>(new LinkedHashSet<>(ss));
                w.print("| " + hx(e) + " | " + (f == null ? "" : f.getName()) + " | " + top.get(i).getValue() + " | " + String.join(", ", vtMember.getOrDefault(e, Collections.emptyList())) + " | "
                        + capList(vtCtor.getOrDefault(e, Collections.emptyList()), 4) + " | " + esc(String.join("; ", sm.subList(0, Math.min(8, sm.size())))).replace("|", "\\|") + " |\n");
            }
            w.print("\n(" + fnStrCount.size() + " distinct functions reference at least one matched string through adrp+add/ldr.)\n");
            w.print("\n## RenderPassBase / graph strings\n\n");
            for (Str st : strs) if (st.cat.equals("RenderPass")) {
                w.print("- string " + hx(st.off) + " (" + st.s.length() + " chars): `" + esc(st.s.length() > 120 ? st.s.substring(0, 120) + "..." : st.s) + "`\n");
                for (long site : xrefsIn(st.off, st.off + st.s.length() + 1)) w.print("  - code xref " + hx(site) + " in " + fcell(site).replace('\t', ' ') + "\n");
            }
            w.print("\n## NVN proc-address bootstrap\n\n");
            w.print("- NVN proc-name strings (`nvn[A-Z]...`): " + nvns.size() + "; contiguous in .rodata: " + contiguous + "; strings with no xref: " + nvnNoRef + "; code xrefs: " + nvnCode + "; data (pointer table) xrefs: " + nvnData + ".\n");
            w.print("- Functions holding those code xrefs (entry: #refs): " + nvnFn + "\n");
            if (!nvnSites.isEmpty()) { Collections.sort(nvnSites); w.print("- xref site range: " + hx(nvnSites.get(0)) + " .. " + hx(nvnSites.get(nvnSites.size() - 1)) + "\n"); }
            for (long fe : nvnFn.keySet()) {
                List<long[]> pairs = new ArrayList<>();
                for (Str st : nvns) for (long site : xrefsIn(st.off, st.off + st.s.length() + 1)) { Function f = funcAt(site); if (f != null && entryOf(f) == fe) pairs.add(new long[]{site, st.off}); }
                pairs.sort((a, b) -> Long.compare(a[0], b[0]));
                int inc = 0; for (int i = 1; i < pairs.size(); i++) if (pairs.get(i)[1] > pairs.get(i - 1)[1]) inc++;
                Function f = funcAt(fe);
                w.print("- function " + hx(fe) + " " + (f == null ? "" : f.getName()) + ": " + pairs.size() + " string refs, sites " + hx(pairs.get(0)[0]) + ".." + hx(pairs.get(pairs.size() - 1)[0])
                        + ", string order increases in " + inc + "/" + Math.max(0, pairs.size() - 1) + " consecutive steps; bl/b callers: " + callersOf(fe) + "\n");
            }
            w.print("- Import `nvnBootstrapLoader`:\n");
            for (String b : boot) w.print("  - " + b + "\n");
        }
        println("render passes: matched=" + strs.size() + " noXref=" + noXref + " fns=" + fnStrCount.size() + " nvn=" + nvns.size() + " nvnFns=" + nvnFn + " contiguous=" + contiguous + " nvnData=" + nvnData + " nvnCode=" + nvnCode);
    }
}
