//The timed-flag registry behind the sign poll: test, set, and any clear sibling.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class FlagRegistry extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/flagregistry.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // Everything in the neighbourhood of the set (0x2854D0) and test (0x285530)
        // helpers: a clear, a reset or an "expire now" is very likely right beside them.
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(toAddr(base + 0x285380L), true);
        while (it.hasNext()) {
            Function f = it.next();
            long rva = f.getEntryPoint().getOffset() - base;
            if (rva >= 0x285700L) break;
            out.println("");
            out.println("################ exe+0x" + Long.toHexString(rva) + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        }
        out.close();
        println("written flagregistry");
    }
}
