// Gives switch jump-table targets entry points. For a switch Ghidra did not resolve, the
// table's targets are referenced only from data, so ExportPS2Functions.java writes no entry
// for them and the recompiled game stops at "[guest-branch:missing-target]" the first time
// the switch runs (e.g. 0x191970, fn_lGetEffectRequiredSize; 0x1581b0 in the sound
// association code). This finds the compiler's pattern
//     sltiu  at, rI, N          (bound, within the 12 instructions before the jr)
//     lui    rT, hi ; addiu rT, rT, lo   (table address)
//     lw     rJ, 0(...)  ;  jr rJ
// reads the N table words, disassembles each target and adds a COMPUTED_JUMP reference from
// the jr to it, so the export creates the entries. Tables with any target outside code are
// skipped. Headless: -process SLUS_207.43 -noanalysis -scriptPath tools/ghidra
//                    -postScript PoPJumpTables.java
// @category PoP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.util.ArrayList;
import java.util.List;

public class PoPJumpTables extends GhidraScript {

    private static final int LOOKBACK = 12;
    private static final int MAX_CASES = 512;

    private boolean isCode(long address) {
        MemoryBlock block = currentProgram.getMemory().getBlock(toAddr(address));
        return block != null && block.isExecute() && (address & 3) == 0;
    }

    private int word(Address address) throws Exception {
        return currentProgram.getMemory().getInt(address);
    }

    @Override
    public void run() throws Exception {
        int tables = 0, targets = 0, disassembled = 0, references = 0, skipped = 0;
        List<Instruction> jumps = new ArrayList<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction insn = it.next();
            int w = word(insn.getAddress());
            // jr rs (SPECIAL funct 8), rs != ra
            if ((w >>> 26) == 0 && (w & 0x3f) == 8 && ((w >>> 21) & 31) != 31) {
                jumps.add(insn);
            }
        }

        for (Instruction jr : jumps) {
            Address jrAddr = jr.getAddress();
            Integer hi = null, lo = null, bound = null;
            for (int back = 1; back <= LOOKBACK; back++) {
                int w;
                try {
                    w = word(jrAddr.subtract(4L * back));
                } catch (Exception e) {
                    break;
                }
                int op = w >>> 26;
                int rs = (w >>> 21) & 31, rt = (w >>> 16) & 31;
                if (op == 0x0f && hi == null) {               // lui
                    hi = w & 0xffff;
                } else if (op == 0x09 && rs == rt && lo == null) { // addiu rT, rT, lo
                    lo = (int) (short) (w & 0xffff);
                } else if (op == 0x0b && bound == null) {      // sltiu
                    bound = w & 0xffff;
                }
            }
            if (hi == null || lo == null || bound == null || bound < 2 || bound > MAX_CASES) {
                continue;
            }
            long base = ((long) hi << 16) + lo;
            List<Long> cases = new ArrayList<>();
            boolean ok = true;
            for (int i = 0; i < bound; i++) {
                long target;
                try {
                    target = word(toAddr(base + 4L * i)) & 0xffffffffL;
                } catch (Exception e) {
                    ok = false;
                    break;
                }
                if (!isCode(target)) {
                    ok = false;
                    break;
                }
                cases.add(target);
            }
            if (!ok) {
                skipped++;
                continue;
            }
            tables++;
            for (long target : cases) {
                Address t = toAddr(target);
                targets++;
                if (getInstructionAt(t) == null) {
                    disassemble(t);
                    disassembled++;
                }
                boolean known = false;
                for (Reference ref : getReferencesFrom(jrAddr)) {
                    if (ref.getToAddress().equals(t)) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    currentProgram.getReferenceManager().addMemoryReference(jrAddr, t, RefType.COMPUTED_JUMP,
                        SourceType.USER_DEFINED, 0);
                    references++;
                }
            }
        }
        println("Jump tables: " + tables + " tables, " + targets + " targets, " + disassembled
            + " newly disassembled, " + references + " references added, " + skipped
            + " candidate tables skipped (targets outside code)");
    }
}
