//The instruction that maintains the region byte, and its function.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class RegionWriter extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/regionwriter2.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Function f = getFunctionContaining(toAddr(base + 0x417B39L));
        if (f == null) { out.println("no function at exe+0x417B39"); out.close(); return; }
        long entry = f.getEntryPoint().getOffset()-base, size = f.getBody().getNumAddresses();
        out.println("#### function exe+0x" + Long.toHexString(entry) + " size=" + size);
        DecompileResults r = dec.decompileFunction(f, 180, monitor);
        if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());

        out.println("--- disasm around exe+0x417B39 ---");
        Address a = toAddr(base + 0x417A60L);
        while (a.getOffset() - base < 0x417B90L) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(1); continue; }
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-7X %-24s %s", a.getOffset()-base, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }
        out.close();
        println("written regionwriter");
    }
}
