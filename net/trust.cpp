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

#include "tls.h"
#include "x509.h"
#include "fs.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"

/* Trust store: /etc/ssl/certs.pem (shipped on the disk image) plus an optional extra CA file
   (--cacert). The store is rebuilt only when one of the files changed (size or path). */
static const char* TRUST_FILE = "/etc/ssl/certs.pem";
static char g_extra[160];
static uint64_t g_main_size = ~0ULL, g_extra_size = ~0ULL;
static char g_loaded_extra[160];
static int g_count = 0;

void tls_set_extra_ca(const char* path)
{
    uint32_t i = 0; if (path) while (path[i] && i < sizeof(g_extra) - 1) { g_extra[i] = path[i]; ++i; }
    g_extra[i] = 0;
}

static int file_size(const char* path, uint64_t* sz)
{
    int id = fs_resolve(path, vfs_cwd());
    if (id < 0 || fs_get_type(id) != FS_FILE) return -1;
    *sz = fs_file_size(id); return 0;
}

int tls_trust_reload()
{
    uint64_t ms = 0, es = 0; int have_main = file_size(TRUST_FILE, &ms) == 0;
    int have_extra = g_extra[0] && file_size(g_extra, &es) == 0;
    if (!have_main) ms = 0;
    if (!have_extra) es = 0;
    int same_extra = true; for (int i = 0; i < (int)sizeof(g_extra); ++i) { if (g_extra[i] != g_loaded_extra[i]) { same_extra = false; break; } if (!g_extra[i]) break; }
    if (g_main_size == ms && g_extra_size == es && same_extra && g_count > 0) return g_count;
    int total = 0; int replace = 1;
    for (int pass = 0; pass < 2; ++pass)
    {
        const char* path = pass == 0 ? TRUST_FILE : g_extra;
        uint64_t sz = pass == 0 ? ms : es;
        if (pass == 0 ? !have_main : !have_extra) continue;
        if (sz == 0 || sz > 520000ULL) continue;
        uint8_t* buf = (uint8_t*)kmalloc(sz + 1);
        if (!buf) break;
        uint64_t got = 0;
        if (fs_read_file(path, vfs_cwd(), buf, sz, &got) == 0)
        {
            int n = x509_trust_load_pem(buf, (uint32_t)got, !replace);
            if (n > 0) { total += n; replace = 0; }
        }
        kfree(buf);
    }
    if (replace) x509_trust_load_pem((const uint8_t*)"", 0, 0);
    g_main_size = ms; g_extra_size = es; g_count = total;
    for (int i = 0; i < (int)sizeof(g_extra); ++i) { g_loaded_extra[i] = g_extra[i]; if (!g_extra[i]) break; }
    return total;
}
