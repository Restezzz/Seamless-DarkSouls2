//The sign manager's periodic update - where it decides whether to ask at all.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class SignPoll extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/signpoll.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] rvas = { 0x2A1801L, 0x2A2359L, 0x2A3E08L, 0x291256L };
        for (long rva : rvas) {
            Function f = getFunctionContaining(toAddr(base + rva));
            out.println("");
            out.println("################ return address exe+0x" + Long.toHexString(rva));
            if (f == null) { out.println("  no function"); continue; }
            out.println("entry exe+0x" + Long.toHexString(f.getEntryPoint().getOffset()-base)
                        + " size=" + f.getBody().getNumAddresses());
            if (f.getBody().getNumAddresses() > 6000) { out.println("  too big, skipped"); continue; }
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written signpoll");
    }
}
