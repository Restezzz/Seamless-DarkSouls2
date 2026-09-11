//Decompile + disassemble every availability handler that gates on table[kind*5+1].
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class KindGates extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/kindgates.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();
        long[] rvas = { 0x452FE0L, 0x453060L, 0x4516C0L, 0x4529F0L, 0x452BE0L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  no function"); continue; }
            long entry = f.getEntryPoint().getOffset() - base;
            long end = entry + f.getBody().getNumAddresses();
            out.println("entry exe+0x" + Long.toHexString(entry) + " size=" + (end-entry));
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            out.println("--- disasm ---");
            Address a = toAddr(base + entry);
            while (a.getOffset() - base < end) {
                Instruction ins = lst.getInstructionAt(a);
                if (ins == null) { a = a.add(1); continue; }
                StringBuilder b = new StringBuilder();
                for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
                out.println(String.format("exe+0x%-7X %-24s %s", a.getOffset()-base, b.toString(), ins.toString()));
                a = a.add(ins.getLength());
            }
        }
        out.close();
        println("written kindgates");
    }
}
