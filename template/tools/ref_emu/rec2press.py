#!/usr/bin/env python3
"""rec2press.py SESSION.rec [--ours] : turn an ff9_play --record file ("TICK PADWORD" per change, active-low pad word) into press ranges `button@first-last`
(comma separated, or `--press ...` with --ours). The ticks are the game's virtual frames since the player started: a session that starts from the Continue/Load
of a save starts the clock at the same place as run_game --card, so the list can be replayed with `run_game DISC --card CARD LIST` or compared with route_play.py."""
import sys
BITS={8:"select",9:"l3",10:"r3",11:"start",12:"up",13:"right",14:"down",15:"left",0:"l2",1:"r2",2:"l1",3:"r1",4:"triangle",5:"circle",6:"cross",7:"square"}
def main(path, ours):
    ev=[(int(a),int(b,16)) for a,b in (l.split() for l in open(path) if l.strip())]
    start={}; out=[]
    for i,(t,w) in enumerate(ev):
        for b,n in BITS.items():
            down=not (w>>b)&1
            if down and n not in start: start[n]=t
            if not down and n in start: out.append((n,start.pop(n),t-1))
    last=ev[-1][0] if ev else 0
    for n,t in start.items(): out.append((n,t,last))
    out.sort(key=lambda x:x[1])
    if ours: out=[(n,max(a-1,0),max(b-1,0)) for n,a,b in out]      # the player records a press at the frame that SAMPLED it +1; run_game/ps1_play --press apply a range from its first tick: one tick earlier (verified: the audio level at ticks 600..3000 of a person's session matches their log exactly only with this shift)
    fmt=lambda x:"%s@%d-%d"%x
    print(" ".join("--press "+fmt(x) for x in out) if ours else ",".join(map(fmt,out)))
if __name__=="__main__": main(sys.argv[1], "--ours" in sys.argv)
