// Prints the instructions of the functions at the given addresses.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class Disasm extends GhidraScript {
  public void run() throws Exception {
    for(String a:getScriptArgs()){ Function f=getFunctionContaining(toAddr(a)); if(f==null){println("none "+a);continue;}
      println("=== "+f.getName());
      for(Instruction i:currentProgram.getListing().getInstructions(f.getBody(),true)) println(i.getAddress()+"  "+i);}
  }
}
