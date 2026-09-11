//Decompile the action dispatch chain around the rest action.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class ActDisp extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/actdisp.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] rvas = { 0x32A330L, 0x451E50L, 0x452790L, 0x329C20L, 0x198920L, 0x19AC90L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ probe exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  no function"); continue; }
            out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            else out.println("  decompile failed");
        }
        out.close();
        println("written actdisp");
    }
}
