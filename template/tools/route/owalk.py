#!/usr/bin/env python3
"""tools/route/owalk.py OUTDIR TICK0 N SEEDS WALKSET WALKDIR [STEP] -> OUTDIR/branches.txt with one walker branch per seed (our build, run_game --branch)"""
import random,sys,os
out,t0,n,seeds,wset,wdir=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4],sys.argv[5],float(sys.argv[6])
step=int(sys.argv[7]) if len(sys.argv)>7 else 500
os.makedirs(out,exist_ok=True)
L=[]
for sd in seeds.split(","):
    r=random.Random(int(sd)); t=t0+20; ps=[]
    while t<t0+n:
        if r.random()<wdir:
            d=r.choice(wset.split(",")); l=r.randint(60,220); ps.append("%s@%d-%d"%(d,t,t+l)); t+=l+5
        else:
            ps.append("cross@%d-%d"%(t,t+8)); t+=40
    L.append("%s/w%s_:%d:%d %s"%(os.path.abspath(out),sd,t0,step," ".join(ps)))
open(out+"/branches.txt","w").write("\n".join(L)+"\n")
