//The sign-manager function that creates a sign, and how it is reached.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class CreateSignFn extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/createsign3.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        long[] rvas = { 0x28FAB0L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ via exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  none"); continue; }
            long entry = f.getEntryPoint().getOffset()-base;
            out.println("entry exe+0x" + Long.toHexString(entry) + " size=" + f.getBody().getNumAddresses());
            if (f.getBody().getNumAddresses() > 4000) { out.println("  too big, decompile skipped"); }
            else {
                DecompileResults r = dec.decompileFunction(f, 180, monitor);
                if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            }
            Set<Long> callers = new LinkedHashSet<>();
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (ri.hasNext()) {
                Function c = getFunctionContaining(ri.next().getFromAddress());
                if (c != null) callers.add(c.getEntryPoint().getOffset()-base);
            }
            out.println("callers: " + callers.size());
            for (long c : callers) out.println("   exe+0x" + Long.toHexString(c));
        }
        out.close();
        println("written createsign");
    }
}
