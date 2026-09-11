//Sign placement machinery: symbols and the phantom-location region type.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;

public class Sign extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter("E:/ghidra/sign.txt");
        String[] want = { "sign", "soapstone", "summon", "phantomlocation", "whitesign", "multiplay" };
        for (String w : want) {
            out.println("");
            out.println("--- " + w + " ---");
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
            int n = 0;
            while (it.hasNext() && n < 30) {
                Symbol s = it.next();
                if (s.getName().toLowerCase().contains(w)) {
                    out.println(String.format("exe+0x%-9s %s",
                        Long.toHexString(s.getAddress().getOffset()-base), s.getName()));
                    n++;
                }
            }
            out.println("count " + n);
        }
        out.close();
        println("written sign");
    }
}
