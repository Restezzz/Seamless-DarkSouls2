//Bonfire menu option handlers + everything that references MapObjItemDropComponent.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class Both extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/both.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        out.println("################ bonfire menu option handlers");
        long[] rvas = { 0x180220L, 0x180160L, 0x1801C0L, 0x180280L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  none"); continue; }
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }

        out.println("");
        out.println("################ refs to MapObjItemDropComponent strings");
        long[] strs = { 0x10C6610L, 0x10C6628L };
        Set<Long> fns = new LinkedHashSet<>();
        for (long s : strs) {
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(toAddr(base + s));
            while (ri.hasNext()) {
                Reference rf = ri.next();
                long from = rf.getFromAddress().getOffset() - base;
                Function f = getFunctionContaining(rf.getFromAddress());
                out.println(String.format("  str 0x%X referenced from exe+0x%-9s in %s", s,
                        Long.toHexString(from), f == null ? "?" : ("exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base))));
                if (f != null) fns.add(f.getEntryPoint().getOffset()-base);
            }
        }
        for (long fr : fns) {
            Function f = getFunctionContaining(toAddr(base + fr));
            out.println("");
            out.println("---- referencing function exe+0x" + Long.toHexString(fr)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written both");
    }
}
