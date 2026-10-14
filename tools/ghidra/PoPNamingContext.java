// Writes naming context for each listed function to <outdir>/<addr>.txt: its class (namespace),
// the functions that call it, the functions it calls, and the strings it references. A second
// column in the list is a name to hide (calibration functions): any occurrence becomes FUN_<addr>.
// Headless: -process SLUS_207.43 -noanalysis -readOnly -scriptPath tools/ghidra
//           -postScript PoPNamingContext.java <outdir> <list file: hex address [hidden name]>
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.TreeSet;

public class PoPNamingContext extends GhidraScript {

    private String label(Function f) {
        String ns = f.getParentNamespace().isGlobal() ? "" : f.getParentNamespace().getName(true) + "::";
        return ns + f.getName();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path out = Path.of(args[0]);
        Files.createDirectories(out);
        List<String> lines = Files.readAllLines(Path.of(args[1]));
        int written = 0;
        for (String line : lines) {
            String[] parts = line.trim().split("[,\\s]+");
            if (parts.length == 0 || parts[0].isEmpty()) {
                continue;
            }
            long a = Long.parseLong(parts[0].replace("0x", ""), 16);
            Function f = getFunctionAt(toAddr(a));
            if (f == null) {
                continue;
            }
            String hide = parts.length > 1 ? parts[1] : null;
            String self = String.format("FUN_%08x", a);
            TreeSet<String> callers = new TreeSet<>(), callees = new TreeSet<>(), strings = new TreeSet<>();
            for (Function c : f.getCallingFunctions(monitor)) {
                callers.add(c.equals(f) ? self : label(c));
            }
            for (Function c : f.getCalledFunctions(monitor)) {
                callees.add(c.equals(f) ? self : label(c));
            }
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    Address to = r.getToAddress();
                    Data d = getDataAt(to);
                    if (d != null && d.hasStringValue()) {
                        strings.add(StringDataInstance.getStringDataInstance(d).getStringValue());
                    } else if (d == null && to.getOffset() >= 0x4e8000L && to.getOffset() < 0x6e0000L) {
                        // undefined data: try reading a C string
                        try {
                            byte[] b = getBytes(to, 80);
                            int n = 0;
                            while (n < b.length && b[n] >= 0x20 && b[n] < 0x7f) {
                                n++;
                            }
                            if (n >= 5 && n < b.length && b[n] == 0) {
                                strings.add(new String(b, 0, n, "ISO-8859-1"));
                            }
                        } catch (Exception e) {
                            // unreadable
                        }
                    }
                }
            }
            String ns = f.getParentNamespace().isGlobal() ? "(none)" : f.getParentNamespace().getName(true);
            StringBuilder sb = new StringBuilder();
            sb.append("Class/namespace: ").append(ns).append('\n');
            sb.append("Called by (").append(callers.size()).append("): ").append(String.join(", ", callers.stream().limit(25).toList())).append('\n');
            sb.append("Calls (").append(callees.size()).append("): ").append(String.join(", ", callees.stream().limit(40).toList())).append('\n');
            sb.append("Strings referenced: ").append(strings.isEmpty() ? "(none)" : String.join(" | ", strings.stream().limit(20).map(s -> '"' + s + '"').toList())).append('\n');
            String text = sb.toString();
            if (hide != null && !hide.isEmpty()) {
                text = text.replaceAll("(?<![A-Za-z0-9_])" + java.util.regex.Pattern.quote(hide) + "(?![A-Za-z0-9_])", self);
            }
            Files.writeString(out.resolve(String.format("%08x.txt", a)), text);
            written++;
        }
        println("naming context: " + written + " written");
    }
}
