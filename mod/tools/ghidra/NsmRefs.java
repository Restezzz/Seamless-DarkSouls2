//Find small predicate functions that read the NetSessionManager global.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class NsmRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/nsm.txt");

        Address nsm = toAddr(base + 0x1616CF8L);
        Set<Function> funcs = new LinkedHashSet<>();

        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(nsm);
        int refs = 0;
        while (ri.hasNext()) {
            Reference r = ri.next();
            refs++;
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) funcs.add(f);
        }
        out.println("refs to NSM global: " + refs + ", in " + funcs.size() + " functions");
        out.println("");

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        for (Function f : funcs) {
            long size = f.getBody().getNumAddresses();
            long rva = f.getEntryPoint().getOffset() - base;
            out.println(String.format("FUNC exe+0x%-8X size=%-5d %s", rva, size, f.getName()));
            if (size <= 0xA0) {
                DecompileResults r = dec.decompileFunction(f, 60, monitor);
                if (r != null && r.decompileCompleted()) {
                    out.println(r.getDecompiledFunction().getC());
                }
            }
        }
        out.close();
        println("written nsm");
    }
}
