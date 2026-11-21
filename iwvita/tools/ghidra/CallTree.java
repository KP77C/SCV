// Prints the direct call tree from a function: CallTree.java <addr> <depth>; each line shows size and first strings referenced.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.util.*;
public class CallTree extends GhidraScript {
  Set<Address> seen=new HashSet<>();
  String strs(Function f){ StringBuilder sb=new StringBuilder(); int n=0;
    for(Instruction i:currentProgram.getListing().getInstructions(f.getBody(),true)) for(Reference r:i.getReferencesFrom()){
      Data d=getDataAt(r.getToAddress()); if(d!=null&&d.hasStringValue()&&n<3){String s=String.valueOf(d.getValue()); if(s.length()>40)s=s.substring(0,40); sb.append(" \"").append(s).append('"'); n++;}}
    return sb.toString();}
  void walk(Function f,int d,int max,String ind){
    boolean again=!seen.add(f.getEntryPoint());
    println(ind+f.getEntryPoint()+" "+f.getName()+" ["+f.getBody().getNumAddresses()+"]"+(again?" (seen)":strs(f)));
    if(again||d>=max)return;
    for(Instruction i:currentProgram.getListing().getInstructions(f.getBody(),true)){
      if(!i.getFlowType().isCall())continue;
      for(Address t:i.getFlows()){ Function c=getFunctionAt(t); if(c!=null)walk(c,d+1,max,ind+"  ");}}
  }
  public void run() throws Exception { String[] a=getScriptArgs(); walk(getFunctionAt(toAddr(a[0])),0,Integer.parseInt(a[1]),""); }
}
