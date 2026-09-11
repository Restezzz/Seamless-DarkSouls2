//Args: <output file> <regex> [max hits]
//Every instruction whose text matches the regex, with its function.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.regex.Pattern;

public class Grep extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        Pattern p = Pattern.compile(a[1]);
        int max = a.length > 2 ? Integer.parseInt(a[2]) : 300;
        PrintWriter out = new PrintWriter(a[0]);
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int hits = 0;
        while (it.hasNext() && hits < max) {
            Instruction ins = it.next();
            String t = ins.toString();
            if (!p.matcher(t).find()) continue;
            hits++;
            Function f = getFunctionContaining(ins.getAddress());
            long fe = f == null ? 0 : f.getEntryPoint().getOffset() - base;
            out.println(String.format("exe+0x%-7X in exe+0x%-7X  %s", ins.getAddress().getOffset() - base, fe, t));
        }
        out.println("hits: " + hits);
        out.close();
        println("written " + a[0]);
    }
}
