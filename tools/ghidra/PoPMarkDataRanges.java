// Clears code that Ghidra auto-analysis wrongly created inside data regions of
// SLUS_207.43 (Prince of Persia: The Sands of Time, US v1.01), so the PS2Recomp
// export only lists real EE code. Addresses are specific to this ELF build.
// Run headless with -process SLUS_207.43 -noanalysis (not -readOnly).
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.util.ArrayList;
import java.util.List;

public class PoPMarkDataRanges extends GhidraScript {

    // [start, endExclusive). Determined 11 Oct 2026; see CLAUDE.md "First recompile results".
    private static final long[][] DATA_RANGES = {
        {0x004EB020L, 0x00651380L}, // data tables, libipu tables, embedded IOP modules
        {0x00652E64L, 0x0066D080L}, // data after the last real function, to end of section
    };

    // Embedded IOP (R3000) modules, loaded from EE memory at runtime.
    private static final Object[][] IRX_MODULES = {
        {0x0052DBC0L, "irx_sio2man"},
        {0x0052F580L, "irx_padman"},
        {0x0053A500L, "irx_unknown_53a500"},
        {0x00551A40L, "irx_mcserv"},
        {0x00553740L, "irx_libsd"},
        {0x0055A700L, "irx_sdrdrv"},
        {0x0055C6C0L, "irx_ubisound_driver"},
    };

    // Function whose body Ghidra ran on into the data that follows it.
    private static final long TRIMMED_FUNCTION = 0x00652E40L;

    @Override
    public void run() throws Exception {
        AddressSet data = new AddressSet();
        for (long[] r : DATA_RANGES) {
            data.add(toAddr(r[0]), toAddr(r[1] - 1));
        }

        reportIncomingFlows(data);

        List<Function> doomed = new ArrayList<>();
        FunctionIterator fit = currentProgram.getFunctionManager().getFunctions(true);
        while (fit.hasNext()) {
            Function f = fit.next();
            long entry = f.getEntryPoint().getOffset();
            if (data.contains(f.getEntryPoint()) || entry == TRIMMED_FUNCTION) {
                doomed.add(f);
            }
        }
        for (Function f : doomed) {
            currentProgram.getFunctionManager().removeFunction(f.getEntryPoint());
        }
        println("Removed " + doomed.size() + " functions");

        for (long[] r : DATA_RANGES) {
            clearListing(toAddr(r[0]), toAddr(r[1] - 1));
        }
        println("Cleared code in " + DATA_RANGES.length + " data ranges");

        Function trimmed = createFunction(toAddr(TRIMMED_FUNCTION), null);
        println("Recreated " + (trimmed == null ? "NOTHING at " + toAddr(TRIMMED_FUNCTION)
            : trimmed.getName() + " body " + trimmed.getBody()));

        for (Object[] m : IRX_MODULES) {
            createLabel(toAddr((Long) m[0]), (String) m[1], true, SourceType.USER_DEFINED);
        }
        println("Labelled " + IRX_MODULES.length + " embedded IOP modules");
    }

    // Calls or jumps from code outside the data ranges into them would mean the
    // ranges are wrong (or the code is). Print them so they get looked at.
    private void reportIncomingFlows(AddressSet data) {
        int count = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction insn = it.next();
            if (data.contains(insn.getAddress())) {
                continue;
            }
            for (Reference ref : insn.getReferencesFrom()) {
                if (ref.getReferenceType().isFlow() && data.contains(ref.getToAddress())) {
                    if (count < 50) {
                        println("Incoming flow: " + insn.getAddress() + " -> " + ref.getToAddress()
                            + " (" + ref.getReferenceType() + ")");
                    }
                    count++;
                }
            }
        }
        println("Incoming flows into data ranges: " + count);
    }
}
