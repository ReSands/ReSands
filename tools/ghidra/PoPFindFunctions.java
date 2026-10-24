// Prints the address of every function whose name contains one of the given substrings, and
// optionally writes the addresses to a list file for PoPDecompileTo.java.
// Headless: -process SLUS_207.43 -noanalysis -readOnly -scriptPath tools/ghidra
//           -postScript PoPFindFunctions.java [out=<list file>] Shadow Projector ...
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class PoPFindFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        Path out = null;
        List<String> needles = new ArrayList<>();
        for (String a : getScriptArgs()) {
            if (a.startsWith("out=")) {
                out = Path.of(a.substring(4));
            } else {
                needles.add(a);
            }
        }
        List<String> lines = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            String name = f.getName(true);
            for (String n : needles) {
                if (name.contains(n)) {
                    println(String.format("0x%08x %s", f.getEntryPoint().getOffset(), name));
                    lines.add(String.format("0x%x", f.getEntryPoint().getOffset()));
                    break;
                }
            }
        }
        if (out != null) {
            Files.write(out, lines);
        }
    }
}
