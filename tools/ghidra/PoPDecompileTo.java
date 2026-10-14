// Writes Ghidra's decompiled C of each listed function to <outdir>/<addr>.c (skips existing
// files, so it resumes). An optional third column in the list replaces the function's own name
// in the output with FUN_<addr> (used to hide known names for a naming test).
// Headless: -process SLUS_207.43 -noanalysis -readOnly -scriptPath tools/ghidra
//           -postScript PoPDecompileTo.java <outdir> <list file: one hex address per line [, hide]>
// @category PoP

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public class PoPDecompileTo extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path out = Path.of(args[0]);
        Files.createDirectories(out);
        List<String> lines = Files.readAllLines(Path.of(args[1]));
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        int written = 0, skipped = 0, failed = 0;
        try {
            for (String line : lines) {
                String[] parts = line.trim().split("[,\\s]+");
                if (parts.length == 0 || parts[0].isEmpty()) {
                    continue;
                }
                long addr = Long.decode(parts[0]);
                Path file = out.resolve(String.format("%08x.c", addr));
                if (Files.exists(file)) {
                    skipped++;
                    continue;
                }
                Function f = getFunctionAt(toAddr(addr));
                if (f == null) {
                    failed++;
                    continue;
                }
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                if (!res.decompileCompleted()) {
                    failed++;
                    continue;
                }
                String c = res.getDecompiledFunction().getC();
                if (parts.length > 1 && parts[1].equals("hide")) {
                    c = c.replace(f.getName(), String.format("FUN_%08x", addr));
                    c = c.replaceAll("/\\* Prototype name: [^*]*\\*/\\n?", "");
                }
                Files.writeString(file, c);
                written++;
            }
        } finally {
            decomp.dispose();
        }
        println("Decompiled " + written + ", already there " + skipped + ", failed " + failed);
    }
}
