from pathlib import Path
import shutil, struct, subprocess

root=Path(__file__).resolve().parents[1]
kernel=root/'build/kernel.elf'; disk=root/'storage/disk.img'; user=root/'build/user_init.elf'
required=['drivers/virtio_mmio.cpp','drivers/virtio_net.cpp','net/netstack.cpp','process/user.cpp','process/elf.cpp','boot/exceptions.S','process/scheduler.cpp','drivers/fb.cpp','drivers/font.cpp']
for name in required: assert (root/name).is_file(), f'missing {name}'
assert kernel.is_file() and kernel.stat().st_size>0
assert user.is_file() and user.stat().st_size>0
assert disk.is_file() and disk.stat().st_size==32768*512
kb=kernel.read_bytes(); assert kb[:4]==b'\x7fELF' and kb[4]==2 and kb[5]==1
assert struct.unpack_from('<H',kb,18)[0]==183
# user ELF
ub=user.read_bytes(); assert ub[:4]==b'\x7fELF' and ub[4]==2 and ub[5]==1
assert struct.unpack_from('<H',ub,18)[0]==183
entry=struct.unpack_from('<Q',ub,24)[0]; phoff=struct.unpack_from('<Q',ub,32)[0]; phentsize,phnum=struct.unpack_from('<HH',ub,54)
assert phentsize==56 and 0<phnum<=8 and entry==0x47000000
found_x=False
entry_exec=False
load_ranges=[]
for i in range(phnum):
    off=phoff+i*phentsize
    p_type,p_flags=struct.unpack_from('<II',ub,off)
    vaddr=struct.unpack_from('<Q',ub,off+16)[0]
    filesz=struct.unpack_from('<Q',ub,off+32)[0]
    memsz=struct.unpack_from('<Q',ub,off+40)[0]
    if p_type==1:
        end=vaddr+memsz
        load_ranges.append((vaddr,end,p_flags))
        if p_flags&1 and vaddr==0x47000000: found_x=True
        if p_flags&1 and vaddr <= entry < end: entry_exec=True
assert found_x
assert entry_exec, 'ELF entry is not inside an executable PT_LOAD'

# nano must be a real AArch64 user ELF with the same safe PT_LOAD layout.
nano=root/'build/nano.elf'
assert nano.is_file() and nano.stat().st_size>0
nb=nano.read_bytes(); assert nb[:4]==b'\x7fELF' and nb[4]==2 and nb[5]==1
assert struct.unpack_from('<H',nb,18)[0]==183
nentry=struct.unpack_from('<Q',nb,24)[0]; nphoff=struct.unpack_from('<Q',nb,32)[0]; nphentsize,nphnum=struct.unpack_from('<HH',nb,54)
assert nphentsize==56 and 0< nphnum <= 8 and nentry==0x47000000
nranges=[]; nexec=False; nentry_exec=False
for i in range(nphnum):
    off=nphoff+i*nphentsize
    pt,pf=struct.unpack_from('<II',nb,off)
    va=struct.unpack_from('<Q',nb,off+16)[0]
    fs=struct.unpack_from('<Q',nb,off+32)[0]
    ms=struct.unpack_from('<Q',nb,off+40)[0]
    if pt==1:
        assert fs<=ms and ms>0
        assert (va & 0xFFF)==0
        nranges.append((va,va+ms,pf))
        if pf&1 and va==0x47000000: nexec=True
        if pf&1 and va<=nentry<va+ms: nentry_exec=True
assert nexec and nentry_exec
for i,(a0,a1,_) in enumerate(nranges):
    for j,(b0,b1,_) in enumerate(nranges):
        if i<j: assert not (a0<b1 and b0<a1), 'nano PT_LOAD ranges overlap'

# Userspace entry must quiesce timer IRQs; normal exec is not allowed to enable them.
user_src=(root/'process/user.cpp').read_text()
assert 'timer_stop();' in user_src and 'interrupt_timer_line_disable();' in user_src and 'interrupt_disable();' in user_src
assert 'interrupt_enable();' not in user_src
for i,(a0,a1,_) in enumerate(load_ranges):
    for j,(b0,b1,_) in enumerate(load_ranges):
        if i<j:
            assert not (a0<b1 and b0<a1), 'overlapping PT_LOAD ranges'
# vectors via nm / llvm-nm
nm_tool = shutil.which('nm') or shutil.which('llvm-nm')
assert nm_tool, 'nm/llvm-nm is required for release verification'
data=subprocess.check_output([nm_tool,'-n',str(kernel)],text=True)
syms={}
for line in data.splitlines():
    q=line.split()
    if len(q)>=3 and q[2] in ('__vectors_start','__vectors_end'): syms[q[2]]=int(q[0],16)
assert '__vectors_start' in syms and '__vectors_end' in syms
assert syms['__vectors_start']%2048==0
assert syms['__vectors_end']-syms['__vectors_start']==2048
# no unresolved symbols
u=subprocess.run([nm_tool,'-u',str(kernel)],text=True,capture_output=True)
assert not u.stdout.strip(), 'unresolved symbols:\n'+u.stdout
# superblock
sb=disk.read_bytes()[:512]; vals=struct.unpack_from('<10I',sb,0); total=struct.unpack_from('<Q',sb,40)[0]
assert vals[0]==0x4D4B4653 and vals[1]==1 and vals[2]==512 and vals[3]==128 and vals[4]==64 and vals[9]==25 and total==32768
print('RELEASE STATIC CHECK: PASS')
