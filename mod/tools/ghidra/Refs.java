//Args: <output file> <symbol-name substring> ...
//Every symbol whose name contains a substring (imports included), the places
//that reference it, and for data references (import slots) the places that
//reference those in turn -- i.e. the real call sites of an imported function.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;

public class Refs extends GhidraScript {
    long base;
    PrintWriter out;

    String where(Reference r) {
        Function f = getFunctionContaining(r.getFromAddress());
        return String.format("exe+0x%X (%s) in %s", r.getFromAddress().getOffset() - base,
                r.getReferenceType(), f == null ? "-" : String.format("exe+0x%X", f.getEntryPoint().getOffset() - base));
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter(a[0]);
        SymbolTable table = currentProgram.getSymbolTable();
        for (int i = 1; i < a.length; i++) {
            SymbolIterator it = table.getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                if (!s.getName().contains(a[i])) continue;
                out.println("#### " + s.getName(true) + " at " + s.getAddress());
                for (Reference r : getReferencesTo(s.getAddress())) {
                    out.println("  " + where(r));
                    if (r.getReferenceType().isData()) {
                        for (Reference r2 : getReferencesTo(r.getFromAddress())) out.println("      <- " + where(r2));
                    }
                }
            }
        }
        out.close();
        println("written " + a[0]);
    }
}
