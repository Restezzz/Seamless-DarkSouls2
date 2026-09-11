//Full decompile of the bonfire "may rest" routine + tail disassembly.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class RestFull extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/restfull.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Function f = getFunctionContaining(toAddr(base + 0x1CB950L));
        if (f == null) { out.println("no fn"); out.close(); return; }
        out.println("FN exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                    + " size=" + f.getBody().getNumAddresses());
        DecompileResults r = dec.decompileFunction(f, 180, monitor);
        if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());

        out.println("");
        out.println("================ DISASM 0x1CBA05 .. 0x1CBB60 ================");
        Listing lst = currentProgram.getListing();
        Address a = toAddr(base + 0x1CBA05L);
        while (a.getOffset() - base < 0x1CBB60L) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(1); continue; }
            long rva = a.getOffset() - base;
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-7X %-26s %s", rva, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }
        out.close();
        println("written restfull");
    }
}
