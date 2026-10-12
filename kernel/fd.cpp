#include "fd.h"
#include "fs.h"
#include "uart.h"

#define FD_KIND_NONE 0
#define FD_KIND_STDIN 1
#define FD_KIND_STDOUT 2
#define FD_KIND_STDERR 3
#define FD_KIND_FILE 4
#define FD_KIND_PIPE_READ 5
#define FD_KIND_PIPE_WRITE 6

#define FD_SEEK_SET 0
#define FD_SEEK_CUR 1
#define FD_SEEK_END 2

#define FD_PROCESS_TABLES 32
#define FD_OPEN_FILES 128
#define FD_PIPE_MAX 8
#define FD_PIPE_CAPACITY 4096

struct FdEntry
{
    int used;
    int object;
};

struct FdTable
{
    int used;
    int owner_pid;
    FdEntry entries[FD_MAX_DESCRIPTORS];
};

struct OpenFile
{
    int used;
    int permanent;
    int kind;
    int refs;
    uint32_t flags;
    int cwd;
    uint64_t offset;
    int pipe_id;
    char path[128];
};

struct PipeState
{
    int used;
    int readers;
    int writers;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t size;
    uint8_t data[FD_PIPE_CAPACITY];
};

static FdTable tables[FD_PROCESS_TABLES];
static OpenFile open_files[FD_OPEN_FILES];
static PipeState pipes[FD_PIPE_MAX];
static int active_table = -1;
static uint8_t file_buffer[FD_FILE_MAX];

