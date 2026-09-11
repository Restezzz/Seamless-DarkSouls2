//Args: <output file> <RVA>[:<count>] ...
//Disassembly (address, bytes, text) of <count> instructions (default 12) from each RVA.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter out = new PrintWriter(a[0]);
        for (int i = 1; i < a.length; i++) {
            String[] p = a[i].split(":");
            long rva = Long.decode(p[0]);
            int count = p.length > 1 ? Integer.parseInt(p[1]) : 12;
            out.println(String.format("#### exe+0x%X", rva));
            Instruction ins = getInstructionAt(toAddr(base + rva));
            if (ins == null) ins = getInstructionAfter(toAddr(base + rva));
            for (int k = 0; k < count && ins != null; k++) {
                StringBuilder hex = new StringBuilder();
                for (byte x : ins.getBytes()) hex.append(String.format("%02X ", x));
                out.println(String.format("exe+0x%-7X  %-32s %s",
                        ins.getAddress().getOffset() - base, hex.toString().trim(), ins.toString()));
                ins = ins.getNext();
            }
        }
        out.close();
        println("written " + a[0]);
    }
}
