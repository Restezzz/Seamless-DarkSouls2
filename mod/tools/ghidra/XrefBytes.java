//Count xrefs to a helper and dump bytes/disasm of the gate.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;

public class XrefBytes extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/gate.txt");

        Address helper = toAddr(base + 0x1D7130L);
        int n = 0;
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(helper);
        while (ri.hasNext()) { Reference r = ri.next(); n++;
            if (n <= 25) out.println("  xref from exe+0x" + Long.toHexString(r.getFromAddress().getOffset() - base)); }
        out.println("XREFS to exe+0x1D7130 (helper): " + n);

        out.println("");
        out.println("=== disasm exe+0x3F2500 (gate) ===");
        Listing lst = currentProgram.getListing();
        Address a = toAddr(base + 0x3F2500L);
        for (int i = 0; i < 40; i++) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) break;
            StringBuilder b = new StringBuilder();
            for (byte by : ins.getBytes()) b.append(String.format("%02X ", by));
            out.println(String.format("exe+0x%-8X %-24s %s", a.getOffset() - base, b.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }
        out.close();
        println("written gate");
    }
}
