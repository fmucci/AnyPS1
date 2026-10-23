#!/usr/bin/env python3
"""walk.py IN_STATE FRAME_IN FRAMES SEED OUTPREFIX: random walker branch (directions held 60-200 frames, Cross taps)"""
import random,subprocess,sys,os
st,fin,n,seed,pre=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),sys.argv[5]
r=random.Random(seed); t=fin+20; ps=[]
while t<fin+n:
    k=r.random()
    if k<float(os.environ.get("WALK_DIR","0.7")):
        d=r.choice(os.environ.get("WALK_SET","up,down,left,right,up,right").split(",")); l=r.randint(60,220); ps.append("%s@%d-%d"%(d,t,t+l)); t+=l+5
    else:
        ps.append("cross@%d-%d"%(t,t+8)); t+=40
subprocess.run([sys.executable,os.path.join(os.path.dirname(os.path.abspath(__file__)),"refstep.py"),st,str(fin),str(fin+n),pre+"state.st",",".join(ps),"300",pre],check=True)
