// Names functions from the Aug 2003 prototype, as matched by tools/proto_names.py
// (TSV: address, namespace "A::B" or empty, name, mangled name). Only renames functions
// that still have default names; existing names (ps2_analyzer's SDK names, manual names)
// are kept, and differing ones are listed for review. Puts the function in its class
// namespace and the mangled name in a plate comment.
// Headless: -process SLUS_207.43 -noanalysis -scriptPath tools/ghidra
//           -postScript PoPApplyProtoNames.java <proto_names.tsv>
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class PoPApplyProtoNames extends GhidraScript {

    private static final String COMMENT_PREFIX = "Prototype name: ";

    @Override
    public void run() throws Exception {
        List<String> lines = Files.readAllLines(Path.of(getScriptArgs()[0]));
        int renamed = 0, already = 0, noFunction = 0, failed = 0;
        List<String> differing = new ArrayList<>();

        for (String line : lines) {
            String[] parts = line.split("\t", -1);
            if (parts.length != 4) {
                continue;
            }
            Address addr = toAddr(Long.decode(parts[0]));
            String nsPath = parts[1];
            String name = parts[2];
            String mangled = parts[3];

            Function f = getFunctionAt(addr);
            if (f == null) {
                noFunction++;
                continue;
            }
            if (f.getSymbol().getSource() != SourceType.DEFAULT) {
                if (f.getName().equals(name)) {
                    already++;
                } else {
                    differing.add(addr + " " + f.getName(true) + " (prototype: "
                        + (nsPath.isEmpty() ? "" : nsPath + "::") + name + ")");
                }
                continue;
            }
            try {
                if (!nsPath.isEmpty()) {
                    Namespace ns = NamespaceUtils.createNamespaceHierarchy(
                        nsPath, null, currentProgram, SourceType.IMPORTED);
                    f.setParentNamespace(ns);
                }
                f.setName(name, SourceType.IMPORTED);
                String plate = getPlateComment(addr);
                if (plate == null || !plate.contains(COMMENT_PREFIX)) {
                    setPlateComment(addr, (plate == null ? "" : plate + "\n") + COMMENT_PREFIX + mangled);
                }
                renamed++;
            } catch (Exception e) {
                println("Failed at " + addr + " " + name + ": " + e.getMessage());
                failed++;
            }
        }

        println("Prototype names: renamed " + renamed + ", already named " + already
            + ", named differently " + differing.size() + ", no function at address " + noFunction
            + ", failed " + failed);
        for (String d : differing) {
            println("Differs: " + d);
        }
    }
}
