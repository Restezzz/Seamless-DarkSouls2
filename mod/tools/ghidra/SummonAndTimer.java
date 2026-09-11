//Summon sender around exe+0x27ADE7, and the sign poll's timer/flag helpers.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SummonAndTimer extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/summontimer.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // 0x27ADE7: summon sender. 0x2760B0/0x276050: state flag test/set used by
        // the sign poll (flags 8/9/3). 0x2AAB90: the value compared with mgr+0x60.
        long[] rvas = { 0x27ADE7L, 0x2760B0L, 0x276050L, 0x2AAB90L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  none"); continue; }
            long entry = f.getEntryPoint().getOffset()-base;
            out.println("entry exe+0x" + Long.toHexString(entry) + " size=" + f.getBody().getNumAddresses());
            if (f.getBody().getNumAddresses() <= 5000) {
                DecompileResults r = dec.decompileFunction(f, 180, monitor);
                if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            } else out.println("  too big, skipped");
            Set<Long> callers = new LinkedHashSet<>();
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (ri.hasNext()) {
                Function c = getFunctionContaining(ri.next().getFromAddress());
                if (c != null) callers.add(c.getEntryPoint().getOffset()-base);
            }
            out.println("callers (" + callers.size() + "):");
            int n = 0;
            for (long c : callers) { if (n++ >= 12) { out.println("   ..."); break; } out.println("   exe+0x" + Long.toHexString(c)); }
        }
        out.close();
        println("written summontimer");
    }
}
