import socket,struct,time,sys,threading,select
import os
LOG=open(os.environ.get('PEER_LOG','/tmp/rioos_peer.log'),'w')
def log(*a):
    LOG.write(' '.join(str(x) for x in a)+'\n'); LOG.flush()
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('127.0.0.1',7002)); s.setblocking(False)
send=lambda f: s.sendto(f,('127.0.0.1',7001))
PM=bytes.fromhex('020000000001'); PIP=bytes([10,0,2,2]); BC=b'\xff'*6
KM=None; KIP=bytes([10,0,2,77]); ASSIGN=bytes([10,0,2,77])
def csum(d):
    if len(d)%2: d+=b'\0'
    t=sum(struct.unpack('!%dH'%(len(d)//2),d)); 
    while t>>16: t=(t&0xffff)+(t>>16)
    return (~t)&0xffff
def eth(dst,typ,pl): return dst+PM+struct.pack('!H',typ)+pl
def ip4(src,dst,proto,pl,ident=1):
    h=struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(pl),ident,0x4000,64,proto,0,src,dst)
    h=h[:10]+struct.pack('!H',csum(h))+h[12:]
    return h+pl
def tcp(src,dst,sp,dp,seq,ack,fl,data=b''):
    h=struct.pack('!HHIIBBHHH',sp,dp,seq,ack,0x50,fl,4096,0,0)
    ps=src+dst+struct.pack('!BBH',0,6,len(h)+len(data))
    c=csum(ps+h+data); h=h[:16]+struct.pack('!H',c)+h[18:]
    return h+data
def arp_reply(req):
    sha=req[22:28]; spa=req[28:32]; tpa=req[38:42]
    pl=struct.pack('!HHBBH',1,0x800,6,4,2)+PM+tpa+sha+spa
    send(eth(sha,0x806,pl))
def dhcp(req):
    u=req[14+20:]; b=u[8:]
    xid=b[4:8]; mac=b[28:34]; opts=b[240:]
    t=None;i=0
    while i<len(opts) and opts[i]!=255:
        if opts[i]==0: i+=1; continue
        if opts[i]==53: t=opts[i+2]
        i+=2+opts[i+1]
    if t not in (1,3): return
    rt=2 if t==1 else 5
    r=bytearray(240); r[0]=2;r[1]=1;r[2]=6; r[4:8]=xid; r[10]=0x80; r[16:20]=ASSIGN; r[20:24]=PIP; r[28:34]=mac; r[236:240]=bytes([0x63,0x82,0x53,0x63])
    o=bytes([53,1,rt,54,4])+PIP+bytes([1,4,255,255,255,0,3,4])+PIP+bytes([6,4,10,0,2,3,51,4,0,0,0x0e,0x10,255])
    pl=struct.pack('!HHHH',67,68,8+len(r)+len(o),0)+bytes(r)+o
    send(eth(BC,0x800,ip4(PIP,bytes([255]*4),17,pl)))
    log('dhcp',{1:'discover',3:'request'}[t],'->',{2:'offer',5:'ack'}[rt])
    global dhcp_done
    if rt==5: dhcp_done=True
dhcp_done=False
def ntp(f):
    ip=f[14:34]; u=f[34:]; sp=struct.unpack('!H',u[:2])[0]
    r=bytearray(48); r[0]=0x24; r[1]=2
    struct.pack_into('!I',r,40,1893456000+2208988800)
    pl=struct.pack('!HHHH',123,sp,8+48,0)+bytes(r)
    send(eth(f[6:12],0x800,ip4(PIP,ip[12:16],17,pl))); log('ntp: reply')
# tcp server: 8081 = HTTP (wget), 2323 = banner+echo (nc/telnet); other ports answer RST
C={}
def ts(kp,seq,ack,fl,dst_port,data=b''):
    send(eth(KM,0x800,ip4(PIP,KIP,6,tcp(PIP,KIP,dst_port,kp,seq,ack,fl,data),5)))
def tcp_in(f):
    global KIP
    KIP=f[26:30]
    ihl=(f[14]&15)*4; t=f[14+ihl:]
    sp,dp,seq,ack,off,fl=struct.unpack('!HHIIBB',t[:14]); data=t[(off>>4)*4:]
    k=(sp,dp)
    if fl&2 and not fl&0x10:
        if dp not in (8081,2323,8443):
            ts(sp,0,seq+1,0x14,dp); log('tcp: RST for port',dp); return
        C[k]={'iss':5000,'nxt':5001,'ack':seq+1,'fin':False,'req':b''}
        if dp==8443:
            sk=socket.create_connection(('127.0.0.1',18443)); sk.setblocking(False); C[k]['sock']=sk
        ts(sp,5000,seq+1,0x12,dp); log('tcp: SYN-ACK port',dp); return
    c=C.get(k)
    if not c: return
    if data:
        c['ack']=seq+len(data)
        if dp==8443:
            ts(sp,c['nxt'],c['ack'],0x10,dp)
            c['sock'].sendall(data); log('tls: fwd',len(data))
        elif dp==8081:
            c['req']+=data
            ts(sp,c['nxt'],c['ack'],0x10,dp)
            hd,sep,bd=c['req'].partition(b'\r\n\r\n')
            cl=0
            for ln in hd.split(b'\r\n')[1:]:
                if ln.lower().startswith(b'content-length:'): cl=int(ln.split(b':')[1])
            if sep and len(bd)>=cl:
                rl=hd.split(b'\r\n')[0].split(b' ')
                meth,path=rl[0],rl[1]
                if path==b'/redir': st=b'302 Found'; extra=b'Location: /final\r\n'; body=b'moved\n'
                elif path==b'/final': st=b'200 OK'; extra=b''; body=b'final-ok\n'
                elif path==b'/missing': st=b'404 Not Found'; extra=b''; body=b'nope\n'
                elif meth==b'POST': st=b'200 OK'; extra=b''; body=b'POST:'+bd+b'\n'
                else: st=b'200 OK'; extra=b''; body=b'wget-body-ok\n'
                if b'X-Test: 42' in hd: body=b'hdr-seen\n'
                resp=b'HTTP/1.0 '+st+b'\r\n'+extra+b'Content-Length: %d\r\n\r\n'%len(body)+(b'' if meth==b'HEAD' else body)
                ts(sp,c['nxt'],c['ack'],0x18,dp,resp); c['nxt']+=len(resp)
                ts(sp,c['nxt'],c['ack'],0x11,dp); c['nxt']+=1; c['fin']=True
                log('tcp: http served',repr(c['req'][:20]))
        else:
            out=b'ECHO:'+data
            ts(sp,c['nxt'],c['ack'],0x18,dp,out); c['nxt']+=len(out); log('tcp: echo',repr(data))
    elif fl&0x10 and not fl&1 and not c.get('banner') and dp==2323:
        c['banner']=True
        bn=b'BANNER ready\r\n'; ts(sp,c['nxt'],c['ack'],0x18,dp,bn); c['nxt']+=len(bn); log('tcp: banner sent')
    if fl&1:
        c['ack']=seq+len(data)+1
        ts(sp,c['nxt'],c['ack'],0x11 if not c['fin'] else 0x10,dp)
        if not c['fin']: c['nxt']+=1; c['fin']=True
        log('tcp: closed',k)
# ipv6
def v6cs(src,dst,msg):
    ps=src+dst+struct.pack('!IBBBB',len(msg),0,0,0,58); return csum(ps+msg)
def v6pkt(dstmac,src,dst,msg,hop=255):
    m=msg[:2]+struct.pack('!H',v6cs(src,dst,msg[:2]+b'\0\0'+msg[4:]))+msg[4:]
    h=struct.pack('!IHBB',0x60000000,len(m),58,hop)+src+dst
    return eth(dstmac,0x86dd,h+m)
PLL=bytes([0xfe,0x80,0,0,0,0,0,0,0x00,0,0,0xff,0xfe,0,0,1])
def v6_in(f):
    if f[14+6] in (6,17): v6_l4(f); return
    h=f[14:]; src=h[8:24]; dst=h[24:40]; m=h[40:]
    if m[0]==135 and m[8:24]==PLL:
        na=bytes([136,0,0,0,0x60,0,0,0])+PLL+bytes([2,1])+PM
        send(v6pkt(f[6:12],PLL,src,na)); log('v6: NS for peer -> NA')
    elif m[0]==128 and dst==PLL:
        send(v6pkt(f[6:12],PLL,src,bytes([129,0,0,0])+m[4:],64)); log('v6: echo request -> reply')
    elif m[0]==136: log('v6: NA from kernel',src.hex()); 
    elif m[0]==129: log('v6: echo reply from kernel')

# ---- IPv6 UDP echo (5353) and TCP server (2323: banner + echo) ----
def csum6(src,dst,nh,seg):
    ps=src+dst+struct.pack('!IBBBB',len(seg),0,0,0,nh); return csum(ps+seg)
def v6raw(dstmac,src,dst,nh,seg):
    h=struct.pack('!IHBB',0x60000000,len(seg),nh,64)+src+dst
    return eth(dstmac,0x86dd,h+seg)
C6={}
def t6(kp,dp,src,dst,seq,ack,fl,data=b''):
    h=struct.pack('!HHIIBBHHH',dp,kp,seq,ack,0x50,fl,4096,0,0)+data
    h=h[:16]+struct.pack('!H',csum6(src,dst,6,h[:16]+b'\0\0'+h[18:]))+h[18:]
    send(v6raw(KM,src,dst,6,h))
CL={'st':0,'sp':45000}
def v6_l4(f):
    h=f[14:]; nh=h[6]; src=h[8:24]; dst=h[24:40]; m=h[40:40+struct.unpack('!H',h[4:6])[0]]
    if dst!=PLL: return
    if nh==6 and struct.unpack('!H',m[2:4])[0]==CL['sp']:
        sp,dp,seq,ack,off,fl=struct.unpack('!HHIIBB',m[:14])
        if CL['st']==1 and fl&0x12==0x12:
            msg=b'hello-v6-client\n'
            t6(sp,dp,PLL,src,9001,seq+1,0x19,msg); CL['st']=2; log('v6tcp: client sent data+FIN')
        elif CL['st']==2 and fl&1:
            t6(sp,dp,PLL,src,9001+len(b'hello-v6-client\n')+1,seq+1+0,0x10); CL['st']=3; log('v6tcp: client done')
        return
    if nh==17:
        sp,dp,ln,ck=struct.unpack('!HHHH',m[:8])
        if dp==5353:
            pl=b'UDP6:'+m[8:]
            u=struct.pack('!HHHH',5353,sp,8+len(pl),0)+pl
            u=u[:6]+struct.pack('!H',csum6(PLL,src,17,u[:6]+b'\0\0'+u[8:]) or 0xffff)+u[8:]
            send(v6raw(KM,PLL,src,17,u)); log('v6udp: echo',repr(m[8:]))
    elif nh==6:
        sp,dp,seq,ack,off,fl=struct.unpack('!HHIIBB',m[:14]); data=m[(off>>4)*4:]
        k=(sp,dp)
        if fl&2 and not fl&0x10:
            if dp!=2323: t6(sp,dp,PLL,src,0,seq+1,0x14); return
            C6[k]={'nxt':7001,'ack':seq+1,'banner':False,'fin':False}
            t6(sp,dp,PLL,src,7000,seq+1,0x12); log('v6tcp: SYN-ACK'); return
        c=C6.get(k)
        if not c: return
        if data:
            c['ack']=seq+len(data); out=b'ECHO6:'+data
            t6(sp,dp,PLL,src,c['nxt'],c['ack'],0x18,out); c['nxt']+=len(out); log('v6tcp: echo',repr(data))
        elif fl&0x10 and not fl&1 and not c['banner']:
            c['banner']=True; bn=b'BANNER6 ready\r\n'
            t6(sp,dp,PLL,src,c['nxt'],c['ack'],0x18,bn); c['nxt']+=len(bn); log('v6tcp: banner')
        if fl&1:
            c['ack']=seq+len(data)+1
            t6(sp,dp,PLL,src,c['nxt'],c['ack'],0x11 if not c['fin'] else 0x10)
            if not c['fin']: c['nxt']+=1; c['fin']=True
            log('v6tcp: closed')
KLL=None
def kernel_ll(mac):
    return bytes([0xfe,0x80,0,0,0,0,0,0,mac[0]^2,mac[1],mac[2],0xff,0xfe,mac[3],mac[4],mac[5]])
def v6_probe():
    sn=bytes([0xff,2]+[0]*9+[1,0xff])+KLL[13:16]
    ns=bytes([135,0,0,0,0,0,0,0])+KLL+bytes([1,1])+PM
    send(v6pkt(bytes([0x33,0x33,0xff])+KLL[13:16],PLL,sn,ns))
    time.sleep(0.3)
    send(v6pkt(KM,PLL,KLL,bytes([128,0,0,0,0x52,0x49,0,1])+b'abcdefgh',64))
t0=time.time(); last=0; vlast=0
while time.time()-t0<float(sys.argv[1]):
    socks=[c['sock'] for c in C.values() if c.get('sock')]
    r,_,_=select.select([s]+socks,[],[],0.2)
    for so in r:
        if so is s: continue
        for k,c in list(C.items()):
            if c.get('sock') is so:
                try: d=so.recv(65536)
                except BlockingIOError: continue
                except Exception: d=b''
                if d:
                    for i in range(0,len(d),1200):
                        chunk=d[i:i+1200]
                        ts(k[0],c['nxt'],c['ack'],0x18,k[1],chunk); c['nxt']+=len(chunk); time.sleep(0.002)
                    log('tls: relayed',len(d))
                else:
                    ts(k[0],c['nxt'],c['ack'],0x11,k[1]); c['nxt']+=1; c['sock']=None; log('tls: server closed')
    if s in r:
        f=s.recv(2000)
        if len(f)<14: continue
        ty=struct.unpack('!H',f[12:14])[0]
        if ty==0x806 and f[20:22]==b'\0\1': arp_reply(f)
        elif ty==0x806: KM=f[6:12]; 
        elif ty==0x800:
            KM=f[6:12]
            if f[23]==17 and f[14+20+2:14+20+4]==b'\0\x43': dhcp(f)
            elif f[23]==17 and f[14+20+2:14+20+4]==b'\0\x7b': ntp(f)
            elif f[23]==6: tcp_in(f)
            elif f[23]==1: 
                ic=f[34:]; 
                if ic[0]==8:
                    rep=bytes([0,0,0,0])+ic[4:]; rep=rep[:2]+struct.pack('!H',csum(rep[:2]+b'\0\0'+rep[4:]))+rep[4:]
                    send(eth(f[6:12],0x800,ip4(PIP,f[26:30],1,rep))); log('icmp echo reply')
        elif ty==0x86dd:
            KM=f[6:12]; KLL=kernel_ll(f[6:12]) if True else None
            v6_in(f)
    now=time.time()
    if KLL and KM and CL['st'] in (0,1) and now-CL.get('last',0)>2.5:
        CL['last']=now; CL['st']=1; t6(7777,CL['sp'],PLL,KLL,9000,0,0x02); log('v6tcp: client SYN sent')
    if dhcp_done and KM and now-vlast>3.0:
        vlast=now; KLL=kernel_ll(KM); v6_probe(); log('v6: probe sent to',KLL.hex())
log('end')
