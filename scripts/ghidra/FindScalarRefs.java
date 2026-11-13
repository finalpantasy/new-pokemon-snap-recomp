// Find instructions with a scalar operand equal to given values. Usage (headless, read-only): -postScript FindScalarRefs.java <out.txt> <hex,hex,...>
// Output: value, instruction address (module-relative), containing function entry, mnemonic text.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;

public class FindScalarRefs extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Set<Long> want = new HashSet<>();
        for (String s : a[1].split(",")) want.add(Long.parseLong(s.trim(), 16));
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                int n = ins.getNumOperands();
                for (int i = 0; i < n; i++) {
                    for (Object o : ins.getOpObjects(i)) {
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            if (want.contains(v)) {
                                Function f = getFunctionContaining(ins.getAddress());
                                w.println(Long.toHexString(v) + "\t" + Long.toHexString(ins.getAddress().getOffset() - 0x7100000000L) + "\t" + (f == null ? "-" : Long.toHexString(f.getEntryPoint().getOffset() - 0x7100000000L)) + "\t" + ins.toString());
                            }
                        }
                    }
                }
            }
        }
    }
}
