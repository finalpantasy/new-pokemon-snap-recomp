// Exports memory map, entry points, symbol and relocation counts of the current program.
// Usage (headless): -postScript ExportImportInfo.java <outfile.json>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.reloc.*;
import java.io.*;
import java.util.*;

public class ExportImportInfo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "import-info.json");
        StringBuilder sb = new StringBuilder("{\n");
        sb.append(" \"program\": \"").append(currentProgram.getName()).append("\",\n");
        sb.append(" \"format\": \"").append(currentProgram.getExecutableFormat()).append("\",\n");
        sb.append(" \"language\": \"").append(currentProgram.getLanguageID()).append("\",\n");
        sb.append(" \"imageBase\": \"").append(currentProgram.getImageBase()).append("\",\n");
        sb.append(" \"blocks\": [\n");
        MemoryBlock[] bl = currentProgram.getMemory().getBlocks();
        for (int i = 0; i < bl.length; i++) {
            MemoryBlock b = bl[i];
            sb.append(String.format("  {\"name\":\"%s\",\"start\":\"%s\",\"end\":\"%s\",\"size\":\"0x%x\",\"perms\":\"%s%s%s\",\"init\":%b}%s\n",
                b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-",
                b.isInitialized(), i + 1 < bl.length ? "," : ""));
        }
        sb.append(" ],\n");
        SymbolTable st = currentProgram.getSymbolTable();
        List<String> eps = new ArrayList<>();
        AddressIterator ei = st.getExternalEntryPointIterator();
        while (ei.hasNext()) eps.add("\"" + ei.next() + "\"");
        sb.append(" \"entryPoints\": [").append(String.join(",", eps)).append("],\n");
        int ext = 0, extLoc = 0, total = 0, funcs = 0, globalSyms = 0;
        SymbolIterator si = st.getAllSymbols(true);
        while (si.hasNext()) {
            Symbol s = si.next();
            total++;
            if (s.isExternal()) ext++;
            if (s.isGlobal() && !s.isExternal()) globalSyms++;
        }
        ExternalManager em = currentProgram.getExternalManager();
        extLoc = 0;
        for (String n : em.getExternalLibraryNames()) {
            ExternalLocationIterator it = em.getExternalLocations(n);
            while (it.hasNext()) { it.next(); extLoc++; }
        }
        funcs = currentProgram.getFunctionManager().getFunctionCount();
        sb.append(" \"symbolsTotal\": ").append(total).append(",\n");
        sb.append(" \"externalSymbols\": ").append(ext).append(",\n");
        sb.append(" \"externalLocations\": ").append(extLoc).append(",\n");
        sb.append(" \"nonExternalGlobalSymbols\": ").append(globalSyms).append(",\n");
        sb.append(" \"functions\": ").append(funcs).append(",\n");
        sb.append(" \"relocations\": ").append(currentProgram.getRelocationTable().getSize()).append(",\n");
        // relocation types histogram
        Map<Integer, Integer> h = new TreeMap<>();
        Iterator<Relocation> ri = currentProgram.getRelocationTable().getRelocations();
        while (ri.hasNext()) h.merge(ri.next().getType(), 1, Integer::sum);
        sb.append(" \"relocationTypes\": ").append(h.toString().replace("=", ":").replace("{", "{\"").replace(", ", ", \"").replace(":", "\":")).append(",\n");
        // Entry first bytes
        Address base = currentProgram.getMinAddress();
        byte[] b = new byte[16];
        currentProgram.getMemory().getBytes(base, b);
        StringBuilder hex = new StringBuilder();
        for (byte x : b) hex.append(String.format("%02x", x));
        sb.append(" \"minAddress\": \"").append(base).append("\",\n \"first16\": \"").append(hex).append("\"\n}\n");
        try (FileWriter w = new FileWriter(out)) { w.write(sb.toString()); }
        println("wrote " + out);
    }
}
