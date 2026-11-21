// From classes.csv (FindClasses output): follows each InternalConstructor to the C++ constructor,
// reads the vtable it stores, names ctor/vtable, and writes name,ctor,vtable,parentctor,slots.
// args: <classes.csv> <out.csv> [apply]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;
import java.io.*; import java.nio.file.*; import java.util.*;
public class ClassVtables extends GhidraScript {
  Instruction at(Address a){Instruction i=getInstructionAt(a); if(i==null){disassemble(a); i=getInstructionAt(a);} return i;}
  public void run() throws Exception {
    String[] a=getScriptArgs(); boolean apply=a.length>2;
    PrintWriter w=new PrintWriter(a[1]); w.println("name,ctor,vtable,basector,slots");
    Map<Long,String> ctorName=new HashMap<>(); List<String[]> rows=new ArrayList<>();
    for(String l:Files.readAllLines(Paths.get(a[0]))){ if(l.startsWith("getter"))continue; String[] p=l.split(",");
      if(p.length<6||!p[5].contains("LAB_"))continue;
      Address ic=toAddr(Long.parseLong(p[5].replaceAll(".*LAB_",""),16));
      Address ctor=null; Instruction i=at(ic);
      for(int k=0;k<6&&i!=null;k++,i=i.getNext()) if(i.getMnemonicString().equals("JMP")||i.getMnemonicString().equals("CALL")){
        Object[] o=i.getOpObjects(0); if(o.length==1&&o[0] instanceof Address){ctor=(Address)o[0];break;}}
      if(ctor==null)continue;
      Address vt=null,base=null; i=at(ctor);
      for(int k=0;k<40&&i!=null;k++,i=i.getNext()){
        String m=i.getMnemonicString();
        if(m.equals("CALL")&&base==null){Object[] o=i.getOpObjects(0); if(o.length==1&&o[0] instanceof Address)base=(Address)o[0];}
        if(m.equals("MOV")&&i.getDefaultOperandRepresentation(0).matches("dword ptr \\[E[A-Z]{2}\\]")){
          Object[] o=i.getOpObjects(1); if(o.length==1&&o[0] instanceof Scalar){vt=toAddr(((Scalar)o[0]).getUnsignedValue());break;}}
        if(m.equals("RET"))break;}
      if(vt==null)continue;
      int slots=0; Address s=vt;
      while(slots<400){ int v=getInt(s); MemoryBlockCheck: { if(getMemoryBlock(toAddr(v&0xffffffffL))==null||!getMemoryBlock(toAddr(v&0xffffffffL)).isExecute())break; }
        if(slots>0&&getSymbolAt(s)!=null&&getSymbolAt(s).getSource()!=SourceType.DEFAULT&&!getSymbolAt(s).getName().startsWith("FUN"))break;
        slots++; s=s.add(4);}
      ctorName.put(ctor.getOffset(),p[1]);
      rows.add(new String[]{p[1],ctor.toString(),vt.toString(),base==null?"":base.toString(),""+slots});
      if(apply){ createLabel(vt,p[1]+"_vtable",true,SourceType.ANALYSIS);
        Function f=getFunctionAt(ctor); if(f==null)f=createFunction(ctor,null);
        if(f!=null&&f.getName().startsWith("FUN_"))f.setName(p[1]+"_ctor",SourceType.ANALYSIS);}
    }
    for(String[] r:rows){ String bn=r[3].isEmpty()?"":ctorName.getOrDefault(Long.parseLong(r[3],16),"?"+r[3]);
      w.println(r[0]+","+r[1]+","+r[2]+","+bn+","+r[4]);}
    w.close(); println("VTABLES "+rows.size());
  }
}
