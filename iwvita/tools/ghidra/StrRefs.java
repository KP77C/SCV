// For each argument string, finds defined strings equal to it and prints references to them (and dwords around data refs).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
public class StrRefs extends GhidraScript {
  public void run() throws Exception {
    java.util.Set<String> want=new java.util.HashSet<>(java.util.Arrays.asList(getScriptArgs()));
    for(Data d:currentProgram.getListing().getDefinedData(true)){
      if(!d.hasStringValue())continue; String v=String.valueOf(d.getValue()); if(!want.contains(v))continue;
      println("STR "+v+" @ "+d.getAddress());
      for(Reference r:getReferencesTo(d.getAddress())){
        Function f=getFunctionContaining(r.getFromAddress());
        String extra=""; if(f==null){ extra=String.format(" next=%08x %08x",getInt(r.getFromAddress().add(4)),getInt(r.getFromAddress().add(8)));}
        println("  ref "+r.getFromAddress()+" "+(f==null?"(data)":f.getName())+extra);}
    }
  }
}