static int string_length(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static void copy_string(char* dst, const char* src, int cap)
{
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src)
        while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void print_u64(uint64_t value)
{
    char digits[24];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value && n < (int)sizeof(digits))
    {
        digits[n++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (n) uart_putc(digits[--n]);
}

static uint32_t access_mode(uint32_t flags) { return flags & 3U; }
static int can_read(uint32_t flags)
{
    uint32_t mode = access_mode(flags);
    return mode == FD_O_RDONLY || mode == FD_O_RDWR;
}
static int can_write(uint32_t flags)
{
    uint32_t mode = access_mode(flags);
    return mode == FD_O_WRONLY || mode == FD_O_RDWR;
}

static int valid_table(int index)
{
    return index >= 0 && index < FD_PROCESS_TABLES && tables[index].used;
}

static int find_table(int pid)
{
    for (int i = 0; i < FD_PROCESS_TABLES; ++i)
        if (tables[i].used && tables[i].owner_pid == pid) return i;
    return -1;
}

static void clear_descriptor(FdTable* table, int descriptor)
{
    if (!table || descriptor < 0 || descriptor >= FD_MAX_DESCRIPTORS) return;
    table->entries[descriptor].used = 0;
    table->entries[descriptor].object = -1;
}

static void install_standard_descriptors(FdTable* table)
{
    if (!table) return;
    for (int i = 0; i < 3; ++i)
    {
        table->entries[i].used = 1;
        table->entries[i].object = i;
    }
}

static int create_table(int pid)
{
    if (pid < 0) return -1;
    int existing = find_table(pid);
    if (existing >= 0) return existing;

    for (int i = 0; i < FD_PROCESS_TABLES; ++i)
    {
        if (tables[i].used) continue;
        tables[i].used = 1;
        tables[i].owner_pid = pid;
        for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d)
            clear_descriptor(&tables[i], d);
        install_standard_descriptors(&tables[i]);
        return i;
    }
    return -1;
}

static FdTable* current_table()
{
    return valid_table(active_table) ? &tables[active_table] : 0;
}

static int descriptor_valid_in(FdTable* table, int descriptor)
{
    if (!table || descriptor < 0 || descriptor >= FD_MAX_DESCRIPTORS ||
        !table->entries[descriptor].used) return 0;
    int object = table->entries[descriptor].object;
    return object >= 0 && object < FD_OPEN_FILES && open_files[object].used;
}

static int free_descriptor(FdTable* table, int minimum)
{
    if (!table) return -1;
    for (int i = minimum; i < FD_MAX_DESCRIPTORS; ++i)
        if (!table->entries[i].used) return i;
    return -1;
}

static int free_object()
{
    for (int i = 3; i < FD_OPEN_FILES; ++i)
        if (!open_files[i].used) return i;
    return -1;
}

static int free_pipe()
{
    for (int i = 0; i < FD_PIPE_MAX; ++i)
        if (!pipes[i].used) return i;
    return -1;
}

static void clear_object(int object)
{
    if (object < 3 || object >= FD_OPEN_FILES) return;
    open_files[object].used = 0;
    open_files[object].permanent = 0;
    open_files[object].kind = FD_KIND_NONE;
    open_files[object].refs = 0;
    open_files[object].flags = 0;
    open_files[object].cwd = -1;
    open_files[object].offset = 0;
    open_files[object].pipe_id = -1;
    open_files[object].path[0] = '\0';
}

static void maybe_destroy_pipe(int pipe_id)
{
    if (pipe_id < 0 || pipe_id >= FD_PIPE_MAX || !pipes[pipe_id].used) return;
    if (pipes[pipe_id].readers == 0 && pipes[pipe_id].writers == 0)
    {
        pipes[pipe_id].used = 0;
        pipes[pipe_id].read_pos = 0;
        pipes[pipe_id].write_pos = 0;
        pipes[pipe_id].size = 0;
    }
}

static void release_object(int object)
{
    if (object < 0 || object >= FD_OPEN_FILES || !open_files[object].used) return;
    OpenFile* file = &open_files[object];
    if (file->permanent) return;
    if (file->refs > 0) --file->refs;
    if (file->refs > 0) return;

    int pipe_id = file->pipe_id;
    if (pipe_id >= 0 && pipe_id < FD_PIPE_MAX && pipes[pipe_id].used)
    {
        if (file->kind == FD_KIND_PIPE_READ && pipes[pipe_id].readers > 0)
            --pipes[pipe_id].readers;
        if (file->kind == FD_KIND_PIPE_WRITE && pipes[pipe_id].writers > 0)
            --pipes[pipe_id].writers;
    }
    clear_object(object);
    maybe_destroy_pipe(pipe_id);
}

static void close_entry(FdTable* table, int descriptor)
{
    if (!descriptor_valid_in(table, descriptor))
    {
        clear_descriptor(table, descriptor);
        return;
    }
    int object = table->entries[descriptor].object;
    clear_descriptor(table, descriptor);
    release_object(object);
}

static int attach_object(FdTable* table, int object, int minimum)
{
    if (!table || object < 0 || object >= FD_OPEN_FILES || !open_files[object].used)
        return -1;
    int descriptor = free_descriptor(table, minimum);
    if (descriptor < 0) return -24;
    table->entries[descriptor].used = 1;
    table->entries[descriptor].object = object;
    if (!open_files[object].permanent) ++open_files[object].refs;
    return descriptor;
}

void fd_init()
{
    for (int i = 0; i < FD_PROCESS_TABLES; ++i)
    {
        tables[i].used = 0;
        tables[i].owner_pid = -1;
        for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d)
        {
            tables[i].entries[d].used = 0;
            tables[i].entries[d].object = -1;
        }
    }
    for (int i = 0; i < FD_OPEN_FILES; ++i) clear_object(i);
    for (int i = 0; i < FD_PIPE_MAX; ++i)
    {
        pipes[i].used = 0;
        pipes[i].readers = pipes[i].writers = 0;
        pipes[i].read_pos = pipes[i].write_pos = pipes[i].size = 0;
    }

    /* Standard streams are immortal shared endpoints; each process table has
       its own descriptor slots referring to these stateless objects. */
    for (int i = 0; i < 3; ++i)
    {
        open_files[i].used = 1;
        open_files[i].permanent = 1;
        open_files[i].kind = i == 0 ? FD_KIND_STDIN : (i == 1 ? FD_KIND_STDOUT : FD_KIND_STDERR);
        open_files[i].refs = 0;
        open_files[i].flags = i == 0 ? FD_O_RDONLY : FD_O_WRONLY;
        open_files[i].cwd = -1;
        open_files[i].offset = 0;
        open_files[i].pipe_id = -1;
        open_files[i].path[0] = '\0';
    }
    active_table = create_table(0);
}

