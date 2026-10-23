import sys,glob
# pick.py SEEDBASE K : every K-th... prints 5 evenly spaced frames per seed 1..8
b=int(sys.argv[1]); out=[]
for i in range(1,9):
    fs=sorted(glob.glob("w%d_*.ppm"%(b+i)),key=lambda f:int(f.split("_")[1].split(".")[0]))
    out+=[fs[int(j*len(fs)/5)] for j in range(5)] if fs else []
print(" ".join(out))
