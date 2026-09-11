//Args: <output file> <class-name substring or vt:0xRVA> ...
//For every RTTI class whose name contains a substring: vtable, first slots, and
//the functions that reference the vtable (constructors / destructors).
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class Classes extends GhidraScript {
    long base; PrintWriter out;

    void describe(String cls, long vt) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (int k = 0; k < 32; k++) {
            long v;
            try { v = getLong(toAddr(base + vt + k * 8)) - base; } catch (Exception e) { break; }
            if (v <= 0 || v >= 0xE00000L) break;
            sb.append(String.format(" [%02X]%X", k * 8, v));
        }
        out.println(String.format("%s  vtable exe+0x%X", cls, vt));
        out.println("   slots:" + sb);
        Set<Long> refs = new LinkedHashSet<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(toAddr(base + vt));
        while (ri.hasNext()) {
            Reference r = ri.next();
            Function g = getFunctionContaining(r.getFromAddress());
            if (g != null) refs.add(g.getEntryPoint().getOffset() - base);
        }
        out.print("   referenced by:");
        for (long x : refs) out.print(" exe+0x" + Long.toHexString(x));
        out.println();
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        base = currentProgram.getImageBase().getOffset();
        out = new PrintWriter(a[0]);
        List<String> names = new ArrayList<>();
        List<Long> inside = new ArrayList<>();
        for (int i = 1; i < a.length; i++) {
            if (a[i].startsWith("vt:")) describe("raw", Long.parseLong(a[i].substring(3).replace("0x", ""), 16));
            else if (a[i].startsWith("in:")) inside.add(Long.parseLong(a[i].substring(3).replace("0x", ""), 16));
            else names.add(a[i].toLowerCase());
        }
        // Nearest vftable symbol at or below each "in:" address, i.e. the class whose
        // vtable holds that slot. Its slots are decompiled so the methods can be read.
        TreeMap<Long, String> tables = new TreeMap<>();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            if (!s.getName().startsWith("vftable")) continue;
            Namespace ns = s.getParentNamespace();
            String cls = ns == null ? "" : ns.getName(true);
            long vt = s.getAddress().getOffset() - base;
            tables.put(vt, cls);
            String low = cls.toLowerCase();
            for (String n : names) {
                if (low.contains(n)) { describe(cls, vt); break; }
            }
        }
        ghidra.app.decompiler.DecompInterface dec = new ghidra.app.decompiler.DecompInterface();
        dec.openProgram(currentProgram);
        for (long want : inside) {
            Map.Entry<Long, String> e = tables.floorEntry(want);
            if (e == null) { out.println("in:0x" + Long.toHexString(want) + " -> no vtable below"); continue; }
            long vt = e.getKey();
            out.println(String.format("in:0x%X -> %s vtable exe+0x%X, slot [%X]", want, e.getValue(), vt, want - vt));
            describe(e.getValue(), vt);
            for (int k = 0; k < 32; k++) {
                long v;
                try { v = getLong(toAddr(base + vt + k * 8)) - base; } catch (Exception ex) { break; }
                if (v <= 0 || v >= 0xE00000L) break;
                Function f = getFunctionAt(toAddr(base + v));
                if (f == null) f = getFunctionContaining(toAddr(base + v));
                out.println(String.format("----- slot [%02X] exe+0x%X", k * 8, v));
                if (f == null || f.getBody().getNumAddresses() > 2500) { out.println("  (skipped)"); continue; }
                ghidra.app.decompiler.DecompileResults r = dec.decompileFunction(f, 120, monitor);
                if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            }
        }
        out.close();
        println("written " + a[0]);
    }
}
