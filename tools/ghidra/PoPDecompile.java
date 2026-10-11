// Prints Ghidra's decompiled C for the functions containing the given addresses.
// Headless: -process SLUS_207.43 -noanalysis -readOnly -scriptPath tools/ghidra
//           -postScript PoPDecompile.java 0x10b658 0x495148 ...
// @category PoP

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class PoPDecompile extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try {
            for (String arg : getScriptArgs()) {
                Function f = getFunctionContaining(toAddr(Long.decode(arg)));
                if (f == null) {
                    println("== " + arg + ": no function");
                    continue;
                }
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                println("== " + arg + " in " + f.getName() + " @ " + f.getEntryPoint());
                println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : res.getErrorMessage());
            }
        } finally {
            decomp.dispose();
        }
    }
}
