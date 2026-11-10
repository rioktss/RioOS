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

#include "textutil.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint8_t outbuf[4096];

/* Run `cmd` (space separated, no quoting) over `input` and return the output. */
static int run(const char* cmd, const char* input, char* result, size_t cap)
{
    char line[256]; strncpy(line, cmd, sizeof(line) - 1); line[sizeof(line) - 1] = 0;
    char* argv[12]; int argc = 0;
    char* sp_ = nullptr; for (char* p = strtok_r(line, " ", &sp_); p && argc < 12; p = strtok_r(nullptr, " ", &sp_)) argv[argc++] = p;
    uint64_t n = 0;
    int rc = textutil_filter(argc, argv, (const uint8_t*)input, strlen(input), outbuf, sizeof(outbuf), &n);
    if (n >= cap) n = cap - 1;
    memcpy(result, outbuf, (size_t)n); result[n] = 0;
    return rc;
}

#define EXPECT(cmd, input, expected) do { char r[4096]; int rc = run(cmd, input, r, sizeof(r)); \
    if (rc != 0 || strcmp(r, expected) != 0) { printf("FAIL line %d: '%s' rc=%d got [%s]\n", __LINE__, cmd, rc, r); return 1; } } while (0)
#define EXPECT_ERR(cmd, input, code) do { char r[4096]; int rc = run(cmd, input, r, sizeof(r)); \
    if (rc != (code)) { printf("FAIL line %d: '%s' rc=%d want %d\n", __LINE__, cmd, rc, code); return 1; } } while (0)

static int eval(const char* e, int64_t* v) { return textutil_eval(e, v); }

