//Callers of the MapGeneralPhantomLocation cast - the actual region queries.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class PhantomUsers extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/phantomusers.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Function t = getFunctionContaining(toAddr(base + 0x1EE610L));
        Set<Long> fns = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(t.getEntryPoint());
        while (ri.hasNext()) {
            Reference rf = ri.next();
            Function f = getFunctionContaining(rf.getFromAddress());
            if (f != null) fns.add(f.getEntryPoint().getOffset()-base);
        }
        out.println("callers: " + fns.size());
        for (long fr : fns) {
            Function f = getFunctionContaining(toAddr(base + fr));
            out.println(String.format("  exe+0x%-9s size=%d", Long.toHexString(fr), f.getBody().getNumAddresses()));
        }
        int shown = 0;
        for (long fr : fns) {
            Function f = getFunctionContaining(toAddr(base + fr));
            if (f.getBody().getNumAddresses() > 2500 || shown >= 6) continue;
            shown++;
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(fr) + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written phantomusers");
    }
}
