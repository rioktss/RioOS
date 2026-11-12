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
/* posixtest: stat/dirent/glob/fnmatch, getopt, regex, termios, pwd, system info, random, futex/shm/sem across fork, epoll, timerfd,
   flock, chdir/umask, temp files, truncate/utime/fchmod, strptime, UTF-8, syslog */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

static int rx(const char* pat, int fl, const char* s, int so, int eo)     /* match must give [so,eo) */
{
    regex_t re; if (regcomp(&re, pat, fl) != 0) return 0;
    regmatch_t m[10]; int r = regexec(&re, s, 10, m, 0);
    int ok = so < 0 ? r == REG_NOMATCH : (r == 0 && m[0].rm_so == so && m[0].rm_eo == eo);
    regfree(&re); return ok;
}
static int rxsub(const char* pat, int fl, const char* s, int n, int so, int eo)
{
    regex_t re; if (regcomp(&re, pat, fl) != 0) return 0;
    regmatch_t m[10]; int r = regexec(&re, s, 10, m, 0); int ok = r == 0 && m[n].rm_so == so && m[n].rm_eo == eo;
    regfree(&re); return ok;
}
static int rxbad(const char* pat, int fl, int code) { regex_t re; int r = regcomp(&re, pat, fl); if (r == 0) regfree(&re); return r == code; }
static int has_name(const char* dir, const char* name) { DIR* d = opendir(dir); if (!d) return 0; struct dirent* e; int f = 0; while ((e = readdir(d))) if (!strcmp(e->d_name, name)) f = 1; closedir(d); return f; }
static int wr_file(const char* p, const char* s) { int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC); if (fd < 0) return -1; write(fd, s, strlen(s)); close(fd); return 0; }
static int ready(int fd, short ev, int ms) { struct pollfd p; p.fd = fd; p.events = ev; p.revents = 0; int r = poll(&p, 1, ms); return r == 1 ? p.revents : (r < 0 ? -1 : 0); }

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    mkdir("/tmp", 0777);

    /* ---- stat / fstat / attributes ---- */
    wr_file("/tmp/pt_a.txt", "hello world");
    { struct stat st; chk("stat-file", stat("/tmp/pt_a.txt", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 11);
      chk("stat-dir", stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode));
      errno = 0; chk("stat-enoent", stat("/tmp/nonexistent_zz", &st) == -1 && errno == ENOENT);
      int fd = open("/tmp/pt_a.txt", O_RDWR); struct stat s2;
      chk("fstat-file", fstat(fd, &s2) == 0 && s2.st_size == 11 && S_ISREG(s2.st_mode));
      chk("fchmod", fchmod(fd, 0640) == 0 && stat("/tmp/pt_a.txt", &st) == 0 && (st.st_mode & 0777) == 0640);
      chk("ftruncate-shrink", ftruncate(fd, 5) == 0 && fstat(fd, &s2) == 0 && s2.st_size == 5);
      chk("ftruncate-grow", ftruncate(fd, 20) == 0 && fstat(fd, &s2) == 0 && s2.st_size == 20);
      chk("fsync", fsync(fd) == 0);
      chk("futimens", ({ struct timespec ts[2] = {{0, UTIME_OMIT}, {1000000000L, 0}}; futimens(fd, ts) == 0 && fstat(fd, &s2) == 0 && s2.st_mtime == 1000000000L; }));
      close(fd);
      chk("truncate-path", truncate("/tmp/pt_a.txt", 3) == 0 && stat("/tmp/pt_a.txt", &st) == 0 && st.st_size == 3);
      struct utimbuf ub = {0, 1234567890L}; chk("utime", utime("/tmp/pt_a.txt", &ub) == 0 && stat("/tmp/pt_a.txt", &st) == 0 && st.st_mtime == 1234567890L);
      chk("fstat-stdin-tty", ({ struct stat t; fstat(0, &t) == 0 && S_ISCHR(t.st_mode); }));
      int pp[2]; pipe(pp); chk("fstat-pipe-fifo", fstat(pp[0], &st) == 0 && S_ISFIFO(st.st_mode)); close(pp[0]); close(pp[1]);
      int so = socket(AF_UNIX, SOCK_STREAM, 0); chk("fstat-socket", fstat(so, &st) == 0 && S_ISSOCK(st.st_mode)); close(so);
      chk("access-exist", access("/tmp/pt_a.txt", F_OK) == 0 && access("/tmp/pt_a.txt", R_OK | W_OK) == 0);
      errno = 0; chk("access-missing", access("/tmp/zz_none", F_OK) == -1 && errno == ENOENT);
      chk("O_EXCL-existing", ({ errno = 0; int f = open("/tmp/pt_a.txt", O_RDWR | O_CREAT | O_EXCL); int ok = f == -1 && errno == EEXIST; if (f >= 0) close(f); ok; }));
      chk("O_EXCL-new", ({ unlink("/tmp/pt_ex"); int f = open("/tmp/pt_ex", O_RDWR | O_CREAT | O_EXCL); int ok = f >= 0; if (f >= 0) close(f); unlink("/tmp/pt_ex"); ok; })); }

    /* ---- cwd / umask / relative paths ---- */
    { char cwd[128]; chk("getcwd-root", getcwd(cwd, sizeof cwd) != 0 && cwd[0] == '/');
      mkdir("/tmp/pt_dir", 0777); chk("chdir", chdir("/tmp/pt_dir") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/tmp/pt_dir"));
      chk("relative-create", wr_file("rel.txt", "r") == 0 && access("/tmp/pt_dir/rel.txt", F_OK) == 0);
      struct stat st; chk("relative-stat", stat("rel.txt", &st) == 0 && st.st_size == 1);
      errno = 0; chk("chdir-file-enotdir", chdir("/tmp/pt_a.txt") == -1 && errno == ENOTDIR);
      char small[3]; errno = 0; chk("getcwd-erange", getcwd(small, sizeof small) == 0 && errno == ERANGE);
      pid_t c = fork(); if (c == 0) { char b[128]; getcwd(b, sizeof b); _exit(!strcmp(b, "/tmp/pt_dir") ? 0 : 1); }
      int stt = 0; waitpid(c, &stt, 0); chk("cwd-inherited-by-fork", stt == 0);
      chk("chdir-back", chdir("/") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/"));
      mode_t old = umask(077); chk("umask-set-get", umask(old) == 077);
      unlink("/tmp/pt_dir/rel.txt"); rmdir("/tmp/pt_dir"); }

    /* ---- dirent / scandir / glob / fnmatch ---- */
    { mkdir("/tmp/pt_g", 0777); wr_file("/tmp/pt_g/a.c", "1"); wr_file("/tmp/pt_g/b.c", "2"); wr_file("/tmp/pt_g/c.h", "3"); wr_file("/tmp/pt_g/.hid", "4"); mkdir("/tmp/pt_g/sub", 0777); wr_file("/tmp/pt_g/sub/x.c", "5");
      chk("readdir-entries", has_name("/tmp/pt_g", "a.c") && has_name("/tmp/pt_g", "sub") && has_name("/tmp/pt_g", ".") && has_name("/tmp/pt_g", ".."));
      errno = 0; chk("opendir-enotdir", opendir("/tmp/pt_g/a.c") == 0 && errno == ENOTDIR);
      errno = 0; chk("opendir-enoent", opendir("/tmp/pt_nonexistent") == 0 && errno == ENOENT);
      struct dirent** list; int n = scandir("/tmp/pt_g", &list, 0, alphasort);
      chk("scandir-sorted", n == 7 && !strcmp(list[0]->d_name, ".") && !strcmp(list[2]->d_name, ".hid") && !strcmp(list[6]->d_name, "sub"));
      for (int i = 0; i < n; ++i) free(list[i]); free(list);
      glob_t g; chk("glob-star", glob("/tmp/pt_g/*.c", 0, 0, &g) == 0 && g.gl_pathc == 2 && !strcmp(g.gl_pathv[0], "/tmp/pt_g/a.c") && !strcmp(g.gl_pathv[1], "/tmp/pt_g/b.c") && g.gl_pathv[2] == 0); globfree(&g);
      chk("glob-hidden-skipped", glob("/tmp/pt_g/*", 0, 0, &g) == 0 && g.gl_pathc == 4); globfree(&g);
      chk("glob-dir-component", glob("/tmp/pt_*/sub/*.c", 0, 0, &g) == 0 && g.gl_pathc == 1 && !strcmp(g.gl_pathv[0], "/tmp/pt_g/sub/x.c")); globfree(&g);
      chk("glob-mark", glob("/tmp/pt_g/s*", GLOB_MARK, 0, &g) == 0 && g.gl_pathc == 1 && !strcmp(g.gl_pathv[0], "/tmp/pt_g/sub/")); globfree(&g);
      chk("glob-bracket-q", glob("/tmp/pt_g/[ab].?", 0, 0, &g) == 0 && g.gl_pathc == 2); globfree(&g);
      chk("glob-nomatch", glob("/tmp/pt_g/*.zzz", 0, 0, &g) == GLOB_NOMATCH);
      chk("glob-nocheck", glob("/tmp/pt_g/*.zzz", GLOB_NOCHECK, 0, &g) == 0 && g.gl_pathc == 1 && !strcmp(g.gl_pathv[0], "/tmp/pt_g/*.zzz")); globfree(&g);
      chk("fnmatch-basic", fnmatch("*.c", "foo.c", 0) == 0 && fnmatch("*.c", "foo.h", 0) == FNM_NOMATCH && fnmatch("f?o", "foo", 0) == 0);
      chk("fnmatch-bracket", fnmatch("[a-c]x", "bx", 0) == 0 && fnmatch("[!a-c]x", "bx", 0) != 0 && fnmatch("[[:digit:]]*", "7abc", 0) == 0);
      chk("fnmatch-pathname", fnmatch("*", "a/b", FNM_PATHNAME) != 0 && fnmatch("*", "a/b", 0) == 0 && fnmatch("a/*", "a/b", FNM_PATHNAME) == 0);
      chk("fnmatch-period-casefold", fnmatch("*", ".h", FNM_PERIOD) != 0 && fnmatch("ABC", "abc", FNM_CASEFOLD) == 0 && fnmatch("a\\*", "a*", 0) == 0);
      unlink("/tmp/pt_g/sub/x.c"); rmdir("/tmp/pt_g/sub"); unlink("/tmp/pt_g/a.c"); unlink("/tmp/pt_g/b.c"); unlink("/tmp/pt_g/c.h"); unlink("/tmp/pt_g/.hid"); rmdir("/tmp/pt_g"); }

    /* ---- temp files, basename/dirname ---- */
    { char t1[] = "/tmp/ptXXXXXX"; int fd = mkstemp(t1); chk("mkstemp", fd >= 0 && strlen(t1) == 13 && strcmp(t1, "/tmp/ptXXXXXX") != 0 && access(t1, F_OK) == 0);
      char t2[] = "/tmp/ptXXXXXX"; int fd2 = mkstemp(t2); chk("mkstemp-unique", fd2 >= 0 && strcmp(t1, t2) != 0);
      if (fd >= 0) close(fd); if (fd2 >= 0) close(fd2); unlink(t1); unlink(t2);
      char d1[] = "/tmp/ptdXXXXXX"; chk("mkdtemp", mkdtemp(d1) != 0 && ({ struct stat st; stat(d1, &st) == 0 && S_ISDIR(st.st_mode); })); rmdir(d1);
      char bad[] = "/tmp/noxs"; errno = 0; chk("mkstemp-einval", mkstemp(bad) == -1 && errno == EINVAL);
      char p1[] = "/usr/lib/x.c", p2[] = "/usr/lib/x.c", p3[] = "x.c", p4[] = "x.c", p5[] = "/", p6[] = "/";
      chk("basename-dirname", !strcmp(basename(p1), "x.c") && !strcmp(dirname(p2), "/usr/lib") && !strcmp(basename(p3), "x.c") && !strcmp(dirname(p4), ".") && !strcmp(basename(p5), "/") && !strcmp(dirname(p6), "/")); }

    /* ---- getopt / getopt_long ---- */
    { char* av[] = {(char*)"prog", (char*)"-a", (char*)"file1", (char*)"-bVAL", (char*)"-c", (char*)"cv", (char*)"file2", (char*)"--", (char*)"-z", 0};
      int ac = 9, a = 0, b = 0, ch; const char* bv = 0; const char* cv = 0; optind = 0; opterr = 0;
      while ((ch = getopt(ac, av, "ab:c:")) != -1) { if (ch == 'a') a = 1; else if (ch == 'b') { b = 1; bv = optarg; } else if (ch == 'c') cv = optarg; }
      chk("getopt-flags", a && b && bv && !strcmp(bv, "VAL") && cv && !strcmp(cv, "cv"));
      chk("getopt-permute", ac - optind == 3 && !strcmp(av[optind], "file1") && !strcmp(av[optind + 1], "file2") && !strcmp(av[optind + 2], "-z"));
      char* av2[] = {(char*)"prog", (char*)"-x", (char*)"-b", 0}; optind = 0; int e1 = getopt(3, av2, "ab:"); int o1 = optopt; int e2 = getopt(3, av2, "ab:");
      chk("getopt-errors", e1 == '?' && o1 == 'x' && e2 == '?' && optopt == 'b');
      char* av3[] = {(char*)"prog", (char*)"-b", 0}; optind = 0; chk("getopt-colon-missing", getopt(2, av3, ":b:") == ':');
      static int verbose = 0; struct option lo[] = {{"verbose", no_argument, &verbose, 1}, {"name", required_argument, 0, 'n'}, {"num", optional_argument, 0, 'N'}, {0, 0, 0, 0}};
      char* av4[] = {(char*)"prog", (char*)"--verbose", (char*)"--name=bob", (char*)"--na", (char*)"al", (char*)"--num=5", (char*)"rest", 0};
      optind = 0; int li = -1; int r1 = getopt_long(7, av4, "n:N::", lo, &li); const char* n1 = optarg;
      int r2 = getopt_long(7, av4, "n:N::", lo, &li); const char* n2 = optarg; int r3 = getopt_long(7, av4, "n:N::", lo, &li); const char* n3 = optarg; int r4 = getopt_long(7, av4, "n:N::", lo, &li);
      chk("getopt_long", r1 == 0 && verbose == 1 && r2 == 'n' && n1 == 0 && n2 && !strcmp(n2, "bob") && r3 == 'n' && n3 && !strcmp(n3, "al") && r4 == 'N' && optarg && !strcmp(optarg, "5") && getopt_long(7, av4, "n:N::", lo, &li) == -1 && optind == 6); }

    /* ---- regex ---- */
    chk("rx-literal", rx("abc", 0, "xxabcxx", 2, 5) && rx("abc", REG_EXTENDED, "ab", -1, 0));
    chk("rx-star-longest", rx("a*", 0, "aaab", 0, 3) && rx("b*", REG_EXTENDED, "aab", 0, 0));
    chk("rx-leftmost", rx("a+", REG_EXTENDED, "baaac", 1, 4));
    chk("rx-alt-longest", rx("a|ab|abc", REG_EXTENDED, "abcd", 0, 3) && rx("(a|ab)(c|bcd)", REG_EXTENDED, "abcd", 0, 4));
    chk("rx-bre-groups", rxsub("\\(ab\\)*c", 0, "ababc", 1, 2, 4) && rx("a\\{2,3\\}", 0, "aaaa", 0, 3));
    chk("rx-ere-interval", rx("a{2,3}", REG_EXTENDED, "aaaa", 0, 3) && rx("^(ab){2}$", REG_EXTENDED, "abab", 0, 4) && rx("^(ab){2}$", REG_EXTENDED, "ababab", -1, 0));
    chk("rx-backref", rx("(a+)b\\1", REG_EXTENDED, "aabaa", 0, 5) && rx("(a+)b\\1", REG_EXTENDED, "aaba", 1, 4) && rx("\\(.\\)\\1", 0, "abccd", 2, 4));
    chk("rx-anchors", rx("^abc$", REG_EXTENDED, "abc", 0, 3) && rx("^b", REG_EXTENDED, "ab", -1, 0) && rx("a$", REG_EXTENDED, "ba", 1, 2) && rx("a^b", 0, "a^b", 0, 3));
    chk("rx-bre-star-literal", rx("*a", 0, "*a", 0, 2) && rx("^*a", 0, "*a", 0, 2));
    chk("rx-bracket", rx("[a-c]+", REG_EXTENDED, "xxabcabd", 2, 7) && rx("[^a-c]+", REG_EXTENDED, "abxyc", 2, 4) && rx("[]a]+", REG_EXTENDED, "x]a]", 1, 4) && rx("[[:digit:]]+", REG_EXTENDED, "ab123c", 2, 5) && rx("[[:alpha:]_][[:alnum:]_]*", REG_EXTENDED, "9 foo_1 ", 2, 7));
    chk("rx-icase", rx("hello", REG_ICASE, "say HeLLo", 4, 9) && rx("[a-c]+", REG_EXTENDED | REG_ICASE, "xABC", 1, 4) && rx("(a)\\1", REG_EXTENDED | REG_ICASE, "aA", 0, 2));
    chk("rx-dot-newline", rx("a.b", 0, "a\nb", 0, 3) && rx("a.b", REG_NEWLINE, "a\nb", -1, 0) && rx("^b", REG_NEWLINE, "a\nb", 2, 3) && rx("a$", REG_NEWLINE, "a\nb", 0, 1));
    chk("rx-optional-plus", rx("colou?r", REG_EXTENDED, "my color", 3, 8) && rx("ab+c", REG_EXTENDED, "abbbc", 0, 5) && rx("a\\+", 0, "caab", 1, 3) && rx("a+", 0, "a+", 0, 2));
    chk("rx-empty-alt", rx("a|", REG_EXTENDED, "b", 0, 0) && rx("(|a)b", REG_EXTENDED, "ab", 0, 2));
    chk("rx-gnu-escapes", rx("\\bfoo\\b", REG_EXTENDED, "a foo b", 2, 5) && rx("\\<ab", REG_EXTENDED, "xab ab", 4, 6) && rx("\\w+", REG_EXTENDED, "  hi_1 ", 2, 6) && rx("\\s+", REG_EXTENDED, "a \t b", 1, 4));
    chk("rx-nested-star-no-hang", rx("(a*)*b", REG_EXTENDED, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", -1, 0) && rx("(a*)+$", REG_EXTENDED, "aaaa", 0, 4));
    chk("rx-pathological-linear", ({ char big[2048]; memset(big, 'a', 2000); big[2000] = 0; rx("(a|aa)*b", REG_EXTENDED, big, -1, 0) && rx("(a|aa)+$", REG_EXTENDED, big, 0, 2000); }));
    chk("rx-long-text", ({ static char t[4000]; memset(t, 'x', 3990); strcpy(t + 3990, "needle"); rx("needle", 0, t, 3990, 3996); }));
    chk("rx-submatches", rxsub("([a-z]+)@([a-z]+)\\.com", REG_EXTENDED, "mail bob@site.com now", 1, 5, 8) && rxsub("([a-z]+)@([a-z]+)\\.com", REG_EXTENDED, "mail bob@site.com now", 2, 9, 13));
    chk("rx-nosub-nmatch", ({ regex_t re; regcomp(&re, "a(b)c", REG_EXTENDED); int ok = re.re_nsub == 1 && regexec(&re, "abc", 0, 0, 0) == 0; regfree(&re); ok; }));
    chk("rx-notbol", ({ regex_t re; regcomp(&re, "^a", REG_EXTENDED); int ok = regexec(&re, "a", 0, 0, REG_NOTBOL) == REG_NOMATCH && regexec(&re, "a", 0, 0, 0) == 0; regfree(&re); ok; }));
    chk("rx-errors", rxbad("a(b", REG_EXTENDED, REG_EPAREN) && rxbad("a)", REG_EXTENDED, REG_EPAREN) && rxbad("[a", 0, REG_EBRACK) && rxbad("a\\", 0, REG_EESCAPE) && rxbad("\\(a\\)\\2", 0, REG_ESUBREG) && rxbad("a{3,1}", REG_EXTENDED, REG_BADBR) && rxbad("[b-a]", 0, REG_ERANGE) && rxbad("[[:nope:]]", 0, REG_ECTYPE) && rxbad("a{1", REG_EXTENDED, REG_EBRACE));
    chk("rx-regerror", ({ char m[64]; regex_t re; size_t l = regerror(REG_NOMATCH, &re, m, sizeof m); l > 1 && !strcmp(m, "No match"); }));
    chk("rx-dup-limit", rxbad("((a{255}){255}){255}", REG_EXTENDED, REG_ESPACE));

    /* ---- termios ---- */
    { struct termios t, raw; chk("tcgetattr", tcgetattr(0, &t) == 0 && (t.c_lflag & ICANON) && (t.c_lflag & ECHO) && t.c_cc[VMIN] == 1);
      raw = t; cfmakeraw(&raw); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
      chk("tcsetattr-raw", tcsetattr(0, TCSANOW, &raw) == 0 && ({ struct termios u; tcgetattr(0, &u) == 0 && !(u.c_lflag & ICANON) && !(u.c_lflag & ECHO) && u.c_cc[VMIN] == 0; }));
      char cb[4]; chk("raw-read-vmin0-returns-0", read(0, cb, 1) == 0);
      chk("tcsetattr-restore", tcsetattr(0, TCSANOW, &t) == 0 && ({ struct termios u; tcgetattr(0, &u) == 0 && (u.c_lflag & ICANON) && (u.c_lflag & ECHO); }));
      chk("isatty", isatty(0) == 1 && ({ int pp[2]; pipe(pp); errno = 0; int r = isatty(pp[0]) == 0 && errno == ENOTTY; close(pp[0]); close(pp[1]); r; }));
      errno = 0; int pp[2]; pipe(pp); chk("tcgetattr-pipe-enotty", tcgetattr(pp[0], &t) == -1 && errno == ENOTTY); close(pp[0]); close(pp[1]);
      struct winsize ws; chk("ioctl-winsz", ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0);
      chk("ttyname", ttyname(0) != 0);
      struct termios t2 = t; cfsetospeed(&t2, B115200); chk("cfspeed", cfgetospeed(&t2) == B115200); }

    /* ---- users and groups ---- */
    { struct passwd* pw = getpwnam("root"); chk("getpwnam-root", pw && pw->pw_uid == 0 && !strcmp(pw->pw_name, "root") && pw->pw_dir && pw->pw_shell);
      struct passwd* p2 = getpwuid(0); chk("getpwuid-0", p2 && !strcmp(p2->pw_name, "root"));
      chk("getpwnam-none", getpwnam("no_such_user_zz") == 0);
      struct passwd s; char buf[200]; struct passwd* res; chk("getpwuid_r", getpwuid_r(0, &s, buf, sizeof buf, &res) == 0 && res == &s && !strcmp(s.pw_name, "root"));
      chk("getpwnam_r-erange", getpwnam_r("root", &s, buf, 8, &res) == ERANGE);
      int cnt = 0; setpwent(); while (getpwent()) ++cnt; endpwent(); chk("getpwent-iterates", cnt >= 1);
      struct group* gr = getgrgid(0); chk("getgrgid-0", gr && !strcmp(gr->gr_name, "root") && gr->gr_gid == 0);
      chk("getgrnam", getgrnam("root") != 0 && getgrnam("no_such_group_zz") == 0);
      chk("getlogin", getlogin() != 0);
      gid_t gl[4]; chk("getgroups", getgroups(0, 0) == 1 && getgroups(4, gl) == 1); }

    /* ---- system information ---- */
    { struct utsname u; chk("uname", uname(&u) == 0 && !strcmp(u.sysname, "RioOS") && !strcmp(u.machine, "aarch64") && u.nodename[0]);
      char hn[64]; chk("gethostname", gethostname(hn, sizeof hn) == 0 && hn[0]);
      chk("sysconf", sysconf(_SC_PAGESIZE) == 4096 && sysconf(_SC_NPROCESSORS_ONLN) >= 1 && sysconf(_SC_PHYS_PAGES) > 1000 && sysconf(_SC_AVPHYS_PAGES) > 0 && sysconf(_SC_OPEN_MAX) == 24 && sysconf(_SC_CLK_TCK) == 100);
      errno = 0; chk("sysconf-einval", sysconf(9999) == -1 && errno == EINVAL);
      chk("pathconf", pathconf("/", _PC_NAME_MAX) == NAME_MAX && fpathconf(0, _PC_PIPE_BUF) == PIPE_BUF);
      struct rlimit rl; chk("getrlimit-nofile", getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 24);
      struct rlimit nl = {10, 20}; chk("setrlimit-roundtrip", setrlimit(RLIMIT_CPU, &nl) == 0 && getrlimit(RLIMIT_CPU, &rl) == 0 && rl.rlim_cur == 10 && rl.rlim_max == 20);
      struct rusage ru; chk("getrusage", getrusage(RUSAGE_SELF, &ru) == 0);
      chk("getpagesize", getpagesize() == 4096 && getdtablesize() == 24);
      chk("wait4", ({ pid_t c = fork(); if (c == 0) _exit(7); int st = 0; struct rusage r2; pid_t w = wait4(c, &st, 0, &r2); w == c && ((st >> 8) & 0xFF) == 7; })); }

    /* ---- random numbers ---- */
    { unsigned char a[32], b[32]; chk("getrandom", getrandom(a, 32, 0) == 32 && getrandom(b, 32, 0) == 32 && memcmp(a, b, 32) != 0);
      chk("getrandom-big", ({ static unsigned char big[10000]; getrandom(big, sizeof big, 0) == 10000 && memcmp(big, big + 5000, 2000) != 0; }));
      chk("getentropy", getentropy(a, 16) == 0); errno = 0; chk("getentropy-too-big", getentropy(a, 300) == -1);
      int low = 0; for (int i = 0; i < 200; ++i) if (arc4random_uniform(10) >= 10) low = 1; chk("arc4random_uniform-bound", !low);
      srandom(42); long r1 = random(); srandom(42); chk("srandom-repeatable", random() == r1);
      unsigned sd = 7; int ra = rand_r(&sd); unsigned sd2 = 7; chk("rand_r-repeatable", rand_r(&sd2) == ra);
      srand48(99); double d1 = drand48(); srand48(99); chk("drand48", drand48() == d1 && d1 >= 0 && d1 < 1);
      chk("lrand48-range", lrand48() >= 0); }

    /* ---- futex across processes via shared memory ---- */
    { volatile uint32_t* w = (volatile uint32_t*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      chk("mmap-shared-anon", w != (volatile uint32_t*)MAP_FAILED); *w = 0;
      errno = 0; chk("futex-eagain", futex_wait(w, 5, 10) == -1 && errno == EAGAIN);
      errno = 0; unsigned long t0 = clock_ms(); chk("futex-timeout", futex_wait(w, 0, 50) == -1 && errno == ETIMEDOUT && clock_ms() - t0 >= 40);
      pid_t c = fork();
      if (c == 0) { usleep(60000); *w = 1; futex_wake(w, 1); _exit(0); }
      t0 = clock_ms(); int r = futex_wait(w, 0, 3000); chk("futex-wake-cross-process", r == 0 && *w == 1 && clock_ms() - t0 >= 40 && clock_ms() - t0 < 2000);
      waitpid(c, 0, 0);
      chk("futex-wake-none", futex_wake(w, 1) == 0);
      munmap((void*)w, 4096); }

    /* ---- shm_open / sem_open ---- */
    { int fd = shm_open("/ptshm", O_RDWR | O_CREAT, 0600); chk("shm_open", fd >= 0 && ftruncate(fd, 4096) == 0);
      volatile int* p = (volatile int*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); chk("shm-mmap", p != (volatile int*)MAP_FAILED); close(fd);
      p[0] = 0;
      pid_t c = fork(); if (c == 0) { int f2 = shm_open("/ptshm", O_RDWR, 0); volatile int* q = (volatile int*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f2, 0); q[0] = 4242; _exit(0); }
      waitpid(c, 0, 0); chk("shm-visible-across-processes", p[0] == 4242);
      chk("shm_unlink", shm_unlink("/ptshm") == 0); errno = 0; chk("shm_open-gone", shm_open("/ptshm", O_RDWR, 0) == -1 && errno == ENOENT);
      errno = 0; chk("shm_open-einval", shm_open("noslash", O_RDWR | O_CREAT, 0600) == -1 && errno == EINVAL);
      sem_unlink("/ptsem"); sem_t* s = sem_open("/ptsem", O_CREAT | O_EXCL, 0600, 0); chk("sem_open-create", s != SEM_FAILED);
      errno = 0; chk("sem_open-excl-exists", sem_open("/ptsem", O_CREAT | O_EXCL, 0600, 0) == SEM_FAILED && errno == EEXIST);
      pid_t c2 = fork(); if (c2 == 0) { sem_t* q = sem_open("/ptsem", 0); if (q == SEM_FAILED) _exit(2); usleep(50000); sem_post(q); sem_post(q); sem_close(q); _exit(0); }
      unsigned long t0 = clock_ms(); chk("sem-wait-cross-process", sem_wait(s) == 0 && clock_ms() - t0 >= 30);
      chk("sem-second-post", sem_trywait(s) == 0); errno = 0; chk("sem-trywait-empty", sem_trywait(s) == -1 && errno == EAGAIN);
      waitpid(c2, 0, 0); chk("sem_close-unlink", sem_close(s) == 0 && sem_unlink("/ptsem") == 0);
      errno = 0; chk("sem_open-gone", sem_open("/ptsem", 0) == SEM_FAILED && errno == ENOENT); }

    /* ---- epoll ---- */
    { int ep = epoll_create1(0); chk("epoll_create1", ep >= 0);
      int p1[2], p2[2]; pipe(p1); pipe(p2);
      struct epoll_event ev; ev.events = EPOLLIN; ev.data.fd = p1[0]; chk("epoll_ctl-add", epoll_ctl(ep, EPOLL_CTL_ADD, p1[0], &ev) == 0);
      ev.data.fd = p2[0]; epoll_ctl(ep, EPOLL_CTL_ADD, p2[0], &ev);
      errno = 0; chk("epoll_ctl-eexist", epoll_ctl(ep, EPOLL_CTL_ADD, p1[0], &ev) == -1 && errno == EEXIST);
      struct epoll_event out[4]; chk("epoll_wait-timeout", epoll_wait(ep, out, 4, 30) == 0);
      write(p2[1], "x", 1); int n = epoll_wait(ep, out, 4, 1000); chk("epoll_wait-ready", n == 1 && out[0].data.fd == p2[0] && (out[0].events & EPOLLIN));
      n = epoll_wait(ep, out, 4, 0); chk("epoll-level-triggered", n == 1);
      char b[4]; read(p2[0], b, 1); chk("epoll-drained", epoll_wait(ep, out, 4, 0) == 0);
      pid_t c = fork(); if (c == 0) { usleep(60000); write(p1[1], "y", 1); _exit(0); }
      unsigned long t0 = clock_ms(); n = epoll_wait(ep, out, 4, -1); chk("epoll_wait-blocks-until-event", n == 1 && out[0].data.fd == p1[0] && clock_ms() - t0 >= 40); waitpid(c, 0, 0);
      read(p1[0], b, 1);
      close(p1[1]); n = epoll_wait(ep, out, 4, 500); chk("epoll-hup", n == 1 && (out[0].events & EPOLLHUP));
      chk("epoll_ctl-del", epoll_ctl(ep, EPOLL_CTL_DEL, p1[0], 0) == 0); errno = 0; chk("epoll_ctl-del-enoent", epoll_ctl(ep, EPOLL_CTL_DEL, p1[0], 0) == -1 && errno == ENOENT);
      /* edge-triggered and oneshot */
      int p3[2]; pipe(p3); ev.events = EPOLLIN | EPOLLET; ev.data.fd = p3[0]; epoll_ctl(ep, EPOLL_CTL_ADD, p3[0], &ev);
      write(p3[1], "ab", 2); n = epoll_wait(ep, out, 4, 500); int first = n; n = epoll_wait(ep, out, 4, 80);
      chk("epoll-edge-reports-once", first == 1 && n == 0);
      read(p3[0], b, 2); write(p3[1], "c", 1); n = epoll_wait(ep, out, 4, 500); chk("epoll-edge-rearms", n == 1);
      read(p3[0], b, 1); epoll_ctl(ep, EPOLL_CTL_DEL, p3[0], 0);
      ev.events = EPOLLIN | EPOLLONESHOT; ev.data.fd = p3[0]; epoll_ctl(ep, EPOLL_CTL_ADD, p3[0], &ev);
      write(p3[1], "d", 1); n = epoll_wait(ep, out, 4, 500); int o1 = n; write(p3[1], "e", 1); n = epoll_wait(ep, out, 4, 80);
      chk("epoll-oneshot", o1 == 1 && n == 0);
      ev.events = EPOLLIN | EPOLLONESHOT; epoll_ctl(ep, EPOLL_CTL_MOD, p3[0], &ev); n = epoll_wait(ep, out, 4, 500); chk("epoll-oneshot-rearm-mod", n == 1);
      /* eventfd + closed descriptor drops out */
      int efd = eventfd(0, 0); ev.events = EPOLLIN; ev.data.u32 = 777; epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev); eventfd_write(efd, 3);
      n = epoll_wait(ep, out, 4, 500); int found = 0; for (int i = 0; i < n; ++i) if (out[i].data.u32 == 777) found = 1; chk("epoll-eventfd", found);
      close(efd); (void)epoll_wait(ep, out, 4, 0); errno = 0; chk("epoll-closed-fd-removed", epoll_ctl(ep, EPOLL_CTL_DEL, efd, 0) == -1);
      errno = 0; chk("epoll-bad-ep", epoll_wait(99, out, 4, 0) == -1 && errno == EBADF);
      close(ep); errno = 0; chk("epoll-closed", epoll_wait(ep, out, 4, 0) == -1);
      int ep2 = epoll_create1(0); chk("epoll-slot-reuse", ep2 >= 0); close(ep2);
      close(p1[0]); close(p2[0]); close(p2[1]); close(p3[0]); close(p3[1]); }

    /* ---- timerfd / clock_nanosleep ---- */
    { int tf = timerfd_create(CLOCK_MONOTONIC, 0); chk("timerfd_create", tf >= 0);
      struct itimerspec its = {{0, 0}, {0, 80 * 1000000L}}; chk("timerfd_settime", timerfd_settime(tf, 0, &its, 0) == 0);
      chk("timerfd-not-ready", ready(tf, POLLIN, 0) == 0);
      unsigned long t0 = clock_ms(); uint64_t cnt = 0; chk("timerfd-read-blocks", read(tf, &cnt, 8) == 8 && cnt >= 1 && clock_ms() - t0 >= 60 && clock_ms() - t0 < 500);
      struct itimerspec cur; chk("timerfd_gettime", timerfd_gettime(tf, &cur) == 0 && cur.it_value.tv_sec == 0);
      struct itimerspec per = {{0, 30 * 1000000L}, {0, 30 * 1000000L}}; timerfd_settime(tf, 0, &per, 0); usleep(150000);
      cnt = 0; read(tf, &cnt, 8); chk("timerfd-periodic-counts", cnt >= 3 && cnt <= 8);
      struct itimerspec off = {{0, 0}, {0, 0}}; struct itimerspec old; chk("timerfd-disarm", timerfd_settime(tf, 0, &off, &old) == 0 && old.it_interval.tv_nsec == 30 * 1000000L);
      fcntl(tf, F_SETFL, O_NONBLOCK); errno = 0; chk("timerfd-nonblock-eagain", read(tf, &cnt, 8) == -1 && errno == EAGAIN);
      struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); struct itimerspec abs = {{0, 0}, {now.tv_sec, now.tv_nsec + 50 * 1000000L}};
      if (abs.it_value.tv_nsec >= 1000000000L) { abs.it_value.tv_sec++; abs.it_value.tv_nsec -= 1000000000L; }
      chk("timerfd-abstime", timerfd_settime(tf, TFD_TIMER_ABSTIME, &abs, 0) == 0 && (ready(tf, POLLIN, 500) & POLLIN) != 0);
      errno = 0; chk("timerfd_create-einval", timerfd_create(99, 0) == -1 && errno == EINVAL);
      close(tf);
      struct timespec rq = {0, 40 * 1000000L}; clock_gettime(CLOCK_MONOTONIC, &now); t0 = clock_ms();
      chk("clock_nanosleep-rel", clock_nanosleep(CLOCK_MONOTONIC, 0, &rq, 0) == 0 && clock_ms() - t0 >= 30);
      clock_gettime(CLOCK_MONOTONIC, &now); struct timespec at = {now.tv_sec, now.tv_nsec + 40 * 1000000L}; if (at.tv_nsec >= 1000000000L) { at.tv_sec++; at.tv_nsec -= 1000000000L; }
      t0 = clock_ms(); chk("clock_nanosleep-abs", clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, 0) == 0 && clock_ms() - t0 >= 25);
      chk("clock_nanosleep-einval", clock_nanosleep(77, 0, &rq, 0) == EINVAL); }

    /* ---- flock ---- */
    { wr_file("/tmp/pt_lock", "x"); int a = open("/tmp/pt_lock", O_RDWR), b = open("/tmp/pt_lock", O_RDWR);
      chk("flock-ex", flock(a, LOCK_EX) == 0);
      errno = 0; chk("flock-conflict-eagain", flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EAGAIN);
      errno = 0; chk("flock-sh-conflict", flock(b, LOCK_SH | LOCK_NB) == -1 && errno == EAGAIN);
      chk("flock-unlock", flock(a, LOCK_UN) == 0 && flock(b, LOCK_EX | LOCK_NB) == 0);
      flock(b, LOCK_UN); flock(a, LOCK_SH); chk("flock-shared-ok", flock(b, LOCK_SH | LOCK_NB) == 0);
      flock(a, LOCK_UN); flock(b, LOCK_UN);
      flock(a, LOCK_EX); pid_t c = fork();
      if (c == 0) { int f = open("/tmp/pt_lock", O_RDWR); unsigned long t0 = clock_ms(); flock(f, LOCK_EX); _exit(clock_ms() - t0 >= 40 ? 0 : 1); }
      usleep(80000); flock(a, LOCK_UN); int st = 0; waitpid(c, &st, 0); chk("flock-blocks-until-released", st == 0);
      flock(a, LOCK_EX); close(a); chk("flock-released-on-close", flock(b, LOCK_EX | LOCK_NB) == 0); close(b); unlink("/tmp/pt_lock"); }

    /* ---- dup3 / misc ---- */
    { int pp[2]; pipe(pp); int d = dup3(pp[0], 15, O_CLOEXEC); chk("dup3", d == 15 && fcntl(15, F_GETFD) == FD_CLOEXEC); errno = 0; chk("dup3-same-einval", dup3(pp[0], pp[0], 0) == -1 && errno == EINVAL); close(15); close(pp[0]); close(pp[1]); }
    chk("many-fopen", ({ FILE* fs[20]; int n = 0; for (; n < 20; ++n) { fs[n] = fopen("/tmp/pt_a.txt", "r"); if (!fs[n]) break; } int ok = n >= 18; for (int i = 0; i < n; ++i) fclose(fs[i]); ok; }));

    /* ---- strings / wide / locale ---- */
    { char d[64]; chk("stpcpy", stpcpy(d, "abc") == d + 3 && !strcmp(d, "abc")); chk("stpncpy", stpncpy(d, "ab", 5) == d + 2 && d[4] == 0);
      chk("strverscmp", strverscmp("file2", "file10") < 0 && strverscmp("a1", "a1") == 0 && strverscmp("v1.10", "v1.9") > 0);
      chk("strerror_r", strerror_r(ENOENT, d, sizeof d) == 0 && d[0] && strerror_r(ENOENT, d, 2) == ERANGE);
      chk("strcoll-strxfrm", strcoll("a", "b") < 0 && strxfrm(d, "abc", sizeof d) == 3 && !strcmp(d, "abc"));
      chk("nl_langinfo", !strcmp(nl_langinfo(CODESET), "UTF-8")); }
    { wchar_t w[8]; const char* u = "h\xc3\xa9llo\xe2\x82\xac"; size_t n = mbstowcs(w, u, 8);
      chk("mbstowcs", n == 6 && w[1] == 0xE9 && w[5] == 0x20AC && w[6] == 0);
      char back[16]; chk("wcstombs", wcstombs(back, w, sizeof back) == 9 && !strcmp(back, u));
      chk("wcslen-cmp", wcslen(w) == 6 && wcscmp(w, w) == 0 && wcschr(w, 0xE9) == w + 1);
      wchar_t c; chk("mbtowc", mbtowc(&c, "\xe2\x82\xac", 3) == 3 && c == 0x20AC && mblen("A", 1) == 1);
      errno = 0; chk("mbtowc-invalid", mbtowc(&c, "\xc3\x28", 2) == -1 && errno == EILSEQ);
      char o[4]; chk("wctomb", wctomb(o, 0x1F600) == 4 && (unsigned char)o[0] == 0xF0); }

    /* ---- strptime ---- */
    { struct tm t; memset(&t, 0, sizeof t); char* e = strptime("2026-11-10 16:04:05", "%Y-%m-%d %H:%M:%S", &t);
      chk("strptime-iso", e && *e == 0 && t.tm_year == 126 && t.tm_mon == 10 && t.tm_mday == 10 && t.tm_hour == 16 && t.tm_min == 4 && t.tm_sec == 5);
      chk("strptime-wday-yday", t.tm_wday == 2 && t.tm_yday == 313);
      memset(&t, 0, sizeof t); e = strptime("Tue, 03 Mar 2020 07:08 PM", "%a, %d %b %Y %I:%M %p", &t);
      chk("strptime-names-ampm", e && *e == 0 && t.tm_mon == 2 && t.tm_mday == 3 && t.tm_hour == 19 && t.tm_year == 120 && t.tm_wday == 2);
      chk("strptime-fail", strptime("abc", "%Y", &t) == 0 && strptime("13", "%m", &t) == 0);
      chk("strptime-remainder", ({ struct tm u; memset(&u, 0, sizeof u); char* r = strptime("12:30xyz", "%R", &u); r && !strcmp(r, "xyz") && u.tm_hour == 12 && u.tm_min == 30; })); }

    /* ---- syslog / on_exit ---- */
    { openlog("posixtest", LOG_PID, LOG_USER); syslog(LOG_INFO, "hello %d", 42); closelog();
      int fd = open("/var/log/messages", O_RDONLY); char buf[2048]; long n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1; if (fd >= 0) close(fd); if (n < 0) n = 0; buf[n] = 0;
      chk("syslog-appends", n > 0 && strstr(buf, "posixtest[") && strstr(buf, "hello 42")); }

    printf("posixtest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
