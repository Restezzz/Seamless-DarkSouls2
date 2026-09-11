//Dump ChrEventKindleBonfireActionCtrl vftable + decompile its methods.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.io.PrintWriter;
import java.util.*;

public class FindBonfire extends GhidraScript {

    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/bonfire.txt");

        List<Symbol> targets = new ArrayList<>();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName(true);
            if (n.contains("KindleBonfire") || n.contains("EventBonfireManager")
                || n.contains("MapObjBonfireComponent")) {
                out.println(String.format("SYM exe+0x%X  %s", s.getAddress().getOffset() - base, n));
                if (n.contains("vftable") && !n.contains("meta_ptr")) targets.add(s);
            }
        }

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Set<Long> done = new HashSet<>();
        for (Symbol vt : targets) {
            out.println("");
            out.println("======== " + vt.getName(true) + " @ exe+0x" + Long.toHexString(vt.getAddress().getOffset() - base));
            Address a = vt.getAddress();
            for (int i = 0; i < 24; i++) {
                long p;
                try { p = currentProgram.getMemory().getLong(a.add((long) i * 8)); } catch (Exception e) { break; }
                if (p < base || p > base + 0x2000000L) break;

                Function f = getFunctionAt(toAddr(p));
                out.println(String.format("  [%2d] exe+0x%-8X %s", i, p - base, f != null ? f.getName() : "(unnamed)"));

                if (f != null && done.add(p)) {
                    DecompileResults r = dec.decompileFunction(f, 60, monitor);
                    if (r != null && r.decompileCompleted()) {
                        out.println("  ---- decompiled exe+0x" + Long.toHexString(p - base) + " ----");
                        out.println(r.getDecompiledFunction().getC());
                    }
                }
            }
        }
        out.close();
        println("written E:/ghidra/bonfire.txt");
    }
}
