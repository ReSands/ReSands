// Prints every reference to the given addresses, with the containing function, and the
// callers of those functions (one level up). Also finds references to a string by its
// address. Headless: -process SLUS_207.43 -noanalysis -readOnly -scriptPath tools/ghidra
//                    -postScript PoPXrefs.java 0x6448c0 ...
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.util.LinkedHashSet;
import java.util.Set;

public class PoPXrefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address target = toAddr(Long.decode(arg));
            println("== refs to " + target + describe(target));
            Set<Function> funcs = new LinkedHashSet<>();
            for (Reference ref : getReferencesTo(target)) {
                Function f = getFunctionContaining(ref.getFromAddress());
                println("  " + ref.getFromAddress() + " " + ref.getReferenceType() + describe(ref.getFromAddress()));
                if (f != null) {
                    funcs.add(f);
                }
            }
            for (Function f : funcs) {
                println("  callers of " + f.getName() + " @ " + f.getEntryPoint() + ":");
                for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                    if (ref.getReferenceType().isCall()) {
                        println("    " + ref.getFromAddress() + describe(ref.getFromAddress()));
                    }
                }
            }
        }
    }

    private String describe(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "" : " in " + f.getName() + " @ " + f.getEntryPoint();
    }
}
