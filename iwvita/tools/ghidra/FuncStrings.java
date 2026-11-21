// For each function, lists the defined strings it references: addr|string|string...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
public class FuncStrings extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w=new PrintWriter(getScriptArgs()[0]);
    Listing L=currentProgram.getListing(); ReferenceManager rm=currentProgram.getReferenceManager();
    for(Function f:currentProgram.getFunctionManager().getFunctions(true)){
      StringBuilder sb=new StringBuilder(f.getEntryPoint().toString());
      for(Address a:f.getBody().getAddresses(true)){
        for(Reference r:rm.getReferencesFrom(a)){
          Data d=L.getDataAt(r.getToAddress());
          if(d!=null&&d.hasStringValue()){String s=String.valueOf(d.getValue()).replace('|','/').replace('\n',' ');
            if(s.length()>80)s=s.substring(0,80); sb.append('|').append(s);}
        }
      }
      w.println(sb);
    }
    w.close();
  }
}
