//Decompile the functions the bonfire menu drives on each step.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class RestCommit extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/restcommit.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] rvas = { 0x17F540L, 0x17FC40L, 0x17FFA0L, 0x17F310L, 0x17F980L, 0x17F420L, 0x17FD70L, 0x17F7C0L };
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
        println("written restcommit");
    }
}
