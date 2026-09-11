//One level above the summon-job starter, and the summon job's request builder.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SummonStart extends GhidraScript {
    long base; DecompInterface dec; PrintWriter out;

    void dump(long rva, String why) throws Exception {
        Function f = getFunctionContaining(toAddr(base + rva));
        out.println("");
        out.println("################ exe+0x" + Long.toHexString(rva) + "  (" + why + ")");
        if (f == null) { out.println("  none"); return; }
        out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base) + " size=" + f.getBody().getNumAddresses());
        if (f.getBody().getNumAddresses() <= 3000) {
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        Set<Long> c = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) {
            Reference r = ri.next();
            Function g = getFunctionContaining(r.getFromAddress());
            if (g != null) c.add(g.getEntryPoint().getOffset()-base);
            else out.println("  data ref from exe+0x" + Long.toHexString(r.getFromAddress().getOffset()-base) + " (vtable?)");
        }
        out.print("callers:");
        for (long x : c) out.print(" exe+0x" + Long.toHexString(x));
        out.println();
    }

    @Override
    public void run() throws Exception {
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter("E:/ghidra/summonstart.txt");
        dec = new DecompInterface(); dec.openProgram(currentProgram);
        dump(0x2A41B0L, "calls exe+0x2A2CA0, the summon-job starter");
        out.close();
        println("written summonstart");
    }
}
