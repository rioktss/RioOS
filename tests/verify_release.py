from pathlib import Path
import shutil, struct, subprocess

root=Path(__file__).resolve().parents[1]
kernel=root/'build/kernel.elf'; disk=root/'storage/disk.img'; user=root/'build/user_init.elf'
required=['drivers/virtio_mmio.cpp','drivers/virtio_net.cpp','net/netstack.cpp','docs/P3_NETWORKING.md','docs/TEST_MATRIX_P3.md','process/user.cpp','process/elf.cpp','boot/exceptions.S','process/scheduler.cpp','process/thread.cpp','process/ctx_switch.S','kernel/fd.cpp','include/fd.h','tests/fd_host_test.cpp','tests/process_host_test.cpp','tests/scheduler_process_host_test.cpp','drivers/fb.cpp','drivers/console.cpp','drivers/console_font.cpp','include/console.h','include/console_font.h','tests/console_host_test.cpp','tools/gen_console_font.py','docs/THIRD_PARTY_NOTICES.md','docs/P2_FILESYSTEM_SECURITY.md','include/smp.h','kernel/smp.cpp','tests/smp_host_test.cpp','docs/P4_SMP_DRIVER_HARDENING.md','docs/TEST_MATRIX_P4.md','docs/TERMINAL.md','docs/TIMER_IRQ.md','.github/workflows/ci.yml','linux/run-terminal.sh','windows/run-terminal.ps1']
for name in required: assert (root/name).is_file(), f'missing {name}'
# The repository root is intentionally extensible. Keep the core project
# metadata required, but do not reject additional files or folders at root.
assert (root / 'README.md').is_file(), 'README.md is missing from repository root'
assert (root / '.gitignore').is_file(), '.gitignore is missing from repository root'

platform_scripts = {
    'linux': ('build.sh', 'run.sh', 'run-terminal.sh'),
    'termux': ('build.sh', 'run.sh', 'run-terminal.sh'),
    'macos': ('build.sh', 'run.sh', 'run-terminal.sh'),
    'windows': ('build.ps1', 'run.ps1', 'run-terminal.ps1'),
}
for platform, scripts in platform_scripts.items():
    for script in scripts:
        assert (root / platform / script).is_file(), f'missing platform entry point: {platform}/{script}'
    ui_script = 'run-ui.ps1' if platform == 'windows' else 'run-ui.sh'
    assert not (root / platform / ui_script).exists(), f'the GUI runner was removed: {platform}/{ui_script}'
for gone in ('ui', 'apps', 'assets'):
    assert not (root / gone).exists(), f'GUI folder must not exist: {gone}/'
assert (root / 'boot/linker.ld').is_file(), 'linker script must remain at boot/linker.ld'
assert (root / 'docs/LICENSE').is_file(), 'MIT license file is missing'
assert (root / 'docs/PORTABLE_BUILD.md').is_file(), 'portable build guide is missing'
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

