//Who consults MapGeneralPhantomLocation, and the region-query path.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class PhantomLoc extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/phantomloc.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        long[] strs = { 0x10C8DB0L, 0x10C8DD0L };
        Set<Long> fns = new LinkedHashSet<>();
        for (long s : strs) {
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(toAddr(base + s));
            while (ri.hasNext()) {
                Reference rf = ri.next();
                Function f = getFunctionContaining(rf.getFromAddress());
                out.println(String.format("str 0x%X from exe+0x%-9s in %s", s,
                        Long.toHexString(rf.getFromAddress().getOffset()-base),
                        f == null ? "?" : ("exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base))));
                if (f != null) fns.add(f.getEntryPoint().getOffset()-base);
            }
        }
        for (long fr : fns) {
            Function f = getFunctionContaining(toAddr(base + fr));
            if (f.getBody().getNumAddresses() > 3000) continue;
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(fr) + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written phantomloc");
    }
}
