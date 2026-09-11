//Find the code that sends RequestSummonSign.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;
import java.util.*;

public class SummonFn extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/summon.txt");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // The sign manager's own functions live around exe+0x2A1000-0x2A5000.
        // exe+0x2A1410 places a sign; the summon counterpart should sit nearby
        // and, like it, be reached from the interaction code.
        out.println("=== functions in exe+0x2A1000..0x2A5000 ===");
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        List<Function> nearby = new ArrayList<>();
        while (it.hasNext()) {
            Function f = it.next();
            long rva = f.getEntryPoint().getOffset() - base;
            if (rva >= 0x2A1000L && rva < 0x2A5000L) nearby.add(f);
        }
        for (Function f : nearby) {
            long rva = f.getEntryPoint().getOffset() - base;
            Set<Long> callers = new LinkedHashSet<>();
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (ri.hasNext()) {
                Function c = getFunctionContaining(ri.next().getFromAddress());
                if (c != null) callers.add(c.getEntryPoint().getOffset()-base);
            }
            out.println(String.format("exe+0x%-8s size=%-6d callers=%d",
                    Long.toHexString(rva), f.getBody().getNumAddresses(), callers.size()));
        }
        out.close();
        println("written summon");
    }
}