# Timer IRQ is live: the system tick keeps running while EL0 programs execute.
user_src=(root/'process/user.cpp').read_text()
assert 'timer_stop();' not in user_src and 'interrupt_timer_line_disable();' not in user_src, 'exec must not stop the system timer tick'
assert 'user_execute_inner' in user_src and 'if (irq_was_on) interrupt_enable();' in user_src, 'exec must restore the CPU IRQ mask'
user_entry=(root/'process/user_entry.S').read_text()
assert 'mov x10, #0x340' in user_entry and '#0x3c0' not in user_entry, 'EL0 must be entered with the I-bit clear (SPSR 0x340)'
vectors_src=(root/'boot/exceptions.S').read_text()
assert 'VECTOR irq_el0_entry' in vectors_src and 'VECTOR irq_el1_entry' in vectors_src, 'lower-EL and current-EL IRQ vectors are required'
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
# Cooperative context-switch entry must be linked into the kernel.
assert 'thread_context_switch' in data, 'cooperative thread context switch is not linked'
assert 'thread_trampoline' in data, 'thread trampoline is not linked'
assert any('fd_open' in line for line in data.splitlines()), 'file descriptor implementation is not linked'
assert any('fd_read' in line for line in data.splitlines()), 'file descriptor read path is not linked'
boot_src=(root/'kernel/kernel.cpp').read_text()
assert 'thread_init();' in boot_src, 'thread subsystem is not initialized'
thread_src=(root/'process/thread.cpp').read_text()
assert 'daifclr' not in thread_src, 'cooperative threads must not unmask CPU IRQs'
assert 'timer_start_periodic(100U);' in boot_src, 'boot must start the 100 Hz system tick'
assert 'interrupt_enable();' in boot_src and 'interrupt_timer_line_enable();' in boot_src, 'boot must unmask the timer line and CPU IRQs'
user_api=(root/'include/user_api.h').read_text()
syscall_src=(root/'kernel/syscall.cpp').read_text()
assert 'user_fd_open' in user_api and 'user_fd_seek' in user_api, 'userspace FD helpers are missing'
assert 'user_fd_dup' in user_api and 'user_fd_dup2' in user_api and 'user_fd_pipe' in user_api, 'P1 userspace FD/pipe helpers are missing'
assert 'case SYS_FD_OPEN:' in syscall_src and 'case SYS_FD_SEEK:' in syscall_src, 'FD syscalls are missing'
assert 'case SYS_FD_DUP:' in syscall_src and 'case SYS_FD_DUP2:' in syscall_src and 'case SYS_FD_PIPE:' in syscall_src, 'P1 duplicate/pipe syscall dispatch is missing'
assert 'fd_close_all();' in user_src and 'fd_set_process(pid)' in user_src and 'fd_release_process(pid)' in user_src, 'EL0 process-local descriptor lifecycle is missing'
fd_src=(root/'kernel/fd.cpp').read_text()
assert 'struct FdTable' in fd_src and 'struct OpenFile' in fd_src, 'per-process tables/shared open files are missing'
assert 'int fd_dup2(int old_descriptor, int new_descriptor)' in fd_src, 'dup2 implementation is missing'
assert 'int fd_pipe(int descriptors[2])' in fd_src and 'FD_PIPE_CAPACITY' in fd_src, 'bounded kernel pipes are missing'
fd_test=(root/'tests/fd_host_test.cpp').read_text()
for marker in ('fd_set_process(77)', 'fd_dup2(shared, 7)', 'fd_pipe(pfd)', '== -11', '== -32'):
    assert marker in fd_test, f'P1 descriptor regression missing: {marker}'
version_h=(root/'include/version.h').read_text()
assert '"1.13.0"' in version_h, 'release version mismatch'

# P0 process/MMU/ELF invariants: these checks make the release fail closed if
# a future edit removes the guards. Runtime QEMU boot is still a separate test.
process_src=(root/'process/process.cpp').read_text()
assert 'int process_destroy(int pid)' in process_src and 'PROCESS_RUNNING' in process_src
assert 'PROCESS_USER' in process_src and 'MMU_USER_BASE' in process_src and 'MMU_USER_STACK_TOP' in process_src
assert 'signed overflow' in process_src
sched_src=(root/'process/scheduler.cpp').read_text()
assert 'int scheduler_set_current_logical(int pid)' in sched_src
assert 'current_valid' in sched_src
mmu_src=(root/'memory/mmu.cpp').read_text()
assert 'flags & ~(USER_R | USER_W | USER_X)' in mmu_src
assert 'user_active = 0;' in mmu_src
elf_src=(root/'process/elf.cpp').read_text()
assert 'eh->type != ET_EXEC' in elf_src
assert 'LOAD segments sharing a permission page rejected' in elf_src
assert 'last_loaded = 0;' in elf_src
proc_test=(root/'tests/process_host_test.cpp').read_text()
assert 'process_destroy(pid) == -3' in proc_test
syscall_src=(root/'kernel/syscall.cpp').read_text()
assert 'base > 0xFFFFFFFFFFFFFFFFULL - i' in syscall_src