int fd_set_process(int pid)
{
    if (pid < 0) return -1;
    int index = find_table(pid);
    if (index < 0) index = create_table(pid);
    if (index < 0) return -24;
    active_table = index;
    return 0;
}

int fd_current_process()
{
    FdTable* table = current_table();
    return table ? table->owner_pid : -1;
}

void fd_close_all()
{
    FdTable* table = current_table();
    if (!table) return;
    for (int i = 0; i < FD_MAX_DESCRIPTORS; ++i)
        close_entry(table, i);
    install_standard_descriptors(table);
}

int fd_close_all_for(int pid)
{
    if (fd_set_process(pid) != 0) return -24;
    fd_close_all();
    return 0;
}

void fd_release_process(int pid)
{
    if (pid == 0) return; /* Keep the kernel descriptor table permanent. */
    int index = find_table(pid);
    if (index < 0) return;
    FdTable* table = &tables[index];
    for (int i = 0; i < FD_MAX_DESCRIPTORS; ++i)
        close_entry(table, i);
    table->used = 0;
    table->owner_pid = -1;
    if (active_table == index)
    {
        active_table = find_table(0);
        if (active_table < 0) active_table = create_table(0);
    }
}

int fd_open(const char* path, uint32_t flags, int cwd)
{
    FdTable* table = current_table();
    if (!table) return -24;
    if (!path || !path[0] || string_length(path) >= (int)sizeof(open_files[0].path))
        return -22;
    if ((flags & ~(FD_O_CREAT | FD_O_TRUNC | FD_O_APPEND | 3U)) != 0 ||
        access_mode(flags) == 3U)
        return -22;
    if ((flags & FD_O_TRUNC) && !can_write(flags)) return -22;

    int descriptor = free_descriptor(table, 3);
    if (descriptor < 0) return -24;
    int object = free_object();
    if (object < 0) return -24;

    int id = fs_resolve(path, cwd);
    if (id < 0)
    {
        if (!(flags & FD_O_CREAT)) return -2;
        id = fs_touch(path, cwd);
        if (id < 0) return -5;
    }
    if (fs_get_type(id) != FS_FILE) return -21;
    if (can_read(flags) && fs_check_access(id, FS_ACCESS_READ) != 0) return -13;
    if (can_write(flags) && fs_check_access(id, FS_ACCESS_WRITE) != 0) return -13;

    if (flags & FD_O_TRUNC)
    {
        static const uint8_t empty = 0;
        if (fs_write_data(path, cwd, &empty, 0) != 0) return -5;
    }

    OpenFile* file = &open_files[object];
    file->used = 1;
    file->permanent = 0;
    file->kind = FD_KIND_FILE;
    file->refs = 1;
    file->flags = flags;
    file->cwd = cwd;
    file->offset = 0;
    file->pipe_id = -1;
    copy_string(file->path, path, (int)sizeof(file->path));
    if (flags & FD_O_APPEND) file->offset = fs_file_size(id);

    table->entries[descriptor].used = 1;
    table->entries[descriptor].object = object;
    return descriptor;
}

int fd_close(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    close_entry(table, descriptor);
    return 0;
}

int fd_dup(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    return attach_object(table, table->entries[descriptor].object, 0);
}

int fd_dup2(int old_descriptor, int new_descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, old_descriptor)) return -9;
    if (new_descriptor < 0 || new_descriptor >= FD_MAX_DESCRIPTORS) return -9;
    if (old_descriptor == new_descriptor) return new_descriptor;

    int object = table->entries[old_descriptor].object;
    if (table->entries[new_descriptor].used) close_entry(table, new_descriptor);
    table->entries[new_descriptor].used = 1;
    table->entries[new_descriptor].object = object;
    if (!open_files[object].permanent) ++open_files[object].refs;
    return new_descriptor;
}

