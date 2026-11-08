/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ulibc.h"
/* Minimal run-time dynamic linker: loads ET_DYN AArch64 objects with mmap,
   applies RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT relocations, resolves imports
   against the host program (libc table + dl_register) and other loaded
   libraries, then makes code pages read+execute (W^X) with mprotect. */
extern "C" {

struct Elf64_Ehdr { unsigned char ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Elf64_Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Elf64_Shdr { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; };
struct Elf64_Sym { uint32_t name; unsigned char info, other; uint16_t shndx; uint64_t value, size; };
struct Elf64_Rela { uint64_t offset, info; int64_t addend; };

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SHT_DYNSYM 11
#define SHT_RELA 4
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027

struct HostSym { const char* name; void* addr; };
#define DL_HOST_MAX 64
static HostSym host_extra[DL_HOST_MAX];
static int host_extra_n = 0;
static const char* dl_err = 0;

#define H(x) { #x, (void*)x }
static const HostSym host_libc[] = {
    H(printf), H(puts), H(putchar), H(snprintf), H(sprintf), H(dprintf), H(malloc), H(free), H(calloc), H(realloc),
    H(memcpy), H(memset), H(memmove), H(memcmp), H(strlen), H(strcmp), H(strncmp), H(strcpy), H(strncpy),
    H(strcat), H(strchr), H(strrchr), H(strstr), H(strdup), H(atoi), H(strtol), H(abs), H(qsort), H(rand),
    H(getpid), H(write), H(read), H(open), H(close), H(sleep_ms), H(clock_ms), H(exit), { "errno", (void*)&errno }
};

int dl_register(const char* name, void* addr)
{
    if (host_extra_n >= DL_HOST_MAX) return -1;
    host_extra[host_extra_n].name = name; host_extra[host_extra_n].addr = addr; ++host_extra_n;
    return 0;
}

struct DlLib
{
    int used;
    char path[64];
    uint8_t* base;
    size_t map_size;
    Elf64_Sym* syms; size_t nsyms; const char* strs;
};
#define DL_MAX 8
static DlLib libs[DL_MAX];

static void* find_in_lib(DlLib* l, const char* name)
{
    for (size_t i = 1; i < l->nsyms; ++i)
        if (l->syms[i].shndx != 0 && (l->syms[i].info & 0xF) != 0 && strcmp(l->strs + l->syms[i].name, name) == 0)
            return l->base + l->syms[i].value;
    return 0;
}

static void* resolve(const char* name)
{
    for (size_t i = 0; i < sizeof(host_libc) / sizeof(host_libc[0]); ++i)
        if (strcmp(host_libc[i].name, name) == 0) return host_libc[i].addr;
    for (int i = 0; i < host_extra_n; ++i)
        if (strcmp(host_extra[i].name, name) == 0) return host_extra[i].addr;
    for (int i = 0; i < DL_MAX; ++i)
        if (libs[i].used) { void* p = find_in_lib(&libs[i], name); if (p) return p; }
    return 0;
}

static void* fail(const char* msg) { dl_err = msg; return 0; }

void* dlopen(const char* path, int flags)
{
    (void)flags;
    dl_err = 0;
    for (int i = 0; i < DL_MAX; ++i)
        if (libs[i].used && strcmp(libs[i].path, path) == 0) return &libs[i];
    DlLib* lib = 0;
    for (int i = 0; i < DL_MAX; ++i) if (!libs[i].used) { lib = &libs[i]; break; }
    if (!lib) return fail("dlopen: too many libraries");

    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail("dlopen: cannot open file");
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size < (long)sizeof(Elf64_Ehdr) || size > 262144) { close(fd); return fail("dlopen: bad file size"); }
    uint8_t* img = (uint8_t*)malloc((size_t)size);
    if (!img) { close(fd); return fail("dlopen: out of memory"); }
    long got = 0;
    while (got < size) { long n = read(fd, img + got, (size_t)(size - got)); if (n <= 0) break; got += n; }
    close(fd);
    if (got != size) { free(img); return fail("dlopen: short read"); }

    Elf64_Ehdr* eh = (Elf64_Ehdr*)img;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 2 || eh->type != 3 || eh->machine != 183)
    { free(img); return fail("dlopen: not an AArch64 shared object"); }

    Elf64_Phdr* ph = (Elf64_Phdr*)(img + eh->phoff);
    uint64_t top = 0;
    for (int i = 0; i < eh->phnum; ++i)
        if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > top) top = ph[i].vaddr + ph[i].memsz;
    if (!top) { free(img); return fail("dlopen: no loadable segments"); }
    size_t map_size = (size_t)((top + 4095) & ~4095ULL);
    uint8_t* base = (uint8_t*)mmap(0, map_size, PROT_READ | PROT_WRITE, 0, -1, 0);
    if (base == MAP_FAILED) { free(img); return fail("dlopen: mmap failed"); }

    for (int i = 0; i < eh->phnum; ++i)
        if (ph[i].type == PT_LOAD && ph[i].filesz)
            memcpy(base + ph[i].vaddr, img + ph[i].offset, (size_t)ph[i].filesz);

    Elf64_Shdr* sh = (Elf64_Shdr*)(img + eh->shoff);
    Elf64_Sym* syms = 0; size_t nsyms = 0; const char* strs = 0;
    for (int i = 0; i < eh->shnum; ++i)
        if (sh[i].type == SHT_DYNSYM)
        {
            syms = (Elf64_Sym*)(base + sh[i].addr);
            nsyms = (size_t)(sh[i].size / sizeof(Elf64_Sym));
            strs = (const char*)(base + sh[sh[i].link].addr);
        }
    /* The section contents were not part of a PT_LOAD copy if they live outside; copy-safe fallback. */
    if (syms)
    {
        for (int i = 0; i < eh->shnum; ++i)
            if (sh[i].type == SHT_DYNSYM)
            {
                Elf64_Sym* dst = (Elf64_Sym*)malloc((size_t)sh[i].size);
                const char* sstr = (const char*)(img + sh[sh[i].link].offset);
                size_t slen = (size_t)sh[sh[i].link].size;
                char* dstr = (char*)malloc(slen);
                if (!dst || !dstr) { free(img); munmap(base, map_size); return fail("dlopen: out of memory"); }
                memcpy(dst, img + sh[i].offset, (size_t)sh[i].size);
                memcpy(dstr, sstr, slen);
                syms = dst; strs = dstr;
            }
    }

    for (int i = 0; i < eh->shnum; ++i)
    {
        if (sh[i].type != SHT_RELA) continue;
        Elf64_Rela* r = (Elf64_Rela*)(img + sh[i].offset);
        size_t n = (size_t)(sh[i].size / sizeof(Elf64_Rela));
        for (size_t k = 0; k < n; ++k)
        {
            uint32_t type = (uint32_t)(r[k].info & 0xFFFFFFFFu);
            uint32_t symi = (uint32_t)(r[k].info >> 32);
            uint64_t* where = (uint64_t*)(base + r[k].offset);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)base + (uint64_t)r[k].addend; continue; }
            if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT)
            {
                uint64_t sval;
                if (syms && symi < nsyms && syms[symi].shndx != 0) sval = (uint64_t)base + syms[symi].value;
                else
                {
                    void* p = (syms && symi < nsyms) ? resolve(strs + syms[symi].name) : 0;
                    if (!p) { free(img); munmap(base, map_size); return fail("dlopen: unresolved symbol"); }
                    sval = (uint64_t)p;
                }
                *where = type == R_AARCH64_JUMP_SLOT ? sval : sval + (uint64_t)r[k].addend;
                continue;
            }
            free(img); munmap(base, map_size);
            return fail("dlopen: unsupported relocation");
        }
    }

    /* W^X: executable segments become read+execute only after relocation. */
    for (int i = 0; i < eh->phnum; ++i)
        if (ph[i].type == PT_LOAD && (ph[i].flags & PF_X))
        {
            uint64_t s = ph[i].vaddr & ~4095ULL, e = (ph[i].vaddr + ph[i].memsz + 4095) & ~4095ULL;
            if (mprotect(base + s, (size_t)(e - s), PROT_READ | PROT_EXEC) != 0) { free(img); return fail("dlopen: mprotect failed"); }
        }
    free(img);

    lib->used = 1; strncpy(lib->path, path, sizeof(lib->path) - 1); lib->path[sizeof(lib->path) - 1] = 0;
    lib->base = base; lib->map_size = map_size; lib->syms = syms; lib->nsyms = nsyms; lib->strs = strs;
    return lib;
}

void* dlsym(void* handle, const char* name)
{
    DlLib* l = (DlLib*)handle;
    if (!l || !l->used) { dl_err = "dlsym: invalid handle"; return 0; }
    void* p = find_in_lib(l, name);
    if (!p) dl_err = "dlsym: symbol not found";
    return p;
}

int dlclose(void* handle)
{
    DlLib* l = (DlLib*)handle;
    if (!l || !l->used) return -1;
    munmap(l->base, l->map_size);
    free(l->syms); free((void*)l->strs);
    l->used = 0;
    return 0;
}

const char* dlerror(void) { const char* e = dl_err; dl_err = 0; return e; }

} /* extern "C" */
