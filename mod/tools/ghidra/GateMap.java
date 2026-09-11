//Label every session gate with the class whose vftable references it.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class GateMap extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/gatemap.txt");

        Function pred = getFunctionContaining(toAddr(base + 0x5135F0L));
        Set<Function> gates = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(pred.getEntryPoint());
        while (ri.hasNext()) {
            Function f = getFunctionContaining(ri.next().getFromAddress());
            if (f != null) gates.add(f);
        }

        for (Function f : gates) {
            long rva = f.getEntryPoint().getOffset() - base;
            Set<String> owners = new LinkedHashSet<>();

            // Any data reference to this function is almost certainly a vftable
            // entry; the nearest symbol above it names the class.
            ReferenceIterator r2 = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (r2.hasNext()) {
                Reference r = r2.next();
                if (getFunctionContaining(r.getFromAddress()) != null) continue;  // code ref, skip
                Address probe = r.getFromAddress();
                for (int back = 0; back < 64 && owners.size() < 4; back++) {
                    Symbol[] syms = currentProgram.getSymbolTable().getSymbols(probe);
                    if (syms.length > 0) {
                        for (Symbol s : syms) {
                            String n = s.getName(true);
                            if (n.contains("vftable") || n.contains("::")) { owners.add(n); break; }
                        }
                        if (!owners.isEmpty()) break;
                    }
                    if (probe.getOffset() - base < 8) break;
                    probe = probe.subtract(8);
                }
            }

            out.println(String.format("exe+0x%-8X size=%-6d %s", rva, f.getBody().getNumAddresses(),
                owners.isEmpty() ? "(no class)" : String.join(" | ", owners)));
        }
        out.close();
        println("written gatemap");
    }
}
