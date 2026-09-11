//Dump the player-kind capability table and the pickup gate disassembly.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class KindTable extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/kindtable.txt");

        out.println("=== table exe+0x10BFFF0, 5-byte records ===");
        for (int i = 0; i < 24; i++) {
            Address a = toAddr(base + 0x10BFFF0L + (long)i * 5);
            StringBuilder b = new StringBuilder();
            for (int j = 0; j < 5; j++) b.append(String.format("%02X ", getByte(a.add(j))));
            out.println(String.format("kind %2d (0x%02X): %s", i, i, b.toString()));
        }

        out.println("");
        out.println("=== disasm exe+0x4528F0 .. 0x452980 ===");
        Listing lst = currentProgram.getListing();
        Address a = toAddr(base + 0x4528F0L);
        while (a.getOffset() - base < 0x452980L) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(1); continue; }
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-7X %-26s %s", a.getOffset()-base, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }

        out.println("");
        out.println("=== who reads exe+0x10BFFF0 .. +0x10C0060 ===");
        Set<String> seen = new LinkedHashSet<>();
        for (long off = 0; off < 0x70; off++) {
            Address t = toAddr(base + 0x10BFFF0L + off);
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(t);
            while (ri.hasNext()) {
                Reference rf = ri.next();
                Function f = getFunctionContaining(rf.getFromAddress());
                String s = String.format("  +0x%02X  from exe+0x%-8s in %s", off,
                        Long.toHexString(rf.getFromAddress().getOffset()-base),
                        f == null ? "?" : ("exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)));
                if (seen.add(s)) out.println(s);
            }
        }
        out.println("total distinct refs: " + seen.size());
        out.close();
        println("written kindtable");
    }
}
