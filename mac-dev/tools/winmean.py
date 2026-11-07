# usage: python3 -I winmean.py <png>...  -> mean brightness inside the mpv window area (x 60..520, y 80..320 of a 960-wide shot)
import sys, zlib, struct
def rows(p):
    d=open(p,'rb').read(); i=8; idat=b''
    while i<len(d):
        n,t=struct.unpack('>I4s',d[i:i+8]); c=d[i+8:i+8+n]
        if t==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',c[:10])
        if t==b'IDAT': idat+=c
        i+=12+n
    bpp={2:3,6:4}[ct]; raw=zlib.decompress(idat); prev=bytearray(w*bpp); k=0; out=[]
    for y in range(h):
        f=raw[k]; k+=1; line=bytearray(raw[k:k+w*bpp]); k+=w*bpp
        for x in range(len(line)):
            a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pp=a+b-c; pa,pb,pc=abs(pp-a),abs(pp-b),abs(pp-c)
                line[x]=(line[x]+(a if pa<=pb and pa<=pc else b if pb<=pc else c))&255
        out.append(line); prev=line
    return bpp,out
for p in sys.argv[1:]:
    bpp,r=rows(p); s=n=0
    for y in range(80,320,4):
        for x in range(60,520,4): s+=sum(r[y][x*bpp:x*bpp+3])/3; n+=1
    print(p.rsplit('/',1)[-1], round(s/n,1))
