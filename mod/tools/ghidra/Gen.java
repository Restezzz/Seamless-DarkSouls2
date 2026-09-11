//Find the enemy generator machinery and the item-drop component descriptor users.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class Gen extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/gen.txt");

        out.println("=== symbols containing 'gener' / 'gene' ===");
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        int n = 0;
        while (it.hasNext() && n < 60) {
            Symbol s = it.next();
            String low = s.getName().toLowerCase();
            if (low.contains("gener")) {
                out.println(String.format("exe+0x%-9s %s", Long.toHexString(s.getAddress().getOffset()-base), s.getName()));
                n++;
            }
        }
        out.println("count " + n);

        out.println("");
        out.println("=== who touches the MapObjItemDropComponent descriptor exe+0x160EB68 ===");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Long> fns = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(toAddr(base + 0x160EB68L));
        while (ri.hasNext()) {
            Reference rf = ri.next();
            Function f = getFunctionContaining(rf.getFromAddress());
            out.println(String.format("  from exe+0x%-9s in %s",
                    Long.toHexString(rf.getFromAddress().getOffset()-base),
                    f == null ? "?" : ("exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base))));
            if (f != null) fns.add(f.getEntryPoint().getOffset()-base);
        }
        for (long fr : fns) {
            Function f = getFunctionContaining(toAddr(base + fr));
            if (f.getBody().getNumAddresses() > 4000) { out.println("\n---- exe+0x" + Long.toHexString(fr) + " (too big, skipped)"); continue; }
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(fr) + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written gen");
    }
}
