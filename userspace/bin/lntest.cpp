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

#include "../libc/ulibc.h"
/* lntest: exercises symlink/link/readlink/lstat/realpath syscalls (60-64) from userspace */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    char buf[256]; struct RioStat st;
    mkdir("/lt", 0755);
    FILE* f = fopen("/lt/real.txt", "w"); fputs("hello", f); fclose(f);
    chk("symlink", symlink("/lt/real.txt", "/lt/sl") == 0);
    chk("symlink-dup-fails", symlink("/lt/real.txt", "/lt/sl") != 0);
    long n = readlink("/lt/sl", buf, sizeof(buf)); buf[n > 0 ? n : 0] = 0;
    chk("readlink", n == 12 && !strcmp(buf, "/lt/real.txt"));
    chk("lstat-type", lstat_path("/lt/sl", &st) == 0 && st.type == 2);
    chk("stat-follows", stat_path("/lt/sl", &st) == 0 && st.type == 0 && st.size == 5);
    f = fopen("/lt/sl", "r"); buf[0] = 0; if (f) { fgets(buf, 16, f); fclose(f); }
    chk("open-through-link", !strcmp(buf, "hello"));
    chk("relative-symlink", symlink("real.txt", "/lt/rel") == 0 && stat_path("/lt/rel", &st) == 0 && st.size == 5);
    chk("dir-symlink", symlink("/lt", "/lt/self") == 0);
    chk("realpath-dir", realpath("/lt/self/self/rel", buf) && !strcmp(buf, "/lt/real.txt"));
    chk("link-hard", link("/lt/real.txt", "/lt/hard") == 0 && stat_path("/lt/hard", &st) == 0 && st.size == 5);
    symlink("/lt/loopb", "/lt/loopa"); symlink("/lt/loopa", "/lt/loopb");
    chk("loop-detected", stat_path("/lt/loopa", &st) != 0 && realpath("/lt/loopa", buf) == NULL);
    symlink("/lt/nothing", "/lt/dangling");
    chk("dangling-lstat-ok", lstat_path("/lt/dangling", &st) == 0 && st.type == 2);
    chk("dangling-stat-fails", stat_path("/lt/dangling", &st) != 0);
    chk("unlink-link-only", unlink("/lt/sl") == 0 && stat_path("/lt/real.txt", &st) == 0);
    chk("readlink-nonlink", readlink("/lt/real.txt", buf, sizeof(buf)) < 0);
    printf("lntest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
