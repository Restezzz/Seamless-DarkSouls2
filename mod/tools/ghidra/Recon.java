//List MapObj component classes and respawn-related symbols/strings.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.PrintWriter;
import java.util.*;

public class Recon extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/recon.txt");

        String[] want = { "mapobj", "respawn", "revival", "reviv", "chrreset", "enemyreset",
                          "bonfire", "itemgib", "itembox", "eventflag" };
        out.println("=== symbols ===");
        Map<String, TreeMap<Long,String>> buckets = new LinkedHashMap<>();
        for (String w : want) buckets.put(w, new TreeMap<>());
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String low = s.getName().toLowerCase();
            for (String w : want) {
                if (low.contains(w) && buckets.get(w).size() < 40) {
                    buckets.get(w).put(s.getAddress().getOffset()-base, s.getName());
                }
            }
        }
        for (String w : want) {
            out.println("");
            out.println("--- " + w + " (" + buckets.get(w).size() + ") ---");
            for (Map.Entry<Long,String> e : buckets.get(w).entrySet())
                out.println(String.format("exe+0x%-9s %s", Long.toHexString(e.getKey()), e.getValue()));
        }
        out.close();
        println("written recon");
    }
}
