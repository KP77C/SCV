// Names native functions from {"int<Class>exec<Name>", func} pairs in the data sections.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
public class NameNatives extends GhidraScript {
  public void run() throws Exception {
    Memory m=currentProgram.getMemory(); int n=0;
    for(MemoryBlock b:m.getBlocks()){ if(b.getStart().getOffset()<0x663720||b.getName().startsWith("_elf"))continue;
      for(Address a=b.getStart(); a.compareTo(b.getEnd().subtract(7))<0; a=a.add(4)){
        long sp=m.getInt(a)&0xffffffffL, fp=m.getInt(a.add(4))&0xffffffffL;
        if(fp<0x12000||fp>=0x5d3980||sp<0x663720||sp>=0x7f32e0)continue;
        String s=readStr(toAddr(sp)); if(s==null||!s.matches("int[A-Z][A-Za-z0-9_]*exec[A-Za-z0-9_]+"))continue;
        int k=s.indexOf("exec",3); String cls=s.substring(3,k), fn=s.substring(k);
        Address f=toAddr(fp); Function F=getFunctionAt(f); if(F==null){disassemble(f);F=createFunction(f,null);}
        if(F!=null){F.setName(cls+"_"+fn,SourceType.ANALYSIS);n++;}
        println(String.format("NATIVE %s %s %08x",cls,fn,fp));
      }}
    println("NAMED "+n);
  }
  String readStr(Address a){ try{ StringBuilder sb=new StringBuilder(); for(int i=0;i<80;i++){int c=getByte(a.add(i))&0xff; if(c==0)return sb.toString(); if(c<32||c>126)return null; sb.append((char)c);} }catch(Exception e){} return null; }
}
