import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
"""Usage: knobs_calltrace.py TARGET LO HI -- for each bl TARGET in [LO,HI) print the nearby arg-register setups (RT sizes / CreatePass)."""
import sys; sys.path.insert(0,_os.path.dirname(_os.path.abspath(__file__)))
from knobs_lib import *
tgt=int(sys.argv[1],16); lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
ins=list(md.disasm(rd(lo,hi-lo),lo)); pg={}
idx=[k for k,i in enumerate(ins) if i.mnemonic=='bl' and i.op_str=='#0x%x'%tgt]
for k in idx:
    regs={}; 
    for j in range(max(0,k-45),k):
        i=ins[j]; o=i.op_str
        if i.mnemonic=='adrp':
            r,v=o.split(', #'); pg[r]=int(v,16)
        if i.mnemonic in('mov','movk','add','ldr','lsr','lsl','sub','ldp','and','orr','mul','fmov') and o.split(',')[0] in ('w1','w2','w3','w4','w5','w6','x1','x2','x3','x4','x5','x6','x7','w7','s0','s1'):
            r=o.split(',')[0]; s=i.mnemonic+' '+o
            if i.mnemonic=='add' and r=='x5' and ins[j-1].mnemonic=='adrp': pass
            regs[r]=s
            if i.mnemonic=='add' and o.startswith(('x5','x3','x4')) and ', #0x' in o:
                p=o.split(', '); 
                try:
                    a=pg[p[1]]+int(p[2][1:],16); s2=cstr(a,50)
                    if s2.isprintable() and len(s2)>3: regs[r]=s+' ="%s"'%s2
                except: pass
    print(hex(ins[k].address),{r:regs[r] for r in sorted(regs)})
