// Writes the disassembly of each listed function to <outdir>/<addr>.s (address, bytes, instruction,
// and the call target's name for jal). Use it when the decompiled C drops arguments (Ghidra often
// loses calls' register/stack arguments past a3).
// Headless: -process SLUS_207.43 -noanalysis -readOnly -scriptPath tools/ghidra
//           -postScript PoPDisasm.java <outdir> <list file: one hex address per line>
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public class PoPDisasm extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path out = Path.of(args[0]);
        Files.createDirectories(out);
        List<String> lines = Files.readAllLines(Path.of(args[1]));
        for (String line : lines) {
            String t = line.trim();
            if (t.isEmpty())
                continue;
            long addr = Long.decode(t.split("[,\\s]+")[0]);
            Address a = toAddr(addr);
            Function f = getFunctionAt(a);
            if (f == null) {
                println("no function at " + t);
                continue;
            }
            StringBuilder sb = new StringBuilder();
            sb.append("; ").append(f.getName()).append("\n");
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                sb.append(String.format("%08x  %s", ins.getAddress().getOffset(), ins.toString()));
                for (Reference r : ins.getReferencesFrom()) {
                    if (r.getReferenceType().isCall()) {
                        Function c = getFunctionAt(r.getToAddress());
                        if (c != null)
                            sb.append("   ; ").append(c.getName());
                    }
                }
                sb.append("\n");
            }
            Files.writeString(out.resolve(String.format("%08x.s", addr)), sb.toString());
        }
    }
}
