// Callers/references to functions. Usage (headless, read-only): -postScript FindCallers.java <out.txt> <hexaddr,hexaddr,...> (module-relative)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class FindCallers extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (String s : a[1].split(",")) {
                long off = Long.parseLong(s.trim(), 16);
                Address ad = toAddr(0x7100000000L + off);
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(ad);
                while (it.hasNext()) {
                    Reference r = it.next();
                    Function f = getFunctionContaining(r.getFromAddress());
                    w.println(s + "\t" + Long.toHexString(r.getFromAddress().getOffset() - 0x7100000000L) + "\t" + (f == null ? "-" : Long.toHexString(f.getEntryPoint().getOffset() - 0x7100000000L)) + "\t" + r.getReferenceType());
                }
            }
        }
    }
}