int fd_pipe(int descriptors[2])
{
    FdTable* table = current_table();
    if (!table || !descriptors) return -14;
    int first = free_descriptor(table, 3);
    if (first < 0) return -24;
    int second = -1;
    for (int i = first + 1; i < FD_MAX_DESCRIPTORS; ++i)
        if (!table->entries[i].used) { second = i; break; }
    if (second < 0) return -24;

    int pipe_id = free_pipe();
    if (pipe_id < 0) return -24;
    int read_object = free_object();
    if (read_object < 0) return -24;
    /* Reserve the second object distinctly before publishing descriptors. */
    open_files[read_object].used = 1;
    int write_object = free_object();
    open_files[read_object].used = 0;
    if (write_object < 0 || write_object == read_object) return -24;

    PipeState* pipe = &pipes[pipe_id];
    pipe->used = 1;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->read_pos = pipe->write_pos = pipe->size = 0;

    OpenFile* rd = &open_files[read_object];
    rd->used = 1; rd->permanent = 0; rd->kind = FD_KIND_PIPE_READ;
    rd->refs = 1; rd->flags = FD_O_RDONLY; rd->cwd = -1; rd->offset = 0;
    rd->pipe_id = pipe_id; rd->path[0] = '\0';
    OpenFile* wr = &open_files[write_object];
    wr->used = 1; wr->permanent = 0; wr->kind = FD_KIND_PIPE_WRITE;
    wr->refs = 1; wr->flags = FD_O_WRONLY; wr->cwd = -1; wr->offset = 0;
    wr->pipe_id = pipe_id; wr->path[0] = '\0';

    table->entries[first].used = 1;
    table->entries[first].object = read_object;
    table->entries[second].used = 1;
    table->entries[second].object = write_object;
    descriptors[0] = first;
    descriptors[1] = second;
    return 0;
}

int64_t fd_read(int descriptor, void* buffer, uint64_t length)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    if (length > FD_IO_MAX) return -22;
    if (length == 0) return 0;
    if (!buffer) return -14;

    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (!can_read(entry->flags)) return -9;

    if (entry->kind == FD_KIND_STDIN)
    {
        uint8_t* out = (uint8_t*)buffer;
        uint64_t got = 0;
        while (got < length)
        {
            int c = uart_try_getc();
            if (c < 0) break;
            out[got++] = (uint8_t)c;
        }
        return (int64_t)got;
    }
    if (entry->kind == FD_KIND_PIPE_READ)
    {
        int pipe_id = entry->pipe_id;
        if (pipe_id < 0 || pipe_id >= FD_PIPE_MAX || !pipes[pipe_id].used) return -5;
        PipeState* pipe = &pipes[pipe_id];
        if (pipe->size == 0) return pipe->writers == 0 ? 0 : -11;
        uint64_t count = length < pipe->size ? length : pipe->size;
        uint8_t* out = (uint8_t*)buffer;
        for (uint64_t i = 0; i < count; ++i)
        {
            out[i] = pipe->data[pipe->read_pos];
            pipe->read_pos = (pipe->read_pos + 1U) % FD_PIPE_CAPACITY;
        }
        pipe->size -= (uint32_t)count;
        return (int64_t)count;
    }
    if (entry->kind != FD_KIND_FILE) return -9;

    uint64_t size = 0;
    int rc = fs_read_file(entry->path, entry->cwd, file_buffer,
                          sizeof(file_buffer), &size);
    if (rc != 0) return rc == -3 ? -27 : rc;
    if (entry->offset >= size) return 0;

    uint64_t available = size - entry->offset;
    uint64_t count = length < available ? length : available;
    uint8_t* out = (uint8_t*)buffer;
    for (uint64_t i = 0; i < count; ++i)
        out[i] = file_buffer[entry->offset + i];
    entry->offset += count;
    return (int64_t)count;
}

