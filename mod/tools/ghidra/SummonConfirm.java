//What exe+0x210B00 does after validating: the menu it opens, and the commit path.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SummonConfirm extends GhidraScript {
    long base; DecompInterface dec; PrintWriter out;

    void dump(long rva, String why) throws Exception {
        Function f = getFunctionContaining(toAddr(base + rva));
        out.println("");
        out.println("################ exe+0x" + Long.toHexString(rva) + "  (" + why + ")");
        if (f == null) { out.println("  none"); return; }
        out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                    + " size=" + f.getBody().getNumAddresses());
        if (f.getBody().getNumAddresses() > 3500) { out.println("  too big"); return; }
        DecompileResults r = dec.decompileFunction(f, 180, monitor);
        if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        Set<Long> callers = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) { Function c = getFunctionContaining(ri.next().getFromAddress()); if (c != null) callers.add(c.getEntryPoint().getOffset()-base); }
        out.print("callers (" + callers.size() + "):");
        for (long c : callers) out.print(" exe+0x" + Long.toHexString(c));
        out.println();
    }

    @Override
    public void run() throws Exception {
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter("E:/ghidra/summonconfirm.txt");
        dec = new DecompInterface(); dec.openProgram(currentProgram);

        dump(0x2115B0L, "called by 210B00 after the id checks out - opens the confirmation?");
        dump(0x20A270L, "also called by 210B00: [mgr+0x70], id & 0xF");
        // Everything else the summon manager owns sits in this band; the commit
        // ("yes" pressed) should be here and should read the same pending id.
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(toAddr(base + 0x210000L), true);
        out.println("");
        out.println("=== functions in exe+0x210000..0x212000 ===");
        while (it.hasNext()) {
            Function f = it.next();
            long rva = f.getEntryPoint().getOffset() - base;
            if (rva >= 0x212000L) break;
            int callers = 0;
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (ri.hasNext()) { ri.next(); callers++; }
            out.println(String.format("  exe+0x%-7X size=%-5d refs=%d", rva, f.getBody().getNumAddresses(), callers));
        }
        out.close();
        println("written summonconfirm");
    }
}
