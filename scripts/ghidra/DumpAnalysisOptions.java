// Writes the analyzer options of the current program (sorted), for provenance.
import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import java.io.*;
import java.util.*;

public class DumpAnalysisOptions extends GhidraScript {
    @Override
    public void run() throws Exception {
        Options o = currentProgram.getOptions("Analyzers");
        List<String> names = new ArrayList<>(o.getOptionNames());
        Collections.sort(names);
        try (PrintWriter w = new PrintWriter(new File(getScriptArgs()[0]), "UTF-8")) {
            for (String n : names) w.println(n + "\t" + o.getObject(n, null));
        }
    }
}
