//Generic dumper. Args: <output file> <rva>=<label> ...
//Decompiles the function containing each RVA and lists its callers.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class Dump extends GhidraScript {
    long base; DecompInterface dec; PrintWriter out;

    void dump(long rva, String why) throws Exception {
        Function f = getFunctionContaining(toAddr(base + rva));
        out.println();
        out.println("################ exe+0x" + Long.toHexString(rva) + "  (" + why + ")");
        if (f == null) { out.println("  no function"); return; }
        out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset() - base)
                    + " size=" + f.getBody().getNumAddresses());
        if (f.getBody().getNumAddresses() <= 4000) {
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            else out.println("  (decompile failed)");
        } else {
            out.println("  (too large to decompile)");
        }
        Set<Long> c = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) {
            Reference r = ri.next();
            Function g = getFunctionContaining(r.getFromAddress());
            if (g != null) c.add(g.getEntryPoint().getOffset() - base);
            else out.println("  data ref from exe+0x" + Long.toHexString(r.getFromAddress().getOffset() - base));
        }
        out.print("callers:");
        for (long x : c) out.print(" exe+0x" + Long.toHexString(x));
        out.println();
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter(a[0]);
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            String[] p = a[i].split("[:=]", 2);
            long rva = Long.parseLong(p[0].replace("0x", ""), 16);
            dump(rva, p.length > 1 ? p[1] : "");
        }
        out.close();
        println("written " + a[0]);
    }
}
