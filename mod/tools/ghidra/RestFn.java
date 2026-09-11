//Decompile the function writing LastSetBonfire and its callers.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class RestFn extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/rest.txt");

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Function target = getFunctionContaining(toAddr(base + 0x44FEB4L));
        if (target == null) { out.println("no function at exe+0x44FEB4"); out.close(); return; }

        out.println("TARGET exe+0x" + Long.toHexString(target.getEntryPoint().getOffset() - base)
                    + "  size=" + target.getBody().getNumAddresses() + "  " + target.getName());
        DecompileResults r = dec.decompileFunction(target, 120, monitor);
        if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());

        // Callers
        out.println("");
        out.println("=================== CALLERS ===================");
        Set<Function> callers = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(target.getEntryPoint());
        while (ri.hasNext()) {
            Function f = getFunctionContaining(ri.next().getFromAddress());
            if (f != null) callers.add(f);
        }
        out.println("callers: " + callers.size());
        int n = 0;
        for (Function f : callers) {
            if (n++ >= 4) break;
            out.println("");
            out.println("--- CALLER exe+0x" + Long.toHexString(f.getEntryPoint().getOffset() - base)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults cr = dec.decompileFunction(f, 120, monitor);
            if (cr != null && cr.decompileCompleted()) out.println(cr.getDecompiledFunction().getC());
        }
        out.close();
        println("written rest");
    }
}
