//Disassemble the top of the bonfire activation routine (refusal branch).
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class DecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/resttop.txt");
        Listing lst = currentProgram.getListing();
        Address a = toAddr(base + 0x1CB950L);
        for (int i = 0; i < 120; i++) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(1); continue; }
            long rva = a.getOffset() - base;
            if (rva > 0x1CBA10L) break;
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-7X %-24s %s", rva, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }
        out.close();
        println("written resttop");
    }
}
