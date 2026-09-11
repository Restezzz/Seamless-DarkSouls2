//Every RTTI-named class with "Summon" in it, with its vtable and first slots.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SummonClasses extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/summonclasses.txt");
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        int n = 0;
        while (it.hasNext() && n < 80) {
            Symbol s = it.next();
            if (!s.getName().startsWith("vftable")) continue;
            Namespace ns = s.getParentNamespace();
            String cls = ns == null ? "" : ns.getName(true);
            String low = cls.toLowerCase();
            if (!low.contains("summon") && !low.contains("signmenu") && !low.contains("signdialog")) continue;
            n++;
            long vt = s.getAddress().getOffset() - base;
            StringBuilder sb = new StringBuilder();
            for (int k = 0; k < 24; k++) {
                long v;
                try { v = getLong(toAddr(base + vt + k * 8)) - base; } catch (Exception e) { break; }
                if (v <= 0 || v >= 0xE00000L) break;
                sb.append(String.format(" [%02X]%X", k * 8, v));
            }
            out.println(String.format("%-60s vtable exe+0x%-8X%s", cls, vt, sb.toString()));
        }
        out.println("classes found: " + n);
        out.close();
        println("written summonclasses");
    }
}
