//Decompile the second session-state veto site and its neighbourhood.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class RestExec extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/restexec.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();
        long[] rvas = { 0x199C2EL, 0xA4F10L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ call site exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  no function"); continue; }
            long entry = f.getEntryPoint().getOffset() - base;
            long end = entry + f.getBody().getNumAddresses();
            out.println("entry exe+0x" + Long.toHexString(entry) + " size=" + (end-entry));
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            out.println("--- disasm around the call ---");
            Address a = toAddr(base + rva - 0x40);
            while (a.getOffset() - base < rva + 0x50) {
                Instruction ins = lst.getInstructionAt(a);
                if (ins == null) { a = a.add(1); continue; }
                StringBuilder b = new StringBuilder();
                for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
                out.println(String.format("exe+0x%-7X %-24s %s", a.getOffset()-base, b.toString(), ins.toString()));
                a = a.add(ins.getLength());
            }
        }
        out.close();
        println("written restexec");
    }
}
