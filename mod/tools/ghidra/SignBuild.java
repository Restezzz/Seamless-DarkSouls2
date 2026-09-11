//The function that builds sign data and decides the resulting type.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class SignBuild extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/signbuild.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();
        long[] rvas = { 0x29C9B0L, 0x2A2780L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  none"); continue; }
            long entry = f.getEntryPoint().getOffset()-base, size = f.getBody().getNumAddresses();
            out.println("entry exe+0x" + Long.toHexString(entry) + " size=" + size);
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            if (size < 400) {
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
            }
        }
        out.close();
        println("written signbuild");
    }
}