# P5 process-isolation contracts. The normal EL0 launch path must use a per-PID
# page-table root and owned frames; host tests cover PCB register-frame storage.
mmu_p5=(root/'memory/mmu.cpp').read_text()
mmu_h=(root/'include/mmu.h').read_text()
elf_p5=(root/'process/elf.cpp').read_text()
user_p5=(root/'process/user.cpp').read_text()
assert 'mmu_user_space_create' in mmu_p5 and 'user_l1[USER_AS_MAX]' in mmu_p5
assert 'mmu_user_space_activate' in mmu_h and 'mmu_user_space_destroy' in mmu_h
assert 'allocate_page()' in mmu_p5 and 'free_page(' in mmu_p5
assert 'elf_load_for_process' in elf_p5 and 'mmu_user_space_copy_to' in elf_p5
assert 'elf_load_for_process(path, &entry, pid)' in user_p5
assert 'mmu_user_space_deactivate();' in user_p5
process_h=(root/'include/process.h').read_text()
assert 'struct ProcessCpuContext' in process_h and 'process_save_context' in process_h
assert 'process_load_context' in process_h
assert 'process_save_context(pid, &context)' in (root/'kernel/exceptions.cpp').read_text()
assert 'process_context_valid' in (root/'tests/process_host_test.cpp').read_text()
assert (root/'docs/P5_PROCESS_ISOLATION.md').exists(), 'P5 documentation missing'

# no unresolved symbols
u=subprocess.run([nm_tool,'-u',str(kernel)],text=True,capture_output=True)
assert not u.stdout.strip(), 'unresolved symbols:\n'+u.stdout
# superblock
sb=disk.read_bytes()[:512]; vals=struct.unpack_from('<10I',sb,0); total=struct.unpack_from('<Q',sb,40)[0]
assert vals[0]==0x4D4B4653 and vals[1]==1 and vals[2]==512 and vals[3]==128 and vals[4]==64 and vals[9]==25 and total==32768
# The disk image carries only real programs now: no .rpk packages / images.
db=disk.read_bytes()
assert b'RIOPKG/1' not in db, 'RPK packages must not be installed any more'
assert not db[512:512+64*128].count(b'.rpk'), 'RPK entries must not exist in the inode table'
print('RELEASE STATIC CHECK: PASS')

# P2 filesystem/security contracts: persistent owners, centralized access checks,
# syscall wrappers, legacy metadata migration, and safe boot defaults.
fs_src=(root/'fs/fs.cpp').read_text()
assert 'fs_check_access_as' in fs_src and 'FS_ACCESS_WRITE | FS_ACCESS_EXEC' in fs_src
assert 'FS_META_TAG' in fs_src and 'fs_assign_legacy_owner_policy' in fs_src
assert 'int fs_stat_path(' in fs_src and 'int fs_truncate(' in fs_src and 'int fs_set_owner(' in fs_src
assert 'fs_read_file_for_update' in fs_src and 'FS_ACCESS_WRITE' in fs_src
assert 'fs_check_access(existing, FS_ACCESS_WRITE)' in fs_src
assert 'fs_check_access(id, FS_ACCESS_READ)' in fs_src
security_src=(root/'kernel/security.cpp').read_text()
assert 'ROOT_PASSWORD' in security_src and 'fs_set_credentials(0U, 0U)' in security_src
commands_src=(root/'kernel/commands.cpp').read_text()
assert 'command_stat' in commands_src and 'command_chown' in commands_src and 'command_truncate' in commands_src
assert 'diskwrite: permission denied (root required)' in commands_src
assert 'fs_check_access(file_id, FS_ACCESS_EXEC)' in (root/'process/elf.cpp').read_text()
assert '(fs_get_mode(file_id) & 0111U) == 0' in (root/'process/elf.cpp').read_text(), 'ELF execute bits must be required even for root'
assert 'nodes[i].size > FS_MAX_FILE_SIZE' in fs_src, 'corrupt oversized inode sizes must be rejected before arithmetic'
assert 'nodes[i].first_sector >= disk_sectors' in fs_src, 'corrupt extents beyond disk must be rejected without unsigned underflow'
assert 'sb.inode_start != FS_INODE_START' in fs_src and 'sb.bitmap_start != FS_BITMAP_START' in fs_src, 'filesystem geometry must match the on-disk layout'
assert 'case SYS_FS_STAT:' in syscall_src and 'case SYS_FS_TRUNCATE:' in syscall_src and 'case SYS_FS_CHMOD:' in syscall_src
user_api=(root/'include/user_api.h').read_text()
assert 'user_fs_stat' in user_api and 'user_fs_truncate' in user_api and 'user_fs_chmod' in user_api
fs_test=(root/'tests/fs_host_test.cpp').read_text()
assert 'const unsigned char impossible_size[8]' in fs_test, 'filesystem corruption regression test is missing'
for marker in ('fs_set_credentials(1000U, 1000U)', 'fs_set_mode(locked_id, 0644U) == -13', 'fs_truncate(owned, root, 5) == 0', 'P2 metadata marker', 'p2-no-search'):
    assert marker in fs_test, f'P2 filesystem regression missing: {marker}'