int64_t fd_write(int descriptor, const void* buffer, uint64_t length)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    if (length > FD_IO_MAX) return -22;
    if (length == 0) return 0;
    if (!buffer) return -14;

    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (!can_write(entry->flags)) return -9;

    if (entry->kind == FD_KIND_STDOUT || entry->kind == FD_KIND_STDERR)
    {
        const char* bytes = (const char*)buffer;
        for (uint64_t i = 0; i < length; ++i) uart_putc(bytes[i]);
        return (int64_t)length;
    }
    if (entry->kind == FD_KIND_PIPE_WRITE)
    {
        int pipe_id = entry->pipe_id;
        if (pipe_id < 0 || pipe_id >= FD_PIPE_MAX || !pipes[pipe_id].used) return -5;
        PipeState* pipe = &pipes[pipe_id];
        if (pipe->readers == 0) return -32;
        uint64_t available = FD_PIPE_CAPACITY - pipe->size;
        if (available == 0) return -11;
        uint64_t count = length < available ? length : available;
        const uint8_t* input = (const uint8_t*)buffer;
        for (uint64_t i = 0; i < count; ++i)
        {
            pipe->data[pipe->write_pos] = input[i];
            pipe->write_pos = (pipe->write_pos + 1U) % FD_PIPE_CAPACITY;
        }
        pipe->size += (uint32_t)count;
        return (int64_t)count;
    }
    if (entry->kind != FD_KIND_FILE) return -9;

    uint64_t old_size = 0;
    int exists = fs_read_file_for_update(entry->path, entry->cwd, file_buffer,
                                         sizeof(file_buffer), &old_size);
    if (exists != 0) return exists == -2 ? -2 : (exists == -3 ? -27 : -5);

    if (entry->flags & FD_O_APPEND) entry->offset = old_size;
    if (entry->offset > FD_FILE_MAX || length > FD_FILE_MAX - entry->offset)
        return -27;

    uint64_t end = entry->offset + length;
    if (entry->offset > old_size)
        for (uint64_t i = old_size; i < entry->offset; ++i)
            file_buffer[i] = 0;

    const uint8_t* input = (const uint8_t*)buffer;
    for (uint64_t i = 0; i < length; ++i)
        file_buffer[entry->offset + i] = input[i];

    uint64_t new_size = end > old_size ? end : old_size;
    int write_rc = fs_write_data(entry->path, entry->cwd, file_buffer, new_size);
    if (write_rc != 0) return write_rc;

    entry->offset = end;
    return (int64_t)length;
}

int64_t fd_seek(int descriptor, int64_t offset, int whence)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (entry->kind != FD_KIND_FILE) return -29;

    int id = fs_resolve(entry->path, entry->cwd);
    if (id < 0 || fs_get_type(id) != FS_FILE) return -2;
    uint64_t size = fs_file_size(id);

    int64_t base = 0;
    if (whence == FD_SEEK_SET) base = 0;
    else if (whence == FD_SEEK_CUR) base = (int64_t)entry->offset;
    else if (whence == FD_SEEK_END) base = (int64_t)size;
    else return -22;

    if ((offset > 0 && base > 0x7FFFFFFFFFFFFFFFLL - offset) ||
        (offset < 0 && base < (-0x7FFFFFFFFFFFFFFFLL - 1LL) - offset))
        return -22;
    int64_t target = base + offset;
    if (target < 0 || (uint64_t)target > FD_FILE_MAX) return -22;

    entry->offset = (uint64_t)target;
    return target;
}

void fd_list()
{
    FdTable* table = current_table();
    uart_puts("FD   TYPE       MODE       OFFSET  PATH\r\n");
    if (!table) { uart_puts("(no descriptor table)\r\n"); return; }
    for (int i = 0; i < FD_MAX_DESCRIPTORS; ++i)
    {
        if (!descriptor_valid_in(table, i)) continue;
        OpenFile* entry = &open_files[table->entries[i].object];
        print_u64((uint64_t)i); uart_puts("    ");
        switch (entry->kind)
        {
            case FD_KIND_STDIN: uart_puts("stdin      "); break;
            case FD_KIND_STDOUT: uart_puts("stdout     "); break;
            case FD_KIND_STDERR: uart_puts("stderr     "); break;
            case FD_KIND_FILE: uart_puts("file       "); break;
            case FD_KIND_PIPE_READ: uart_puts("pipe-read  "); break;
            case FD_KIND_PIPE_WRITE: uart_puts("pipe-write "); break;
            default: uart_puts("unknown    "); break;
        }
        if (entry->kind == FD_KIND_FILE)
        {
            uint32_t mode = access_mode(entry->flags);
            uart_puts(mode == FD_O_RDONLY ? "read      " :
                      mode == FD_O_WRONLY ? "write     " : "read/write");
        }
        else uart_puts("-         ");
        uart_puts(" "); print_u64(entry->offset); uart_puts("  ");
        uart_puts(entry->path[0] ? entry->path : "-"); uart_puts("\r\n");
    }
}
