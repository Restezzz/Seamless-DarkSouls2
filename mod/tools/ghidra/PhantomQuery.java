//Who reads the MapGeneralPhantomLocation class descriptor - the region query.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class PhantomQuery extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/phantomquery.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Set<Long> fns = new LinkedHashSet<>();
        for (long off : new long[]{ 0x160F668L, 0x160F670L }) {
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(toAddr(base + off));
            while (ri.hasNext()) {
                Reference rf = ri.next();
                Function f = getFunctionContaining(rf.getFromAddress());
                out.println(String.format("desc+0x%X from exe+0x%-9s in %s", off,
                        Long.toHexString(rf.getFromAddress().getOffset()-base),
                        f == null ? "?" : ("exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base))));
                if (f != null) fns.add(f.getEntryPoint().getOffset()-base);
            }
        }
        out.println("distinct: " + fns.size());
        int shown = 0;
        for (long fr : fns) {
            Function f = getFunctionContaining(toAddr(base + fr));
            if (f.getBody().getNumAddresses() > 2000 || shown >= 10) continue;
            shown++;
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(fr) + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written phantomquery");
    }
}
