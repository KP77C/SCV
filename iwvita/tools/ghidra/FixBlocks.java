// Marks the XBE data sections (.rdata and later, from 0x663720) as non-executable.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;
public class FixBlocks extends GhidraScript { public void run() throws Exception {
  for(MemoryBlock b:currentProgram.getMemory().getBlocks()) if(b.getStart().getOffset()>=0x663720&&b.getStart().getAddressSpace().isMemorySpace()&&!b.getName().startsWith("_elf")){b.setExecute(false);println("data "+b.getName());}}}
