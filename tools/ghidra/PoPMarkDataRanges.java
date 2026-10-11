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
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
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

    // The ELF has a single "main" section, so Ghidra loads code and data as one
    // executable block. PS2Recomp's export only scans non-executable blocks for
    // function pointers (vtables, callbacks, .ctors), so split it up. Each entry:
    // start, name, executable. A block runs to the next entry's start.
    // .ctors: 48 static-constructor pointers walked by crt0 (a0..a1 = 0x652E70..0x652F30).
    private static final Object[][] BLOCKS = {
        {0x00100000L, ".text", true},
        {0x004EB020L, ".data", false},
        {0x00651380L, ".text.ctor_funcs", true},
        {0x00652E64L, ".data2", false},
        {0x00652E70L, ".ctors", false},
        {0x00652F30L, ".data3", false},
    };

    @Override
    public void run() throws Exception {
        splitBlocks();

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

        createCtorFunctions();
        createDataPointedFunctions();
        createOrphanPrologueFunctions();
        createAfterPaddingFunctions();

        for (Object[] m : IRX_MODULES) {
            createLabel(toAddr((Long) m[0]), (String) m[1], true, SourceType.USER_DEFINED);
        }
        println("Labelled " + IRX_MODULES.length + " embedded IOP modules");
    }

    // Some constructors are long leaf functions with no prologue, which the export's
    // data-pointer heuristic does not recognise, so make every .ctors target a function.
    private void createCtorFunctions() throws Exception {
        MemoryBlock ctors = currentProgram.getMemory().getBlock(".ctors");
        int created = 0;
        for (Address a = ctors.getStart(); a.compareTo(ctors.getEnd()) < 0; a = a.add(4)) {
            Address target = toAddr(getInt(a) & 0xFFFFFFFFL);
            if (getFunctionAt(target) != null) {
                continue;
            }
            if (getInstructionAt(target) == null) {
                disassemble(target);
            }
            Function f = createFunction(target, null);
            if (f == null) {
                println("Could not create .ctors function at " + target);
            } else {
                created++;
            }
        }
        println("Created " + created + " .ctors functions");
    }

    // Virtual methods and callbacks reached only through pointers in data (vtables)
    // are often not functions after auto-analysis: either never disassembled, or
    // disassembled but not owned by any function. Find data words pointing at such
    // code that starts like a function, and make functions there.
    private void createDataPointedFunctions() throws Exception {
        Memory memory = currentProgram.getMemory();
        int created = 0, candidates = 0;
        for (MemoryBlock block : memory.getBlocks()) {
            if (block.isExecute() || !block.isInitialized()) {
                continue;
            }
            for (Address a = block.getStart(); a.compareTo(block.getEnd().subtract(3)) <= 0; a = a.add(4)) {
                long target = getInt(a) & 0xFFFFFFFFL;
                if ((target & 3) != 0) {
                    continue;
                }
                MemoryBlock targetBlock = memory.getBlock(toAddr(target));
                // Only our own code blocks: the EE loader also defines executable regions
                // with no contents (reads throw).
                if (targetBlock == null || !targetBlock.isExecute() || !targetBlock.isInitialized()
                    || !targetBlock.getName().startsWith(".text")) {
                    continue;
                }
                Address t = toAddr(target);
                // Also when instructions exist but no function owns them: the export then
                // only emits a short entry_* fragment there, and a call into it returns
                // garbage (the allocator method at 0x1399C0 did exactly that).
                if (getFunctionContaining(t) != null || !looksLikeFunctionStart(t)) {
                    continue;
                }
                candidates++;
                if (getInstructionAt(t) == null) {
                    disassemble(t);
                }
                if (createFunction(t, null) != null) {
                    created++;
                }
            }
        }
        println("Data-pointed functions: " + candidates + " candidates, " + created + " created");
    }

    // Auto-analysis leaves many instruction runs that no function owns (reached only
    // through labels or pointers). The export turns those into short entry_* fragments,
    // so a call into one returns early with garbage. Make a function at every unowned
    // instruction that opens a stack frame (addiu $sp,$sp,-N), repeating until stable
    // because each new function can absorb or expose others.
    private void createOrphanPrologueFunctions() throws Exception {
        int created = 0, failed = 0;
        for (int pass = 0; pass < 5; pass++) {
            int before = created;
            for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
                if (!block.isExecute() || !block.getName().startsWith(".text")) {
                    continue;
                }
                InstructionIterator it = currentProgram.getListing()
                    .getInstructions(new AddressSet(block.getStart(), block.getEnd()), true);
                List<Address> starts = new ArrayList<>();
                while (it.hasNext()) {
                    Instruction insn = it.next();
                    if (getFunctionContaining(insn.getAddress()) != null) {
                        continue;
                    }
                    long word = getInt(insn.getAddress()) & 0xFFFFFFFFL;
                    if ((word & 0xFFFF0000L) == 0x27BD0000L && (word & 0x8000L) != 0) {
                        starts.add(insn.getAddress());
                    }
                }
                for (Address a : starts) {
                    if (getFunctionContaining(a) != null) {
                        continue;
                    }
                    if (createFunction(a, null) != null) {
                        created++;
                    } else {
                        failed++;
                    }
                }
            }
            if (created == before) {
                break;
            }
        }
        println("Orphan prologue functions: " + created + " created, " + failed + " failed");
    }

    // Leaf functions with no stack frame, reached only through addresses built at runtime
    // (e.g. method tables filled by code), are invisible to the passes above. The compiler
    // pads functions with zero words, so: non-zero code at an 8-aligned address that no
    // function owns, preceded by zero padding, where the code before the padding ended with
    // jr $ra, j or an unconditional b (possibly followed by its delay slot). 0x41CD00 is one.
    private void createAfterPaddingFunctions() throws Exception {
        int created = 0, failed = 0;
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        long start = text.getStart().getOffset() + 8, end = text.getEnd().getOffset() - 3;
        for (long a = start; a <= end; a += 8) {
            Address addr = toAddr(a);
            if (word(a) == 0 || word(a - 4) != 0 || getFunctionContaining(addr) != null) {
                continue;
            }
            long last = a - 4;
            while (last > start && word(last) == 0) {
                last -= 4;
            }
            if (!endsFlow(word(last)) && !endsFlow(word(last - 4))) {
                continue;
            }
            if (getInstructionAt(addr) == null) {
                disassemble(addr);
            }
            if (createFunction(addr, null) != null) {
                created++;
            } else {
                failed++;
            }
        }
        println("After-padding functions: " + created + " created, " + failed + " failed");
    }

    private long word(long a) throws Exception {
        return getInt(toAddr(a)) & 0xFFFFFFFFL;
    }

    // jr $ra, j target, or b (beq $zero,$zero).
    private static boolean endsFlow(long insn) {
        return insn == 0x03E00008L || (insn >>> 26) == 2 || (insn >>> 16) == 0x1000L;
    }

    // addiu $sp,$sp,-N in the first two instructions, or a leaf that returns
    // (jr $ra) within its first four.
    private boolean looksLikeFunctionStart(Address t) {
        for (int i = 0; i < 4; i++) {
            long insn;
            try {
                insn = getInt(t.add(4L * i)) & 0xFFFFFFFFL;
            } catch (Exception e) {
                return false;
            }
            if (i < 2 && (insn & 0xFFFF0000L) == 0x27BD0000L && (insn & 0x8000L) != 0) {
                return true;
            }
            if (insn == 0x03E00008L) {
                return true;
            }
        }
        return false;
    }

    // Safe to re-run: only splits where a block does not already start.
    private void splitBlocks() throws Exception {
        Memory memory = currentProgram.getMemory();
        for (Object[] b : BLOCKS) {
            Address start = toAddr((Long) b[0]);
            MemoryBlock block = memory.getBlock(start);
            if (block == null) {
                throw new IllegalStateException("No memory block at " + start);
            }
            if (!block.getStart().equals(start)) {
                memory.split(block, start);
            }
        }
        for (Object[] b : BLOCKS) {
            MemoryBlock block = memory.getBlock(toAddr((Long) b[0]));
            block.setName((String) b[1]);
            block.setExecute((Boolean) b[2]);
            println("Block " + block.getName() + " " + block.getStart() + "-" + block.getEnd()
                + (block.isExecute() ? " exec" : " data"));
        }
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
