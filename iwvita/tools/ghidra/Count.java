import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class Count extends GhidraScript {
  public void run() throws Exception {
    FunctionIterator it=currentProgram.getFunctionManager().getFunctions(true);
    long n=0,bytes=0;
    while(it.hasNext()){Function f=it.next(); n++; bytes+=f.getBody().getNumAddresses();}
    println("FUNCS "+n+" BYTES "+bytes);
  }
}