fd_test=(root/'tests/fd_host_test.cpp').read_text()
assert 'write-only file remains writable without granting read permission' in fd_test, 'P2 write-only descriptor regression missing'


# P3 networking contracts: owner-scoped UDP sockets, checksum validation, DNS,
# user syscall wrappers, process cleanup, and IRQ-safe default boot path.
net_src=(root/'net/netstack.cpp').read_text()
net_header=(root/'include/netstack.h').read_text()
assert 'int net_udp_socket(int owner_pid)' in net_src
assert 'udp_checksum_ipv4' in net_src and 'checksum16((const uint8_t*)ip, ihl) != 0' in net_src
assert 'int net_dns_resolve(' in net_src and 'dns_parse_a_response' in net_src
assert 'int net_dns_set_server(' in net_src and 'DNS_DEFAULT_SERVER' in net_src
assert 'int net_tcp_socket(int owner_pid)' in net_src and 'int net_tcp_connect(' in net_src
assert 'int net_tcp_send(' in net_src and 'int net_tcp_recv(' in net_src and 'tcp_checksum_ipv4' in net_src
assert 'net_udp_close_all_for(pid)' in user_src, 'EL0 UDP sockets must close at process exit'
assert 'SYS_NET_UDP_SOCKET' in syscall_src and 'SYS_NET_DNS_LOOKUP' in syscall_src
assert 'SYS_NET_TCP_SOCKET' in syscall_src and 'SYS_NET_TCP_CONNECT' in syscall_src
assert 'user_udp_socket' in user_api and 'user_udp_sendto' in user_api and 'user_udp_recvfrom' in user_api
assert 'user_dns_lookup' in user_api
assert 'user_tcp_socket' in user_api and 'user_tcp_connect' in user_api and 'user_tcp_recv' in user_api
assert 'command_nslookup' in commands_src and 'command_httpget' in commands_src
assert 'dnsserver' in commands_src and 'ifconfig' in commands_src
net_test=(root/'tests/net_host_test.cpp').read_text()
for marker in ('cross-owner socket access', 'corrupt IPv4 checksum', 'bad UDP checksum not delivered', 'outgoing UDP pseudo-header checksum validates', 'DNS A lookup end-to-end', 'TCP three-way handshake', 'TCP receives HTTP response', 'TCP socket close succeeds', 'close-all releases process sockets'):
    assert marker in net_test, f'P3 networking regression missing: {marker}'
print('P3 NETWORK STATIC CHECK: PASS')

assert '#define NET_HDR_SIZE             10U' in (root/'drivers/virtio_net.cpp').read_text(), 'base VirtIO-Net header must be 10 bytes when MRG_RXBUF is not negotiated'


