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

#include "security.h"
#include "fs.h"
#include "uart.h"
#include "keyboard.h"
#include "string.h"

#include "sha256.h"
#include "timer.h"

/* Account database: /etc/passwd (root-only, mode 0600), one line per account:
     name:uid:gid:$rio$<iterations>$<salt hex>$<pbkdf2-hmac-sha256 hex>
   Passwords are never stored in clear text. */
#define ACCOUNT_MAX 8
#define PW_ITERATIONS 3000U

struct Account
{
    int used;
    char name[16];
    uint32_t uid, gid;
    uint32_t iterations;
    uint8_t salt[8];
    uint8_t hash[32];
};

static Account accounts[ACCOUNT_MAX];
static int current_account = -1;
static SecurityRole current_role = SECURITY_USER;
static int accounts_loaded = 0;
static uint32_t salt_counter = 0;

static const char* PASSWD_PATH = "/etc/passwd";

static void hex_encode(const uint8_t* d, int n, char* out)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; ++i) { out[i*2] = h[d[i] >> 4]; out[i*2+1] = h[d[i] & 15]; }
    out[n*2] = 0;
}

static int hex_val(char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1); }

static int hex_decode(const char* s, uint8_t* out, int n)
{
    for (int i = 0; i < n; ++i)
    {
        int hi = hex_val(s[i*2]), lo = hex_val(s[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return 0;
}

static void make_salt(uint8_t salt[8])
{
    uint64_t t = timer_millis() * 2654435761ULL + (++salt_counter) * 40503ULL;
    uint64_t cnt;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    t ^= cnt * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; ++i) { salt[i] = (uint8_t)(t >> (i * 8)); t = t * 6364136223846793005ULL + 1442695040888963407ULL; }
}

static void derive(const char* password, Account* a)
{
    pbkdf2_sha256(password, a->salt, 8, a->iterations, a->hash);
}

static int ct_equal(const uint8_t* x, const uint8_t* y, int n)
{
    uint8_t d = 0;
    for (int i = 0; i < n; ++i) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

static uint32_t saved_uid, saved_gid;
static void become_root() { saved_uid = fs_current_uid(); saved_gid = fs_current_gid(); fs_set_credentials(0U, 0U); }
static void restore_creds() { fs_set_credentials(saved_uid, saved_gid); }

static void put_dec(char*& p, uint32_t v)
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
}

static int save_accounts()
{
    static char buf[1024];
    char* p = buf;
    for (int i = 0; i < ACCOUNT_MAX; ++i)
    {
        if (!accounts[i].used) continue;
        for (const char* n = accounts[i].name; *n; ++n) *p++ = *n;
        *p++ = ':'; put_dec(p, accounts[i].uid);
        *p++ = ':'; put_dec(p, accounts[i].gid);
        const char* pre = ":$rio$"; while (*pre) *p++ = *pre++;
        put_dec(p, accounts[i].iterations);
        *p++ = '$';
        char hx[65];
        hex_encode(accounts[i].salt, 8, hx); for (char* q = hx; *q; ++q) *p++ = *q;
        *p++ = '$';
        hex_encode(accounts[i].hash, 32, hx); for (char* q = hx; *q; ++q) *p++ = *q;
        *p++ = '\n';
    }
    become_root();
    (void)fs_mkdir_p("/etc", fs_get_root());
    int rc = fs_write_data(PASSWD_PATH, fs_get_root(), (const uint8_t*)buf, (uint64_t)(p - buf));
    if (rc == 0) { (void)fs_set_mode_path(PASSWD_PATH, fs_get_root(), 0600U); (void)fs_set_owner_path(PASSWD_PATH, fs_get_root(), 0U, 0U); }
    restore_creds();
    return rc;
}

static int parse_u32(const char*& p, uint32_t* out)
{
    uint32_t v = 0; int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); ++p; any = 1; }
    *out = v; return any;
}

