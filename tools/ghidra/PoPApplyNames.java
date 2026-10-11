// Renames functions from a "name,address" CSV (e.g. ps2_analyzer's SCE SDK
// signature matches), creating functions where none exist. Only renames
// functions that still have default names, so manual names survive.
// Headless: -process SLUS_207.43 -noanalysis -scriptPath tools/ghidra
//           -postScript PoPApplyNames.java <names.csv>
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public class PoPApplyNames extends GhidraScript {

    @Override
    public void run() throws Exception {
        List<String> lines = Files.readAllLines(Path.of(getScriptArgs()[0]));
        int renamed = 0, created = 0, kept = 0, failed = 0;
        for (String line : lines) {
            String[] parts = line.trim().split(",");
            if (parts.length != 2 || !parts[1].startsWith("0x")) {
                continue;
            }
            String name = parts[0];
            Address addr = toAddr(Long.decode(parts[1]));

            Function f = getFunctionAt(addr);
            if (f == null) {
                if (getInstructionAt(addr) == null) {
                    disassemble(addr);
                }
                f = createFunction(addr, name);
                if (f == null) {
                    println("Could not create " + name + " at " + addr);
                    failed++;
                } else {
                    created++;
                }
                continue;
            }
            if (f.getSymbol().getSource() != SourceType.DEFAULT && !f.getName().equals(name)) {
                kept++;
                continue;
            }
            f.setName(name, SourceType.ANALYSIS);
            renamed++;
        }
        println("Names: renamed " + renamed + ", created " + created + ", kept existing " + kept
            + ", failed " + failed);
    }
}
