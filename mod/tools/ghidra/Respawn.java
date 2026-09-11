//The per-map work done on a rest, and how the bonfire menu state is reached.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class Respawn extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/respawn.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] rvas = { 0x44F7F0L, 0x3C12B0L, 0x3C1B40L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  none"); continue; }
            out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.println("");
        out.println("=== skip ===");
        Function t = getFunctionContaining(toAddr(base + 0x44F7F0L));
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(t.getEntryPoint());
        Set<Long> seen = new LinkedHashSet<>();
        while (ri.hasNext()) {
            Reference rf = ri.next();
            Function f = getFunctionContaining(rf.getFromAddress());
            if (f != null) seen.add(f.getEntryPoint().getOffset()-base);
        }
        for (long fr : seen) {
            out.println("");
            out.println("---- caller exe+0x" + Long.toHexString(fr));
            Function f = getFunctionContaining(toAddr(base + fr));
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written respawn");
    }
}