static int load_accounts()
{
    static uint8_t buf[1025];
    uint64_t size = 0;
    become_root();
    int rc = fs_read_file(PASSWD_PATH, fs_get_root(), buf, sizeof(buf) - 1, &size);
    restore_creds();
    if (rc != 0 || size == 0) return -1;
    buf[size] = 0;
    for (int i = 0; i < ACCOUNT_MAX; ++i) accounts[i].used = 0;
    const char* p = (const char*)buf;
    int n = 0;
    while (*p && n < ACCOUNT_MAX)
    {
        Account a; a.used = 1;
        int k = 0;
        while (*p && *p != ':' && *p != '\n' && k < 15) a.name[k++] = *p++;
        a.name[k] = 0;
        if (*p != ':') return -2;
        ++p;
        if (!parse_u32(p, &a.uid) || *p++ != ':') return -2;
        if (!parse_u32(p, &a.gid) || *p++ != ':') return -2;
        const char* pre = "$rio$";
        for (; *pre; ++pre) if (*p++ != *pre) return -2;
        if (!parse_u32(p, &a.iterations) || *p++ != '$' || a.iterations == 0) return -2;
        if (hex_decode(p, a.salt, 8) != 0) return -2;
        p += 16;
        if (*p++ != '$') return -2;
        if (hex_decode(p, a.hash, 32) != 0) return -2;
        p += 64;
        if (*p == '\n') ++p;
        accounts[n++] = a;
    }
    return n > 0 ? 0 : -3;
}

static int find_account(const char* name)
{
    for (int i = 0; i < ACCOUNT_MAX; ++i)
        if (accounts[i].used && str_equal(accounts[i].name, name)) return i;
    return -1;
}

static void add_account(const char* name, uint32_t uid, uint32_t gid, const char* password)
{
    int slot = -1;
    for (int i = 0; i < ACCOUNT_MAX; ++i) if (!accounts[i].used) { slot = i; break; }
    if (slot < 0) return;
    Account* a = &accounts[slot];
    a->used = 1; str_copy(a->name, name, sizeof(a->name));
    a->uid = uid; a->gid = gid; a->iterations = PW_ITERATIONS;
    make_salt(a->salt);
    derive(password, a);
}

static void select_account(int idx)
{
    current_account = idx;
    current_role = accounts[idx].uid == 0 ? SECURITY_ROOT : SECURITY_USER;
    fs_set_credentials(accounts[idx].uid, accounts[idx].gid);
}

uint32_t security_uid() { return current_account >= 0 ? accounts[current_account].uid : 1000U; }
uint32_t security_gid() { return current_account >= 0 ? accounts[current_account].gid : 1000U; }
void security_apply_fs_credentials() { fs_set_credentials(security_uid(), security_gid()); }

static int security_get_char()
{
    while (1)
    {
        /* Let a foreground pixel terminal paint password prompts before blocking. */
        uart_flush_output();
        if (uart_has_data())
            return (int)(unsigned char)uart_getc();

        if (keyboard_has_data())
            return (int)(unsigned char)keyboard_getc();

        asm volatile("yield");
    }
}

void security_init()
{
    for (int i = 0; i < ACCOUNT_MAX; ++i) accounts[i].used = 0;
    if (load_accounts() != 0)
    {
        for (int i = 0; i < ACCOUNT_MAX; ++i) accounts[i].used = 0;
        add_account("root", 0U, 0U, "123");     /* default passwords as before; change with `passwd` */
        add_account("user", 1000U, 1000U, "123");
        (void)save_accounts();
    }
    accounts_loaded = 1;
    int u = find_account("user");
    select_account(u >= 0 ? u : 0);
}

SecurityRole security_role() { return current_role; }
int security_is_root() { return security_uid() == 0U; }
const char* security_username() { return current_account >= 0 ? accounts[current_account].name : "user"; }
char security_prompt_char() { return security_is_root() ? '#' : '$'; }

int security_verify(const char* name, const char* password)
{
    int i = find_account(name);
    if (i < 0 || !password) return -1;
    Account tmp = accounts[i];
    derive(password, &tmp);
    return ct_equal(tmp.hash, accounts[i].hash, 32) ? 0 : -1;
}

int security_ids_of(const char* name, uint32_t* uid, uint32_t* gid)
{
    int i = find_account(name);
    if (i < 0) return -1;
    *uid = accounts[i].uid; *gid = accounts[i].gid; return 0;
}

