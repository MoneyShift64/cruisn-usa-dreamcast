import struct
def vlq(b,i):
    v=0
    while True:
        c=b[i]; i+=1; v=(v<<7)|(c&0x7f)
        if not c&0x80: return v,i
def parse(path):
    b=open(path,'rb').read()
    assert b[:4]==b'MThd'
    fmt,ntrk,div=struct.unpack('>HHH',b[8:14]); p=14
    tracks=[]
    for t in range(ntrk):
        assert b[p:p+4]==b'MTrk', b[p:p+4]
        ln=struct.unpack('>I',b[p+4:p+8])[0]; q=p+8; end=q+ln; p=end
        tick=0; run=None; ev=[]; name=''
        while q<end:
            d,q=vlq(b,q); tick+=d
            c=b[q]
            if c==0xFF:
                typ=b[q+1]; l,q2=vlq(b,q+2); data=b[q2:q2+l]; q=q2+l
                if typ==0x51: ev.append((tick,'tempo',int.from_bytes(data,'big')))
                elif typ in (3,4): name=data.decode('latin1')
                elif typ==0x2f: break
                continue
            if c in (0xF0,0xF7):
                l,q2=vlq(b,q+1); q=q2+l; continue
            if c&0x80: run=c; q+=1
            st=run; ch=st&15; hi=st&0xF0
            if hi in (0xC0,0xD0): a=b[q]; q+=1; ev.append((tick,'prog' if hi==0xC0 else 'at',ch,a)); continue
            a,v=b[q],b[q+1]; q+=2
            if hi==0x90 and v>0: ev.append((tick,'on',ch,a,v))
            elif hi==0x80 or (hi==0x90 and v==0): ev.append((tick,'off',ch,a))
            elif hi==0xB0: ev.append((tick,'cc',ch,a,v))
            elif hi==0xE0: ev.append((tick,'bend',ch,(v<<7|a)-8192))
        tracks.append((name,ev))
    return fmt,div,tracks
if __name__=='__main__':
    fmt,div,tr=parse('jbr.mid')
    print('fmt',fmt,'div',div)
    for name,ev in tr:
        ons=[e for e in ev if e[1]=='on']
        chs=sorted(set(e[2] for e in ons)); keys=sorted(set(e[3] for e in ons))
        print(repr(name), len(ev),'notes',len(ons),'ch',chs,'keys',keys if len(keys)<20 else (min(keys),max(keys)), [e for e in ev if e[1] in ('tempo','prog','cc')][:6])
