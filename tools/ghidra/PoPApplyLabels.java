// Labels data addresses from a TSV (address hex, name, ...; e.g. tools/decomp/dare_callees.py's globals.tsv).
// Rows whose name has an offset (name+0x4) are skipped. Only labels addresses whose primary symbol is
// a default one (DAT_...), so existing names survive.
// Headless: -process SLUS_207.43 -noanalysis -scriptPath tools/ghidra
//           -postScript PoPApplyLabels.java <globals.tsv>
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

import java.nio.file.Files;
import java.nio.file.Path;

public class PoPApplyLabels extends GhidraScript {

    @Override
    public void run() throws Exception {
        int labelled = 0, kept = 0, skipped = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            String[] f = line.split("\t");
            if (f.length < 2 || f[1].contains("+")) {
                skipped++;
                continue;
            }
            Address addr = toAddr(Long.parseLong(f[0], 16));
            Symbol s = getSymbolAt(addr);
            if (s != null && s.getSource() != SourceType.DEFAULT) {
                kept++;
                continue;
            }
            createLabel(addr, f[1], true, SourceType.ANALYSIS);
            labelled++;
        }
        println("Labels: labelled " + labelled + ", kept existing " + kept + ", skipped " + skipped);
    }
}
