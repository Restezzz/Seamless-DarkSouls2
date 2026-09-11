//Decompile the object-side of the pickup answer and the world/owner lookup.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class ItemPath extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/itempath.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] rvas = { 0x1E6C60L, 0x1DE2A0L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  no function"); continue; }
            out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }

        // Any named symbol that looks item-lot / treasure related.
        out.println("");
        out.println("=== symbols matching item/lot/treasure/pick ===");
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        int n = 0;
        while (it.hasNext() && n < 200) {
            Symbol s = it.next();
            String nm = s.getName();
            String low = nm.toLowerCase();
            if (low.contains("itemlot") || low.contains("treasure") || low.contains("pickup")
                || low.contains("itemgib") || low.contains("dropitem")) {
                out.println(String.format("exe+0x%-8s %s", Long.toHexString(s.getAddress().getOffset()-base), nm));
                n++;
            }
        }
        out.println("matches: " + n);
        out.close();
        println("written itempath");
    }
}
