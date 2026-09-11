//Decompile the code that leaves the bonfire rest state.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class StandUp extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/standup.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Function f = getFunctionContaining(toAddr(base + 0x17EEB3L));
        out.println("################ function containing exe+0x17EEB3");
        if (f != null) {
            long entry = f.getEntryPoint().getOffset() - base;
            out.println("entry exe+0x" + Long.toHexString(entry) + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        } else out.println("  none");

        out.println("");
        out.println("=== disasm exe+0x17EE20 .. 0x17EEE0 ===");
        Address a = toAddr(base + 0x17EE20L);
        while (a.getOffset() - base < 0x17EEE0L) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(1); continue; }
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-7X %-24s %s", a.getOffset()-base, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }
        out.close();
        println("written standup");
    }
}
