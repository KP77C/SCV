// Finds UClass constructions (calls to the UClass registration constructor) and names the
// GetPrivateStaticClass / StaticClass / InternalConstructor functions. Writes a CSV.
// args: <uclass ctor addr> <out.csv> [apply]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*; import java.util.*; import java.util.regex.*;
public class FindClasses extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); Address ctor=toAddr(a[0]); boolean apply=a.length>2;
    PrintWriter w=new PrintWriter(a[1]);
    w.println("getter,name,config,size,flags,ctor,staticctor,staticclass,global,package");
    DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
    Set<Function> getters=new LinkedHashSet<>();
    for(Reference r:getReferencesTo(ctor)){ if(!r.getReferenceType().isCall())continue;
      Function f=getFunctionContaining(r.getFromAddress()); if(f!=null)getters.add(f);}
    Pattern call=Pattern.compile("FUN_"+String.format("%08x",ctor.getOffset())+"\\(([^;]*)\\);");
    Pattern sc=null;
    int n=0;
    for(Function g:getters){
      String c=di.decompileFunction(g,60,monitor).getDecompiledFunction().getC();
      Matcher m=call.matcher(c); if(!m.find())continue;
      String[] p=m.group(1).split(",");
      String name="?",cfg="?";
      List<String> strs=new ArrayList<>();
      for(String s:p){s=s.trim(); if(s.startsWith("\""))strs.add(s.replace("\"",""));}
      if(strs.size()>0)name=strs.get(0); if(strs.size()>1)cfg=strs.get(1);
      String size=p.length>1?p[1].trim():"?", flags="?", ictor="?", sctor="?";
      for(int i=0;i<p.length;i++){String s=p[i].trim();
        if(s.equals("\""+cfg+"\"")&&i+3<p.length){flags=p[i+1].trim();ictor=p[i+2].trim();sctor=p[i+3].trim();}}
      // StaticClass callers of the getter
      String scName="",glob="",pkg=""; int tries=0;
      for(Reference r:getReferencesTo(g.getEntryPoint())){
        if(!r.getReferenceType().isCall())continue;
        Instruction ci=getInstructionAt(r.getFromAddress()); if(ci==null)continue;
        Instruction nx=ci.getNext(); Instruction pv=ci.getPrevious();
        if(nx!=null&&nx.getMnemonicString().equals("ADD")&&nx.getDefaultOperandRepresentation(0).equals("ESP"))nx=nx.getNext();
        if(nx==null||pv==null)continue;
        // expect: PUSH "pkg" ; CALL getter ; MOV [glob],EAX
        if(!nx.getMnemonicString().equals("MOV")||nx.getNumOperands()<2)continue;
        Object[] o0=nx.getOpObjects(0); if(o0.length!=1||!(o0[0] instanceof Address))continue;
        if(!nx.getDefaultOperandRepresentation(1).equals("EAX"))continue;
        Address ga=(Address)o0[0];
        String pk="";
        if(pv.getMnemonicString().equals("PUSH")){ Object[] po=pv.getOpObjects(0);
          if(po.length==1){ Address pa=po[0] instanceof Address?(Address)po[0]:(po[0] instanceof ghidra.program.model.scalar.Scalar?toAddr(((ghidra.program.model.scalar.Scalar)po[0]).getUnsignedValue()):null);
            if(pa!=null){Data d=getDataAt(pa); if(d!=null&&d.hasStringValue())pk=String.valueOf(d.getValue());}}}
        glob=ga.toString(); pkg=pk; Function s=getFunctionContaining(ci.getAddress()); scName=s==null?"":s.getEntryPoint().toString();
        if(apply&&!name.equals("?")){
          createLabel(ga,name+"_PrivateStaticClass",true,SourceType.ANALYSIS);
          if(s!=null&&s.getBody().getNumAddresses()<=96&&s.getName().startsWith("FUN_")) s.setName(name+"_StaticClass",SourceType.ANALYSIS);}
        break;
      }
      if(apply&&!name.equals("?")) g.setName(name+"_GetPrivateStaticClass",SourceType.ANALYSIS);
      w.printf("%s,%s,%s,%s,%s,%s,%s,%s,%s,%s%n",g.getEntryPoint(),name,cfg,size,flags,ictor,sctor,scName,glob,pkg); n++;
    }
    w.close(); println("CLASSES "+n+" of "+getters.size());
  }
}
