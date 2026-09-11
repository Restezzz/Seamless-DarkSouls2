//Args: <output file> <call offset hex, e.g. 0xa0> <context marker, e.g. "+ 0x40]">
//Every indirect call through [reg + offset] whose preceding instructions contain the
//marker, with the ten instructions before it, so constant arguments can be read off.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class Scan extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter(a[0]);
        String call = "+ " + a[1].toLowerCase() + "]";
        String marker = a.length > 2 ? a[2] : "";
        Listing lst = currentProgram.getListing();
        InstructionIterator it = lst.getInstructions(true);
        ArrayDeque<Instruction> window = new ArrayDeque<>();
        int hits = 0;
        while (it.hasNext() && hits < 400) {
            Instruction ins = it.next();
            String t = ins.toString();
            if (t.startsWith("CALL qword ptr [R") && t.endsWith(call)) {
                boolean ok = marker.isEmpty();
                for (Instruction w : window) if (w.toString().contains(marker)) ok = true;
                if (ok) {
                    hits++;
                    Function f = getFunctionContaining(ins.getAddress());
                    long fe = f == null ? 0 : f.getEntryPoint().getOffset() - base;
                    out.println(String.format("=== call at exe+0x%X in exe+0x%X", ins.getAddress().getOffset() - base, fe));
                    for (Instruction w : window)
                        out.println(String.format("   exe+0x%-7X %s", w.getAddress().getOffset() - base, w.toString()));
                    out.println(String.format("   exe+0x%-7X %s", ins.getAddress().getOffset() - base, t));
                }
            }
            window.addLast(ins);
            if (window.size() > 10) window.removeFirst();
        }
        out.println("hits: " + hits);
        out.close();
        println("written " + a[0]);
    }
}
