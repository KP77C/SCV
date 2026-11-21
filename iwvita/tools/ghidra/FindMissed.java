// Creates functions Ghidra missed: every code pointer in .rdata/.data that points to the start
// of non-function bytes in .text, then gaps between functions in .text (after CC/90 padding).
// Prints coverage before and after.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
public class FindMissed extends GhidraScript {
  long cover(MemoryBlock t){ long n=0; for(Function f:currentProgram.getFunctionManager().getFunctions(true)) if(t.contains(f.getEntryPoint())) n+=f.getBody().getNumAddresses(); return n;}
  boolean inFunc(Address a){return getFunctionContaining(a)!=null;}
  int tryMake(Address a) throws Exception{
    if(inFunc(a))return 0; if(getInstructionAt(a)==null){ if(getDataAt(a)!=null&&getDataAt(a).isDefined())return 0; disassemble(a);} 
    if(getInstructionAt(a)==null)return 0; return createFunction(a,null)!=null?1:0;}
  public void run() throws Exception {
    Memory m=currentProgram.getMemory(); MemoryBlock text=null;
    for(MemoryBlock b:m.getBlocks()) if(b.getStart().getOffset()==0x12000) text=b;
    println("text "+text.getSize()+" covered before "+cover(text));
    int made=0;
    for(MemoryBlock b:m.getBlocks()){ if(b.getStart().getOffset()<0x663720||b.getName().startsWith("_elf"))continue;
      for(Address a=b.getStart(); a.compareTo(b.getEnd().subtract(3))<0; a=a.add(4)){
        long v; try{v=m.getInt(a)&0xffffffffL;}catch(Exception e){continue;}
        if(v<0x12000||v>=0x12000+text.getSize())continue;
        Address t=toAddr(v); if(!inFunc(t)) made+=tryMake(t);
        if(monitor.isCancelled())return;}}
    println("from pointers "+made);
    // gaps: only at 16-byte aligned addresses right after CC/90 padding that follows a function end
    int gaps=0; Address a=text.getStart(); Address end=text.getEnd();
    while(a.compareTo(end)<0){
      Function f=getFunctionContaining(a);
      if(f!=null){ a=f.getBody().getMaxAddress().add(1); continue;}
      Address p=a; int pad=0;
      while(p.compareTo(end)<0){int b=m.getByte(p)&0xff; if(b!=0xcc&&b!=0x90)break; p=p.add(1); pad++;}
      if((p.getOffset()&15)==0&&getFunctionContaining(p)==null){
        int r=tryMake(p); gaps+=r; if(r==1){a=p;continue;}
      }
      // skip to the next function start
      Function nf=getFunctionAfter(p);
      if(nf==null)break; a=nf.getEntryPoint();
    }
    println("from gaps "+gaps);
    println("covered after "+cover(text)+" funcs "+currentProgram.getFunctionManager().getFunctionCount());
  }
}
