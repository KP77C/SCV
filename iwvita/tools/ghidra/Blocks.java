import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;
public class Blocks extends GhidraScript { public void run() throws Exception {
  for(MemoryBlock b:currentProgram.getMemory().getBlocks()) println(b.getName()+" "+b.getStart()+" "+b.getEnd()+" x="+b.isExecute()+" init="+b.isInitialized());
  println(String.format("%08x %08x",getInt(toAddr(0x71edc0)),getInt(toAddr(0x71edc4))));}}
