//The per-sign-type availability predicate and who consults it.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;
import java.util.*;

public class SignUsable extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/signusable.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Function f = getFunctionContaining(toAddr(base + 0x275EA0L));
        if (f == null) { out.println("no function at exe+0x275EA0"); out.close(); return; }
        long entry = f.getEntryPoint().getOffset()-base, size = f.getBody().getNumAddresses();
        out.println("#### exe+0x" + Long.toHexString(entry) + " size=" + size);
        DecompileResults r = dec.decompileFunction(f, 180, monitor);
        if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());

        out.println("--- disasm ---");
        Address a = toAddr(base + entry);
        while (a.getOffset() - base < entry + size) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(1); continue; }
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-7X %-24s %s", a.getOffset()-base, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }

        out.println("");
        out.println("=== callers ===");
        Set<Long> fns = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) {
            Reference rf = ri.next();
            Function c = getFunctionContaining(rf.getFromAddress());
            if (c != null) fns.add(c.getEntryPoint().getOffset()-base);
        }
        out.println("distinct callers: " + fns.size());
        for (long fr : fns) out.println("  exe+0x" + Long.toHexString(fr));
        out.close();
        println("written signusable");
    }
}