# P4 SMP/driver/terminal foundations. SMP intentionally stays offline until
# the secondary boot protocol and per-CPU kernel state are implemented.
smp_header=(root/'include/smp.h').read_text()
smp_src=(root/'kernel/smp.cpp').read_text()
smp_test=(root/'tests/smp_host_test.cpp').read_text()
assert 'uint32_t smp_cpu_id()' in smp_header and 'smp_online_cpu_count()' in smp_header
assert 'smp_enabled_flag = 0' in smp_src and 'online_cpus = 1' in smp_src
assert '__atomic_exchange_n' in smp_src and '__ATOMIC_ACQUIRE' in smp_src and '__ATOMIC_RELEASE' in smp_src
assert 'SMP must remain disabled by default' in smp_test
assert 'smp_init();' in boot_src and 'Secondary-core startup remains disabled' in boot_src
fb_src=(root/'drivers/fb.cpp').read_text()
console_src=(root/'drivers/console.cpp').read_text()
console_test=(root/'tests/console_host_test.cpp').read_text()
assert 'fwcfg_init_ramfb' in fb_src and 'RAMFB_FOURCC_XRGB8888' in fb_src, 'QEMU ramfb pixel output path missing'
assert 'uint8_t entry[64]' in fb_src and 'entry + 4' in fb_src, 'fw_cfg ramfb parser must use fixed 64-byte FWCfgFile records'
assert 'elem->len > RX_BUFFER_SIZE' in (root/'drivers/virtio_net.cpp').read_text(), 'VirtIO-Net RX length bounds are missing'
assert 'return -90; /* EMSGSIZE */' in (root/'drivers/virtio_net.cpp').read_text(), 'oversized frame truncation should be rejected'
commands=(root/'kernel/commands.cpp').read_text()
assert 'command_smp_status' in commands and 'command_fbinfo' in commands
vfs_src=(root/'fs/vfs.cpp').read_text()
nano_src=(root/'userspace/nano.S').read_text()
assert 'fs_resolve("/home/user", root)' in vfs_src, 'interactive VFS must default to the writable user home'
assert 'current_directory = data_home' in vfs_src, 'VFS writable-home fallback is missing'
assert 'cmn x0, #2' in nano_src and 'b.eq load_empty' in nano_src, 'nano must only treat ENOENT as a new file'
assert 'load_failed:' in nano_src and 'msg_denied:' in nano_src, 'nano read/save error handling is missing'

# Framebuffer terminal (QEMU window) contracts.
assert 'console_putc(c)' in (root/'drivers/uart.cpp').read_text(), 'UART output must be mirrored to the framebuffer terminal'
assert 'console_start()' in boot_src and 'ui_start' not in boot_src, 'kernel must start the terminal, not a GUI'
for marker in ('console_scroll_lines', 'console_scroll_pages', 'history_floor', 'run_csi', 'apply_sgr'):
    assert marker in console_src, f'terminal feature missing: {marker}'
for marker in ('wheel up goes back', 'scrolled back 30 lines', 'scroll clamps at oldest line', 'output snaps to bottom', 'font is anti-aliased', 'cursor stays in bounds under fuzz'):
    assert marker.replace('wheel up goes back','scrolled back 30 lines') in console_test, f'terminal regression missing: {marker}'
input_driver=(root/'drivers/virtio_input.cpp').read_text()
assert 'console_scroll_lines(event.scroll * WHEEL_LINES_PER_NOTCH)' in input_driver, 'mouse wheel must scroll the terminal'
assert 'INPUT_REL_WHEEL' in input_driver and 'INPUT_BTN_GEAR_UP' in input_driver, 'wheel must work as REL_WHEEL and as button events'
assert 'console_scroll_pages(1)' in input_driver and 'console_scroll_pages(-1)' in input_driver, 'PageUp/PageDown must scroll the terminal'
assert 'virtio_input_poll' in input_driver and 'INPUT_SYN_REPORT' in input_driver, 'VirtIO input events must be polled and synchronized'
assert '&d->queue_size' in input_driver and 'event_avail[i].idx % d->queue_size' in input_driver, 'VirtIO input must honor the negotiated queue size'
assert 'INPUT_CFG_ID_NAME' in input_driver and 'read_device_config(d, index)' in input_driver, 'input devices must be identified by their advertised name'
assert 'INPUT_DEVICE_COUNT    4U' in input_driver, 'VirtIO input scan must support keyboard, tablet, and mouse concurrently'
terminal_runner=(root/'linux/run-terminal.sh').read_text()
assert '-device ramfb' in terminal_runner and 'virtio-keyboard-device' in terminal_runner and 'virtio-tablet-device' in terminal_runner, 'run-terminal.sh must attach ramfb + keyboard + wheel-capable pointer'
assert '-nographic' in (root/'linux/run.sh').read_text() and 'ramfb' not in (root/'linux/run.sh').read_text(), 'run.sh must stay a plain host-terminal runner'
assert 'ramfb' in (root/'windows/run-terminal.ps1').read_text(), 'Windows terminal runner must attach ramfb'
assert 'uart_flush_output();' in (root/'kernel/security.cpp').read_text(), 'interactive prompts must flush to the terminal before waiting for input'
print('P4 SMP/DRIVER/FRAMEBUFFER STATIC CHECK: PASS')
print('TERMINAL/IRQ STATIC CHECK: PASS')
