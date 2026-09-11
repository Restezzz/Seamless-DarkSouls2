//How a MapGeneral* region class is registered, and what index it gets.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class RegType extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/regtype.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] rvas = { 0x83F070L, 0x1EEA80L, 0x1EE610L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("---- exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  none"); continue; }
            out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written regtype");
    }
}
