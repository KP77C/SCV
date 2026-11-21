// Prints vtable slot targets: VSlot.java <vtable> <slot> [slot...]
import ghidra.app.script.GhidraScript;
public class VSlot extends GhidraScript { public void run() throws Exception { String[] a=getScriptArgs(); long vt=Long.decode(a[0]);
  for(int i=1;i<a.length;i++){int s=Integer.parseInt(a[i]); long t=getInt(toAddr(vt+4L*s))&0xffffffffL; var f=getFunctionAt(toAddr(t)); println("slot "+s+" -> "+Long.toHexString(t)+" "+(f==null?"":f.getName()+" ["+f.getBody().getNumAddresses()+"]"));}}}