int security_account_at(int index, char* name, uint32_t* uid, uint32_t* gid)
{
    if (!accounts_loaded) security_init();
    int seen = 0;
    for (int i = 0; i < ACCOUNT_MAX; ++i)
    {
        if (!accounts[i].used) continue;
        if (seen++ != index) continue;
        for (int k = 0; k < 16; ++k) name[k] = accounts[i].name[k];
        name[15] = 0;
        *uid = accounts[i].uid; *gid = accounts[i].gid;
        return 0;
    }
    return -1;
}

int security_login_as(const char* name, const char* password)
{
    if (security_verify(name, password) != 0) return -1;
    select_account(find_account(name));
    return 0;
}

int security_login_user(const char* password) { return security_login_as("user", password); }
int security_login_root(const char* password) { return security_login_as("root", password); }

void security_logout_to_user()
{
    int u = find_account("user");
    if (u >= 0) select_account(u);
}

int security_user_add(const char* name, const char* password)
{
    if (!security_is_root()) return -1;
    if (!name || !name[0] || str_len(name) > 15 || find_account(name) >= 0) return -2;
    for (const char* c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return -3;
    uint32_t uid = 1000U;
    for (int i = 0; i < ACCOUNT_MAX; ++i) if (accounts[i].used && accounts[i].uid >= uid) uid = accounts[i].uid + 1U;
    int before = 0; for (int i = 0; i < ACCOUNT_MAX; ++i) before += accounts[i].used;
    add_account(name, uid, uid, password);
    int after = 0; for (int i = 0; i < ACCOUNT_MAX; ++i) after += accounts[i].used;
    if (after == before) return -4;
    become_root();
    char home[40]; str_copy(home, "/home/", sizeof(home)); str_copy(home + 6, name, 30);
    if (fs_mkdir_p(home, fs_get_root()) >= 0 || fs_resolve(home, fs_get_root()) >= 0)
    {
        (void)fs_set_owner_path(home, fs_get_root(), uid, uid);
        (void)fs_set_mode_path(home, fs_get_root(), 0750U);
    }
    restore_creds();
    return save_accounts();
}

int security_user_del(const char* name)
{
    if (!security_is_root()) return -1;
    int i = find_account(name);
    if (i < 0) return -2;
    if (accounts[i].uid == 0U || i == current_account) return -3;
    accounts[i].used = 0;
    return save_accounts();
}

int security_set_password(const char* name, const char* old_password, const char* new_password)
{
    int i = find_account(name);
    if (i < 0 || !new_password || !new_password[0]) return -2;
    if (!security_is_root())
    {
        if (i != current_account) return -1;
        if (!old_password || security_verify(name, old_password) != 0) return -3;
    }
    make_salt(accounts[i].salt);
    accounts[i].iterations = PW_ITERATIONS;
    derive(new_password, &accounts[i]);
    return save_accounts();
}

void security_list_accounts()
{
    for (int i = 0; i < ACCOUNT_MAX; ++i)
    {
        if (!accounts[i].used) continue;
        uart_puts(accounts[i].name); uart_puts("  uid="); 
        char b[12]; int n = 0; uint32_t v = accounts[i].uid; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) uart_putc(b[--n]);
        uart_puts("  gid="); v = accounts[i].gid; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) uart_putc(b[--n]);
        uart_puts(i == current_account ? "  (current)\r\n" : "\r\n");
    }
}

int security_read_password(char* out, uint64_t cap)
{
    if (out == 0 || cap < 2)
        return -1;

    uint64_t length = 0;
    out[0] = '\0';

    while (1)
    {
        int c = security_get_char();

        if (c == '\r' || c == '\n')
        {
            out[length] = '\0';
            uart_puts("\r\n");
            return (int)length;
        }

        if (c == 8 || c == 127)
        {
            if (length > 0)
            {
                --length;
                out[length] = '\0';
                uart_puts("\b \b");
            }
            continue;
        }

        if (c >= 32 && c <= 126 && length + 1 < cap)
        {
            out[length++] = (char)c;
            out[length] = '\0';
            /* Do not echo the actual password. */
            uart_putc('*');
        }
    }
}
