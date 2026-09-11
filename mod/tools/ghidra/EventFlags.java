//Functions in the EventFlagManager method cluster that do flag bit math.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class EventFlags extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/flagfuncs.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            long rva = f.getEntryPoint().getOffset() - base;
            if (rva < 0x473000L || rva > 0x476000L) continue;
            long sz = f.getBody().getNumAddresses();
            if (sz > 0x220) continue;

            DecompileResults r = dec.decompileFunction(f, 60, monitor);
            if (r == null || !r.decompileCompleted()) continue;
            String c = r.getDecompiledFunction().getC();
            boolean bitmath = (c.contains(">> 3") || c.contains(">> 5") || c.contains("& 7") || c.contains("& 0x1f")
                               || c.contains("1 <<") || c.contains("& 0x3f"));
            if (!bitmath) continue;

            out.println("######### exe+0x" + Long.toHexString(rva) + " size=" + sz);
            out.println(c);
        }
        out.close();
        println("written flagfuncs");
    }
}
