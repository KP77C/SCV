// Pre-analysis: names the xboxkrnl thunk slots and the entry point.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.nio.file.*; import java.util.*;
public class XbeLabel extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs();
    Map<Integer,String> names=new HashMap<>();
    for(String l:Files.readAllLines(Paths.get(a[0]))){String[] p=l.trim().split(" ");names.put(Integer.parseInt(p[0]),p[1]);}
    long thunk=Long.decode(a[1]); long entry=Long.decode(a[2]);
    Address t=toAddr(thunk); int n=0;
    while(true){int v=getInt(t); if(v==0)break; int ord=v&0x7fffffff;
      String nm=names.getOrDefault(ord,"xboxkrnl_"+ord);
      createLabel(t,"__imp_"+nm,true,SourceType.IMPORTED);
      createDWord(t); t=t.add(4); n++;}
    createLabel(toAddr(entry),"XbeEntry",true,SourceType.USER_DEFINED);
    addEntryPoint(toAddr(entry));
    println("THUNKS "+n);
  }
}
