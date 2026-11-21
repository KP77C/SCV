// Prints N instructions starting at an address: DisasmAt.java <addr> <n>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class DisasmAt extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); Instruction i=getInstructionAt(toAddr(a[0]));
    if(i==null){disassemble(toAddr(a[0])); i=getInstructionAt(toAddr(a[0]));}
    for(int k=0;k<Integer.parseInt(a[1])&&i!=null;k++,i=i.getNext()) println(i.getAddress()+"  "+i);
  }
}
