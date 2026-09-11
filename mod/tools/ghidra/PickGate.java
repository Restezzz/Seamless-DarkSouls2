//Decompile the availability handlers for pickup-class actions.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class PickGate extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/pickgate.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        // 0x4528F0 = case 0x1b/0x1e/0x1f (pickup class)
        // 0x452A90 = case 0x1a/0x21/0x22 ; 0x4529F0 = the many "simple" ids
        // 0x452BE0 = case 0/0xc/0xf/0x10 ; 0x452680/0x4526F0/0x452720 = doors etc.
        long[] rvas = { 0x4528F0L, 0x4529F0L, 0x452A90L, 0x452BE0L, 0x452590L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  no function"); continue; }
            out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                        + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            else out.println("  decompile failed");
        }
        out.close();
        println("written pickgate");
    }
}
