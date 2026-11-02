import socket,struct,time,sys,threading,select
import os
LOG=open(os.environ.get('PEER_LOG','/tmp/rioos_peer.log'),'w')
def log(*a):
    LOG.write(' '.join(str(x) for x in a)+'\n'); LOG.flush()
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('127.0.0.1',7002)); s.setblocking(False)
send=lambda f: s.sendto(f,('127.0.0.1',7001))
PM=bytes.fromhex('020000000001'); PIP=bytes([10,0,2,2]); BC=b'\xff'*6
KM=None; KIP=None; ASSIGN=bytes([10,0,2,77])
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
# tcp client state
T={'st':0,'seq':1000,'ack':0,'resp':b'','sp':40000,'t':0}
def tcp_in(f):
    ihl=(f[14]&15)*4; t=f[14+ihl:]
    sp,dp,seq,ack,off,fl=struct.unpack('!HHIIBB',t[:14]); data=t[(off>>4)*4:]
    if dp!=T['sp']: return
    if T['st']==1 and fl&0x12==0x12:
        T['ack']=seq+1; T['seq']+=1; T['st']=2
        req=b'GET /home/user/a.txt HTTP/1.0\r\n\r\n'
        send(eth(KM,0x800,ip4(PIP,ASSIGN,6,tcp(PIP,ASSIGN,T['sp'],8080,T['seq'],T['ack'],0x10))))
        send(eth(KM,0x800,ip4(PIP,ASSIGN,6,tcp(PIP,ASSIGN,T['sp'],8080,T['seq'],T['ack'],0x18,req),2)))
        T['seq']+=len(req); log('tcp: syn-ack received, request sent')
    elif T['st']==2:
        if data:
            T['resp']+=data; T['ack']=seq+len(data)
            send(eth(KM,0x800,ip4(PIP,ASSIGN,6,tcp(PIP,ASSIGN,T['sp'],8080,T['seq'],T['ack'],0x10),3)))
        if fl&1:
            T['ack']+=1
            send(eth(KM,0x800,ip4(PIP,ASSIGN,6,tcp(PIP,ASSIGN,T['sp'],8080,T['seq'],T['ack'],0x11),4)))
            T['st']=3; log('tcp: response',repr(T['resp']))
# ipv6
def v6cs(src,dst,msg):
    ps=src+dst+struct.pack('!IBBBB',len(msg),0,0,0,58); return csum(ps+msg)
def v6pkt(dstmac,src,dst,msg,hop=255):
    m=msg[:2]+struct.pack('!H',v6cs(src,dst,msg[:2]+b'\0\0'+msg[4:]))+msg[4:]
    h=struct.pack('!IHBB',0x60000000,len(m),58,hop)+src+dst
    return eth(dstmac,0x86dd,h+m)
PLL=bytes([0xfe,0x80,0,0,0,0,0,0,0x00,0,0,0xff,0xfe,0,0,1])
def v6_in(f):
    h=f[14:]; src=h[8:24]; dst=h[24:40]; m=h[40:]
    if m[0]==135 and m[8:24]==PLL:
        na=bytes([136,0,0,0,0x60,0,0,0])+PLL+bytes([2,1])+PM
        send(v6pkt(f[6:12],PLL,src,na)); log('v6: NS for peer -> NA')
    elif m[0]==128 and dst==PLL:
        send(v6pkt(f[6:12],PLL,src,bytes([129,0,0,0])+m[4:],64)); log('v6: echo request -> reply')
    elif m[0]==136: log('v6: NA from kernel',src.hex()); 
    elif m[0]==129: log('v6: echo reply from kernel')
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
    r,_,_=select.select([s],[],[],0.2)
    if r:
        f=s.recv(2000)
        if len(f)<14: continue
        ty=struct.unpack('!H',f[12:14])[0]
        if ty==0x806 and f[20:22]==b'\0\1': arp_reply(f)
        elif ty==0x806: KM=f[6:12]; 
        elif ty==0x800:
            KM=f[6:12]
            if f[23]==17 and f[14+20+2:14+20+4]==b'\0\x43': dhcp(f)
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
    if dhcp_done and KM and T['st']==0 and now-last>1.0:
        last=now; T['st']=1; T['sp']+=1; T['seq']=1000
        send(eth(KM,0x800,ip4(PIP,ASSIGN,6,tcp(PIP,ASSIGN,T['sp'],8080,1000,0,0x02))))
        log('tcp: SYN sent')
    elif T['st']==1 and now-last>2.0:
        last=now; send(eth(KM,0x800,ip4(PIP,ASSIGN,6,tcp(PIP,ASSIGN,T['sp'],8080,1000,0,0x02)))); log('tcp: SYN retry')
    if dhcp_done and KM and now-vlast>3.0:
        vlast=now; KLL=kernel_ll(KM); v6_probe(); log('v6: probe sent to',KLL.hex())
log('end')
