//Three levels of callers above the dispatch site.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class Callers extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/callers.txt");

        Set<Function> cur = new LinkedHashSet<>();
        Function start = getFunctionContaining(toAddr(base + 0x1402C0L));
        cur.add(start);

        for (int level = 1; level <= 3; level++) {
            Set<Function> next = new LinkedHashSet<>();
            for (Function f : cur) next.addAll(callersOf(f));
            out.println("=== level " + level + " (" + next.size() + " functions) ===");
            for (Function f : next) {
                out.println(String.format("  exe+0x%-8X size=%-5d %s",
                    f.getEntryPoint().getOffset() - base, f.getBody().getNumAddresses(), f.getName()));
            }
            if (next.isEmpty()) break;
            cur = next;
        }
        out.close();
        println("written callers");
    }

    private Set<Function> callersOf(Function t) throws Exception {
        Set<Function> res = new LinkedHashSet<>();
        if (t == null) return res;
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(t.getEntryPoint());
        while (ri.hasNext()) {
            Function f = getFunctionContaining(ri.next().getFromAddress());
            if (f != null) res.add(f);
        }
        return res;
    }
}
