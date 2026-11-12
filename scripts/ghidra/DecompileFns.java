// Decompile functions to text. Usage (headless): -process <prog> -noanalysis -readOnly -postScript DecompileFns.java <out.txt> <hexaddr,hexaddr,...>
// Addresses are module-relative (image base 0x7100000000). Read-only.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DecompileFns extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (String s : a[1].split(",")) {
                long off = Long.parseLong(s.trim(), 16);
                Address ad = toAddr(0x7100000000L + off);
                Function f = getFunctionContaining(ad);
                if (f == null) { w.println("// no function at " + s); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " (req " + s + ")");
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
