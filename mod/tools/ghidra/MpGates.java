//Among the session gates, find any that mention the pickup action id 0x1F.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class MpGates extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/pickup.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Function pred = getFunctionContaining(toAddr(base + 0x5135F0L));
        Set<Function> gates = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(pred.getEntryPoint());
        while (ri.hasNext()) {
            Function f = getFunctionContaining(ri.next().getFromAddress());
            if (f != null) gates.add(f);
        }

        out.println("scanning " + gates.size() + " session gates for 0x1f / 31");
        for (Function f : gates) {
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            if (r == null || !r.decompileCompleted()) continue;
            String c = r.getDecompiledFunction().getC();
            if (c.contains("0x1f") || c.contains("0x1F") || c.contains(" 31)") || c.contains("== 31")) {
                out.println("");
                out.println("######### exe+0x" + Long.toHexString(f.getEntryPoint().getOffset() - base)
                            + " size=" + f.getBody().getNumAddresses());
                out.println(c);
            }
        }
        out.close();
        println("written pickup");
    }
}