int main()
{
    const char* abc = "a\nb\nc\nd\ne\n";

    /* head / tail */
    EXPECT("head -n 2", abc, "a\nb\n");
    EXPECT("head -n2", abc, "a\nb\n");
    EXPECT("head -3", abc, "a\nb\nc\n");
    EXPECT("head -n 0", abc, "");
    EXPECT("head -n 99", abc, "a\nb\nc\nd\ne\n");
    EXPECT("tail -n 2", abc, "d\ne\n");
    EXPECT("tail -n 99", abc, "a\nb\nc\nd\ne\n");
    EXPECT("head", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
    EXPECT("tail -n 1", "x\ny", "y\n");              /* no trailing newline in input */
    EXPECT_ERR("head -n", abc, TEXTUTIL_ERR_USAGE);
    EXPECT_ERR("head -n x", abc, TEXTUTIL_ERR_USAGE);
    EXPECT_ERR("head extra", abc, TEXTUTIL_ERR_USAGE);

    /* sort */
    EXPECT("sort", "pear\napple\nfig\n", "apple\nfig\npear\n");
    EXPECT("sort -r", "pear\napple\nfig\n", "pear\nfig\napple\n");
    EXPECT("sort -n", "10\n9\n100\n-5\n", "-5\n9\n10\n100\n");
    EXPECT("sort -nr", "10\n9\n100\n", "100\n10\n9\n");
    EXPECT("sort -u", "b\na\nb\na\nc\n", "a\nb\nc\n");
    EXPECT("sort", "", "");
    EXPECT("sort", "B\na\nA\n", "A\nB\na\n");        /* bytewise, not locale */
    EXPECT_ERR("sort -z", abc, TEXTUTIL_ERR_USAGE);

    /* uniq */
    EXPECT("uniq", "a\na\nb\na\n", "a\nb\na\n");
    EXPECT("uniq -c", "a\na\nb\n", "   2 a\n   1 b\n");
    EXPECT("uniq -d", "a\na\nb\nc\nc\n", "a\nc\n");

    /* rev / tac / nl */
    EXPECT("rev", "abc\nxy\n", "cba\nyx\n");
    EXPECT("tac", "1\n2\n3\n", "3\n2\n1\n");
    EXPECT("nl", "x\ny\n", "     1  x\n     2  y\n");
    EXPECT("rev", "ab\r\ncd\r\n", "ba\ndc\n");        /* CRLF tolerated */

    /* cut */
    EXPECT("cut -d : -f 1", "root:x:0\nuser:y:1\n", "root\nuser\n");
    EXPECT("cut -d: -f2,3", "a:b:c:d\n", "b:c\n");
    EXPECT("cut -d : -f 2-", "a:b:c\n", "b:c\n");
    EXPECT("cut -d : -f 1", "nodelim\n", "nodelim\n");
    EXPECT("cut -c 1-3", "abcdef\n", "abc\n");
    EXPECT("cut -c 2,4", "abcdef\n", "bd\n");
    EXPECT_ERR("cut -f 1", "a\n", TEXTUTIL_OK);       /* default TAB delimiter */
    EXPECT_ERR("cut -d : -f 0", "a\n", TEXTUTIL_ERR_USAGE);
    EXPECT_ERR("cut -d : -f 1 -c 1", "a\n", TEXTUTIL_ERR_USAGE);
    EXPECT_ERR("cut", "a\n", TEXTUTIL_ERR_USAGE);

    /* tr */
    EXPECT("tr a-z A-Z", "hello 1\n", "HELLO 1\n");
    EXPECT("tr -d l", "hello\n", "heo\n");
    EXPECT("tr abc x", "aabbcc\n", "xxxxxx\n");       /* short SET2 pads */
    EXPECT_ERR("tr a", "a\n", TEXTUTIL_ERR_USAGE);

    /* grep / wc as pipeline stages */
    EXPECT("grep an", "banana\nkiwi\nmango\n", "banana\nmango\n");
    EXPECT("grep -i AN", "banana\nkiwi\n", "banana\n");
    EXPECT("grep -v an", "banana\nkiwi\n", "kiwi\n");
    EXPECT("grep -n kiwi", "banana\nkiwi\n", "2:kiwi\n");
    EXPECT("grep -c a", "a\nb\na\n", "2\n");
    EXPECT_ERR("grep", "a\n", TEXTUTIL_ERR_USAGE);
    EXPECT("wc -l", "a\nb\nc\n", "3\n");
    EXPECT("wc", "one two\nthree\n", "2 3 14\n");
    EXPECT("wc -l", "no newline", "1\n");

    /* limits and errors */
    EXPECT_ERR("nosuch", "a\n", TEXTUTIL_ERR_UNKNOWN);
    CHECK(textutil_is_filter("sort") && textutil_is_filter("grep") && !textutil_is_filter("ls"));
    CHECK(textutil_usage("sort") != 0 && textutil_usage("zzz") == 0);
    {
        /* output buffer overflow is reported, not silently truncated */
        char* argv[1]; char name[] = "tac"; argv[0] = name;
        uint8_t small[4]; uint64_t n = 0;
        const char* in = "aaaa\nbbbb\n";
        CHECK(textutil_filter(1, argv, (const uint8_t*)in, strlen(in), small, sizeof(small), &n) == TEXTUTIL_ERR_OVERFLOW);
        CHECK(n <= sizeof(small));
    }
    {
        /* more lines than the scratch tables allow */
        static char big[3 * 5000 + 1]; size_t p = 0;
        for (int i = 0; i < 5000; ++i) { big[p++] = 'x'; big[p++] = '\n'; }
        big[p] = 0;
        char* argv[1]; char name[] = "tac"; argv[0] = name;
        static uint8_t o[20000]; uint64_t n = 0;
        CHECK(textutil_filter(1, argv, (const uint8_t*)big, p, o, sizeof(o), &n) == TEXTUTIL_ERR_OVERFLOW);
    }

    /* a realistic pipeline: sort | uniq -c | sort -nr | head */
    {
        char r1[4096], r2[4096];
        run("sort", "b\na\nb\nc\nb\na\n", r1, sizeof(r1));
        run("uniq -c", r1, r2, sizeof(r2));
        run("sort -nr", r2, r1, sizeof(r1));
        run("head -n 1", r1, r2, sizeof(r2));
        CHECK(strcmp(r2, "   3 b\n") == 0);
    }

    /* expr */
    int64_t v = 0;
    CHECK(eval("1+2*3", &v) == 0 && v == 7);
    CHECK(eval("(1+2)*3", &v) == 0 && v == 9);
    CHECK(eval(" 10 / 3 ", &v) == 0 && v == 3);
    CHECK(eval("10 % 3", &v) == 0 && v == 1);
    CHECK(eval("-4 + 10", &v) == 0 && v == 6);
    CHECK(eval("2*-3", &v) == 0 && v == -6);
    CHECK(eval("1/0", &v) != 0);
    CHECK(eval("1%0", &v) != 0);
    CHECK(eval("(1+2", &v) != 0);
    CHECK(eval("1+", &v) != 0);
    CHECK(eval("abc", &v) != 0);
    CHECK(eval("", &v) != 0);
    CHECK(eval("1 2", &v) != 0);
    CHECK(eval("99999999999999999999", &v) != 0);
    CHECK(eval("((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))", &v) != 0); /* depth cap */

    /* basename / dirname */
    char b[64];
    textutil_basename("/a/b/c.txt", b, sizeof(b)); CHECK(strcmp(b, "c.txt") == 0);
    textutil_basename("c.txt", b, sizeof(b));      CHECK(strcmp(b, "c.txt") == 0);
    textutil_basename("/a/b/", b, sizeof(b));      CHECK(strcmp(b, "b") == 0);
    textutil_basename("/", b, sizeof(b));          CHECK(strcmp(b, "/") == 0);
    textutil_basename("", b, sizeof(b));           CHECK(strcmp(b, "") == 0);
    textutil_dirname("/a/b/c.txt", b, sizeof(b));  CHECK(strcmp(b, "/a/b") == 0);
    textutil_dirname("c.txt", b, sizeof(b));       CHECK(strcmp(b, ".") == 0);
    textutil_dirname("/c", b, sizeof(b));          CHECK(strcmp(b, "/") == 0);
    textutil_dirname("/a/b/", b, sizeof(b));       CHECK(strcmp(b, "/a") == 0);
    textutil_dirname("a/b", b, sizeof(b));         CHECK(strcmp(b, "a") == 0);
    textutil_basename("/a/b/c.txt", b, 4);         CHECK(strcmp(b, "c.t") == 0);   /* truncation is safe */

    printf("TEXTUTIL HOST TEST: PASS\n");
    return 0;
}
