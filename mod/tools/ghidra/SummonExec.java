//Action 0xC executor (summon) and what it calls; plus the task registry getter.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SummonExec extends GhidraScript {
    long base;
    DecompInterface dec;
    PrintWriter out;

    void dump(long rva, String why) throws Exception {
        Function f = getFunctionContaining(toAddr(base + rva));
        out.println("");
        out.println("################ exe+0x" + Long.toHexString(rva) + "  (" + why + ")");
        if (f == null) { out.println("  none"); return; }
        out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                    + " size=" + f.getBody().getNumAddresses());
        if (f.getBody().getNumAddresses() > 3000) { out.println("  too big"); return; }
        DecompileResults r = dec.decompileFunction(f, 180, monitor);
        if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
    }

    @Override
    public void run() throws Exception {
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter("E:/ghidra/summonexec.txt");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        dump(0x452BE0L, "action 0xC executor - summon");
        // Direct callees of the executor, decompiled too, since the summon task is
        // queued one or two levels down.
        Function f = getFunctionContaining(toAddr(base + 0x452BE0L));
        if (f != null) {
            Set<Long> callees = new LinkedHashSet<>();
            for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getEntryPoint().getOffset()-base);
            out.println("");
            out.println("=== callees of exe+0x452BE0: " + callees.size() + " ===");
            for (long c : callees) out.println("   exe+0x" + Long.toHexString(c));
            int n = 0;
            for (long c : callees) { if (n++ >= 6) break; dump(c, "callee of summon executor"); }
        }
        dump(0x28FBB0L, "task registry getter used by the sign poll flags");
        out.close();
        println("written summonexec");
    }
}
