// Post-analysis: writes address,size,block,name for every function and a per-block summary.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.io.*; import java.util.*;
public class ExportFuncs extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w=new PrintWriter(getScriptArgs()[0]);
    Map<String,long[]> sum=new TreeMap<>();
    for(Function f:currentProgram.getFunctionManager().getFunctions(true)){
      long sz=f.getBody().getNumAddresses();
      MemoryBlock b=currentProgram.getMemory().getBlock(f.getEntryPoint());
      String bn=b==null?"?":b.getName();
      w.printf("%s,%d,%s,%s%n",f.getEntryPoint(),sz,bn,f.getName());
      long[] s=sum.computeIfAbsent(bn,k->new long[2]); s[0]++; s[1]+=sz;}
    w.close();
    for(var e:sum.entrySet()) println("BLOCK "+e.getKey()+" funcs "+e.getValue()[0]+" bytes "+e.getValue()[1]);
  }
}
