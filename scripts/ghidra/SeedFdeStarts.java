// Pre-analysis seeding. Parses .eh_frame (located via MOD0 eh_frame_hdr), writes every FDE (start,length)
// to <fde.tsv>, creates a function at each FDE start, plus DT_INIT, DT_FINI, init_array and fini_array targets.
// Usage (headless preScript): SeedFdeStarts.java <fde.tsv out>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.io.*;
import java.util.*;

public class SeedFdeStarts extends GhidraScript {
    Memory mem; Address base;
    long off;

    long rd(Address a, int n) throws Exception {
        long v = 0;
        for (int i = 0; i < n; i++) v |= ((long) (mem.getByte(a.add(i)) & 0xff)) << (8 * i);
        return v;
    }
    int u8() throws Exception { return (int) rd(base.add(off++), 1); }
    long uleb() throws Exception { long r = 0; int s = 0; while (true) { int b = u8(); r |= (long) (b & 0x7f) << s; s += 7; if ((b & 0x80) == 0) return r; } }
    long sleb() throws Exception { long r = 0; int s = 0; int b; do { b = u8(); r |= (long) (b & 0x7f) << s; s += 7; } while ((b & 0x80) != 0); if (s < 64 && (b & 0x40) != 0) r |= -1L << s; return r; }
    long enc(int e) throws Exception {
        long here = off; long v;
        switch (e & 0x0f) {
            case 0x00: v = rd(base.add(off), 8); off += 8; break;
            case 0x02: v = rd(base.add(off), 2); off += 2; break;
            case 0x03: v = rd(base.add(off), 4); off += 4; break;
            case 0x04: v = rd(base.add(off), 8); off += 8; break;
            case 0x0a: v = (short) rd(base.add(off), 2); off += 2; break;
            case 0x0b: v = (int) rd(base.add(off), 4); off += 4; break;
            case 0x0c: v = rd(base.add(off), 8); off += 8; break;
            default: throw new RuntimeException("enc " + e);
        }
        if ((e & 0x70) == 0x10) v += here;
        return v;
    }

    @Override
    public void run() throws Exception {
        mem = currentProgram.getMemory();
        base = currentProgram.getImageBase();
        File out = new File(getScriptArgs()[0]);
        long mod0 = rd(base.add(4), 4); // header word at +4 holds MOD0 offset
        if (rd(base.add(mod0), 4) != 0x30444f4dL) throw new RuntimeException("MOD0 not found at " + mod0);
        long dynOff = mod0 + (int) rd(base.add(mod0 + 4), 4);
        long ehStart = mod0 + (int) rd(base.add(mod0 + 0x10), 4);
        off = ehStart;
        int ver = u8(), ptrEnc = u8(), cntEnc = u8(), tabEnc = u8();
        long ehFrame = enc(ptrEnc);
        long fdeCount = enc(cntEnc);
        println("eh_frame_hdr ver=" + ver + " eh_frame=0x" + Long.toHexString(ehFrame) + " hdr FDE count=" + fdeCount);

        Map<Long, long[]> cies = new HashMap<>();
        TreeMap<Long, Long> fdes = new TreeMap<>();
        long p = ehFrame;
        int walked = 0;
        while (true) {
            off = p;
            long len = rd(base.add(off), 4); off += 4;
            if (len == 0) break;
            if (len == 0xffffffffL) { len = rd(base.add(off), 8); off += 8; }
            long bodyStart = off;
            long next = bodyStart + len;
            long id = rd(base.add(off), 4); off += 4;
            if (id == 0) {
                int cver = u8();
                StringBuilder aug = new StringBuilder();
                int c; while ((c = u8()) != 0) aug.append((char) c);
                String a = aug.toString();
                if (a.contains("eh")) off += 8;
                uleb(); sleb();
                if (cver == 1) u8(); else uleb();
                long fe = 0;
                if (a.startsWith("z")) {
                    long al = uleb(); long aend = off + al;
                    for (int i = 1; i < a.length(); i++) {
                        char ch = a.charAt(i);
                        if (ch == 'R') fe = u8();
                        else if (ch == 'L') u8();
                        else if (ch == 'P') { int pe = u8(); enc(pe & 0x7f); }
                    }
                    off = aend;
                }
                cies.put(p, new long[]{fe});
            } else {
                long cieOff = bodyStart - id;
                long[] ci = cies.get(cieOff);
                if (ci == null) throw new RuntimeException("CIE missing for FDE at 0x" + Long.toHexString(p));
                int fe = (int) ci[0];
                long start = enc(fe);
                long range = enc(fe & 0x0f);
                fdes.put(start, range);
                walked++;
            }
            p = next;
        }
        println("walked FDEs=" + walked + " unique starts=" + fdes.size());
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("# start_offset\tlength");
            for (Map.Entry<Long, Long> e : fdes.entrySet())
                w.println(String.format("0x%x\t0x%x", e.getKey(), e.getValue()));
        }
        TreeSet<Long> seeds = new TreeSet<>(fdes.keySet());
        long dInit = -1, dFini = -1, ia = -1, ias = 0, fa = -1, fas = 0;
        for (long q = dynOff; ; q += 16) {
            long tag = rd(base.add(q), 8), val = rd(base.add(q + 8), 8);
            if (tag == 0) break;
            if (tag == 12) dInit = val; else if (tag == 13) dFini = val;
            else if (tag == 25) ia = val; else if (tag == 27) ias = val;
            else if (tag == 26) fa = val; else if (tag == 28) fas = val;
        }
        if (dInit >= 0) seeds.add(dInit);
        if (dFini >= 0) seeds.add(dFini);
        for (int k = 0; k < 2; k++) {
            long arr = k == 0 ? ia : fa, sz = k == 0 ? ias : fas;
            for (long i = 0; arr >= 0 && i < sz / 8; i++) {
                Address ea = base.add(arr + 8 * i);
                long v = rd(ea, 8);
                if (v >= base.getOffset()) v -= base.getOffset();
                if (v == 0 || v > 0xa38e40L) {
                    var rl = currentProgram.getRelocationTable().getRelocations(ea);
                    if (!rl.isEmpty() && rl.get(0).getValues().length > 0) v = rl.get(0).getValues()[0];
                    if (v >= base.getOffset()) v -= base.getOffset();
                }
                if (i < 3) println("array " + k + "[" + i + "] -> 0x" + Long.toHexString(v));
                if (v > 0 && v < 0xa38e40L) seeds.add(v);
            }
        }
        println("DT_INIT=" + dInit + " DT_FINI=" + dFini + " seeds=" + seeds.size());
        int made = 0, existing = 0, failed = 0;
        for (long s : seeds) {
            Address a = base.add(s);
            if (getFunctionAt(a) != null) { existing++; continue; }
            if (getInstructionAt(a) == null) {
                DisassembleCommand dc = new DisassembleCommand(a, null, true);
                dc.applyTo(currentProgram, monitor);
            }
            CreateFunctionCmd cmd = new CreateFunctionCmd(null, a, null, SourceType.ANALYSIS);
            if (cmd.applyTo(currentProgram, monitor)) made++; else failed++;
        }
        println("seeded functions made=" + made + " existing=" + existing + " failed=" + failed);
    }
}
