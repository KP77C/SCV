// Renames functions: Rename.java <addr>=<name> ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
public class Rename extends GhidraScript { public void run() throws Exception { for(String s:getScriptArgs()){String[] p=s.split("=");
  var a=toAddr(p[0]); var f=getFunctionAt(a); if(f==null)f=createFunction(a,null); if(f!=null){f.setName(p[1],SourceType.USER_DEFINED);println("named "+p[0]+" "+p[1]);}}}}
