//Who constructs NetSvrSummonSignSummonJob (the job "yes" starts), and its +0x50.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SummonJobCtor extends GhidraScript {
    long base; DecompInterface dec; PrintWriter out;

    Set<Long> callersOf(Function f) {
        Set<Long> c = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) { Function g = getFunctionContaining(ri.next().getFromAddress()); if (g != null) c.add(g.getEntryPoint().getOffset()-base); }
        return c;
    }

    void dump(long rva, String why) throws Exception {
        Function f = getFunctionContaining(toAddr(base + rva));
        out.println("");
        out.println("################ exe+0x" + Long.toHexString(rva) + "  (" + why + ")");
        if (f == null) { out.println("  none"); return; }
        long e = f.getEntryPoint().getOffset()-base;
        out.println("entry exe+0x" + Long.toHexString(e) + " size=" + f.getBody().getNumAddresses());
        if (f.getBody().getNumAddresses() <= 3000) {
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        } else out.println("  too big to decompile");
        out.print("callers:");
        for (long c : callersOf(f)) out.print(" exe+0x" + Long.toHexString(c));
        out.println();
    }

    @Override
    public void run() throws Exception {
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter("E:/ghidra/summonjob.txt");
        dec = new DecompInterface(); dec.openProgram(currentProgram);

        // A constructor stores the vtable pointer, so references to it are the ctor(s).
        long[] vtables = { 0x10D63A8L, 0x10D6448L };
        Set<Long> ctors = new LinkedHashSet<>();
        for (long vt : vtables) {
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(toAddr(base + vt));
            while (ri.hasNext()) {
                Reference r = ri.next();
                Function f = getFunctionContaining(r.getFromAddress());
                out.println(String.format("vtable exe+0x%X referenced from exe+0x%X in %s", vt,
                        r.getFromAddress().getOffset()-base,
                        f == null ? "?" : ("exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base))));
                if (f != null) ctors.add(f.getEntryPoint().getOffset()-base);
            }
        }
        for (long c : ctors) {
            dump(c, "stores the summon job vtable");
            Function f = getFunctionContaining(toAddr(base + c));
            int n = 0;
            for (long up : callersOf(f)) { if (n++ >= 3) break; dump(up, "calls the summon job constructor"); }
        }
        dump(0x2A5B40L, "NetSvrSummonSignSummonJob vtable+0x50 - builds the network request");
        out.close();
        println("written summonjob");
    }
}
