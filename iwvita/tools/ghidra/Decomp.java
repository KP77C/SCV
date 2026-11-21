// Prints Ghidra decompiler output for the functions at the given addresses.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
public class Decomp extends GhidraScript {
  public void run() throws Exception {
    DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
    for(String a:getScriptArgs()){
      Function f=getFunctionContaining(toAddr(a)); if(f==null){println("none "+a);continue;}
      DecompileResults r=di.decompileFunction(f,60,monitor);
      println("=== "+a+"\n"+r.getDecompiledFunction().getC());
    }
  }
}
