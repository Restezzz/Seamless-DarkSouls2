//Find the class of the sign-list task: vtables referenced by the request builder.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;
import java.util.*;

public class Task8Class extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/task8.txt");
        Listing lst = currentProgram.getListing();

        // Functions that build the sign-list request, and the poll that calls it.
        long[] owners = { 0x2A3530L, 0x2A22A0L, 0x2A1D50L };
        for (long o : owners) {
            Function f = getFunctionContaining(toAddr(base + o));
            if (f == null) continue;
            out.println("");
            out.println("######## data refs from exe+0x" + Long.toHexString(o) + " into .rdata that look like vtables");
            Set<Long> seen = new TreeSet<>();
            InstructionIterator ii = lst.getInstructions(f.getBody(), true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                for (Reference r : ins.getReferencesFrom()) {
                    long t = r.getToAddress().getOffset() - base;
                    if (t < 0x1000000L || t > 0x1200000L) continue;     // .rdata range seen so far
                    if (!seen.add(t)) continue;
                    // A vtable's first slot points back into code.
                    long slot0 = 0;
                    try { slot0 = getLong(toAddr(base + t)) - base; } catch (Exception e) { continue; }
                    if (slot0 <= 0 || slot0 >= 0xE00000L) continue;
                    StringBuilder sb = new StringBuilder();
                    for (int k = 0; k < 20; k++) {
                        long v = 0;
                        try { v = getLong(toAddr(base + t + k * 8)) - base; } catch (Exception e) { break; }
                        if (v <= 0 || v >= 0xE00000L) break;
                        sb.append(String.format(" [%02X]exe+0x%X", k * 8, v));
                    }
                    Symbol sym = getSymbolAt(toAddr(base + t));
                    out.println(String.format("  at exe+0x%-8X from exe+0x%-8X %s%s",
                        t, ins.getAddress().getOffset() - base,
                        sym == null ? "" : ("<" + sym.getName() + "> "), sb.toString()));
                }
            }
        }
        out.close();
        println("written task8");
    }
}
