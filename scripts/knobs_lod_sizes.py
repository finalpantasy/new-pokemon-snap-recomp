import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
import csv,collections,re
rows={}
for l in open(_ROOT+'/analysis/romfs/drp_unpacked.tsv',encoding='utf8'):
    p=l.rstrip('\n').split('\t')
    if len(p)>6 and p[2].endswith('.numshb'): rows[(p[0],p[2])]=(int(p[5]),p[6])
r12=[];same=0;n=0;r01=[];base=0
for (a,e),(sz,h) in rows.items():
    if e.endswith('_LOD1.numshb'):
        b=e[:-len('_LOD1.numshb')]
        l0=rows.get((a,b+'.numshb')); l2=rows.get((a,b+'_LOD2.numshb'))
        if l0: r01.append(sz/l0[0])
        if l2:
            n+=1; same+= (l2[1]==h); r12.append(l2[0]/sz)
import statistics as s
print('LOD1/LOD0 pairs',len(r01),'median',s.median(r01),'min',min(r01),'max',max(r01))
print('LOD2 vs LOD1 pairs',n,'identical hash',same,'median size ratio',s.median(r12))
