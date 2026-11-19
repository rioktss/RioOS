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

#include "futex.h"
#include "thread.h"
#include "process.h"
#include "mmu.h"
#include "task.h"
#include "timer.h"

/* Fast userspace mutexes. All state is touched with the kernel lock held, so a wake can never slip between a waiter's check
   of the word and its going to sleep. */

struct FWaiter
{
    FWaiter* next;
    FWaiter* prev;
    uint64_t ka, kb;       /* key: private = (pid + 1, virtual address); shared = (0, physical address) */
    int thread;
    uint32_t bitset;
    int woken;
    int pi;                /* waits for a priority-inheritance futex */
    int owner;             /* thread that owns that futex right now */
    int pid;               /* process of the waiter and the address of the word there (PI futexes: owner death rewrites the word) */
    uint64_t uaddr;
};

#define FBUCKETS 256
static FWaiter* fb_head[FBUCKETS];
static FWaiter* fb_tail[FBUCKETS];

static unsigned fb_index(uint64_t ka, uint64_t kb)
{
    uint64_t h = (kb >> 2) * 0x9E3779B97F4A7C15ULL ^ (ka * 0xC2B2AE3D27D4EB4FULL);
    return (unsigned)(h >> 40) % FBUCKETS;
}
static void fb_link(FWaiter* w)
{
    unsigned i = fb_index(w->ka, w->kb);
    w->next = 0; w->prev = fb_tail[i];
    if (fb_tail[i]) fb_tail[i]->next = w; else fb_head[i] = w;
    fb_tail[i] = w;
}
static void fb_unlink(FWaiter* w)
{
    unsigned i = fb_index(w->ka, w->kb);
    if (w->prev) w->prev->next = w->next; else fb_head[i] = w->next;
    if (w->next) w->next->prev = w->prev; else fb_tail[i] = w->prev;
    w->next = w->prev = 0;
}

struct FKey { uint64_t ka, kb; uint32_t* word; };

/* Makes the word readable and writable (this breaks copy-on-write sharing, so the page stays put while threads sleep on it)
   and computes its key. The kernel runs on the caller's page tables, so the user address is usable directly. */
static int fkey(int pid, uint64_t uaddr, FKey* k)
{
    if ((uaddr & 3ULL) != 0) return -22;
    if (!mmu_user_pointer_ok(uaddr, 4, 1) && !mmu_user_pointer_ok(uaddr, 4, 0)) return -14;
    k->word = (uint32_t*)uaddr;
    if (mmu_user_space_is_shared(pid, uaddr))
    {
        void* phys = mmu_user_space_pointer(pid, uaddr);
        if (!phys) return -14;
        k->ka = 0; k->kb = (uint64_t)phys;
    }
    else { k->ka = (uint64_t)(uint32_t)pid + 1ULL; k->kb = uaddr; }
    return 0;
}

static int wake_key(uint64_t ka, uint64_t kb, int n, uint32_t bitset)
{
    int woke = 0;
    unsigned i = fb_index(ka, kb);
    for (FWaiter* w = fb_head[i]; w && woke < n; )
    {
        FWaiter* nx = w->next;
        if (w->ka == ka && w->kb == kb && !w->pi && (w->bitset & bitset))
        {
            w->woken = 1;
            fb_unlink(w);
            (void)thread_wake(w->thread, THREAD_WAKE_NORMAL);
            ++woke;
        }
        w = nx;
    }
    return woke;
}

int futex_wake_user(int pid, uint64_t uaddr, int n)
{
    FKey k;
    if (fkey(pid, uaddr, &k) != 0) return 0;
    return wake_key(k.ka, k.kb, n, 0xFFFFFFFFU);
}

/* Sleeps until woken, timed out (-110) or interrupted (-4). The waiter record lives on this kernel stack. */
static int64_t block_waiter(int pid, FWaiter* w, uint64_t timeout_ms)
{
    uint64_t deadline = timeout_ms == ~0ULL ? 0 : timer_millis() + timeout_ms;
    int64_t rc = 0;
    for (;;)
    {
        uint64_t remain = 0;
        if (deadline)
        {
            uint64_t n = timer_millis();
            if (n >= deadline) { rc = -110; break; }
            remain = deadline - n;
        }
        int r = thread_block(remain);
        if (w->woken) return 0;
        if (r == THREAD_WAKE_TIMEOUT) { rc = -110; break; }
        if (task_sig_interrupts(pid)) { rc = -4; break; }
    }
    if (!w->woken) fb_unlink(w);
    return rc;
}

static int64_t do_wait(int pid, uint64_t uaddr, uint32_t expected, uint64_t timeout_ms, uint32_t bitset)
{
    FKey k;
    int rc = fkey(pid, uaddr, &k);
    if (rc != 0) return rc;
    if (*k.word != expected) return -11;                       /* EAGAIN: the value already changed */
    if (task_sig_interrupts(pid)) return -4;
    FWaiter w;
    w.next = w.prev = 0; w.ka = k.ka; w.kb = k.kb; w.thread = thread_current(); w.bitset = bitset; w.woken = 0; w.pi = 0; w.owner = -1; w.pid = pid; w.uaddr = uaddr;
    fb_link(&w);
    return block_waiter(pid, &w, timeout_ms);
}

static int64_t do_requeue(int pid, uint64_t uaddr, int nwake, int nrequeue, uint64_t uaddr2, int check, uint32_t expected)
{
    FKey a, b;
    int rc = fkey(pid, uaddr, &a);
    if (rc == 0) rc = fkey(pid, uaddr2, &b);
    if (rc != 0) return rc;
    if (check && *a.word != expected) return -11;
    int woke = wake_key(a.ka, a.kb, nwake, 0xFFFFFFFFU);
    int moved = 0;
    unsigned i = fb_index(a.ka, a.kb);
    for (FWaiter* w = fb_head[i]; w && moved < nrequeue; )
    {
        FWaiter* nx = w->next;
        if (w->ka == a.ka && w->kb == a.kb && !w->pi)
        {
            fb_unlink(w);
            w->ka = b.ka; w->kb = b.kb;
            fb_link(w);
            ++moved;
        }
        w = nx;
    }
    return woke + moved;
}

static int64_t do_wake_op(int pid, uint64_t uaddr, int nwake, int nwake2, uint64_t uaddr2, uint32_t enc)
{
    FKey a, b;
    int rc = fkey(pid, uaddr, &a);
    if (rc == 0) rc = fkey(pid, uaddr2, &b);
    if (rc != 0) return rc;
    int op = (int)((enc >> 28) & 7U), cmp = (int)((enc >> 24) & 15U);
    int32_t oparg = (int32_t)((enc >> 12) & 0xFFFU), cmparg = (int32_t)(enc & 0xFFFU);
    if (oparg & 0x800) oparg |= ~0xFFF;
    if (cmparg & 0x800) cmparg |= ~0xFFF;
    if (enc & 0x80000000U) oparg = 1 << (oparg & 31);
    uint32_t old = *b.word, nv;
    switch (op)
    {
        case 0: nv = (uint32_t)oparg; break;
        case 1: nv = old + (uint32_t)oparg; break;
        case 2: nv = old | (uint32_t)oparg; break;
        case 3: nv = old & ~(uint32_t)oparg; break;
        case 4: nv = old ^ (uint32_t)oparg; break;
        default: return -38;
    }
    int cond;
    int32_t so = (int32_t)old;
    switch (cmp)
    {
        case 0: cond = so == cmparg; break;
        case 1: cond = so != cmparg; break;
        case 2: cond = so < cmparg; break;
        case 3: cond = so <= cmparg; break;
        case 4: cond = so > cmparg; break;
        case 5: cond = so >= cmparg; break;
        default: return -38;
    }
    *b.word = nv;
    int woke = wake_key(a.ka, a.kb, nwake, 0xFFFFFFFFU);
    if (cond) woke += wake_key(b.ka, b.kb, nwake2, 0xFFFFFFFFU);
    return woke;
}

/* ---- priority-inheritance futexes: the word holds the owner's thread id; the kernel hands the lock to the best waiter ---- */

/* A thread's boost is the highest priority among the waiters of every PI futex it owns; a boosted thread that itself waits on
   a PI futex passes the boost on to that futex's owner. */
static void pi_recompute(int th)
{
    for (int depth = 0; th >= 0 && depth < 32; ++depth)
    {
        int best = 0;
        for (unsigned i = 0; i < FBUCKETS; ++i)
            for (FWaiter* w = fb_head[i]; w; w = w->next)
                if (w->pi && w->owner == th)
                {
                    int p = thread_effective_prio(w->thread);
                    if (p > best) best = p;
                }
        (void)thread_set_boost(th, best);
        int next = -1;
        for (unsigned i = 0; i < FBUCKETS && next < 0; ++i)
            for (FWaiter* w = fb_head[i]; w; w = w->next)
                if (w->pi && w->thread == th) { next = w->owner; break; }
        th = next;
    }
}

static int64_t do_lock_pi(int pid, uint64_t uaddr, uint64_t timeout_ms, int trylock)
{
    FKey k;
    int rc = fkey(pid, uaddr, &k);
    if (rc != 0) return rc;
    int me = thread_current();
    uint32_t mytid = thread_tid_of(me);
    uint32_t word = *k.word;
    uint32_t owner_tid = word & FUTEX_TID_MASK;
    if (owner_tid == 0) { *k.word = (word & (FUTEX_WAITERS | FUTEX_OWNER_DIED)) | mytid; return 0; }
    if (owner_tid == mytid) return -35;                         /* EDEADLK */
    int owner = thread_find_tid(owner_tid);
    if (owner < 0)                                              /* the owner vanished without unlocking */
    {
        *k.word = (word & FUTEX_WAITERS) | FUTEX_OWNER_DIED | mytid;
        return 0;
    }
    if (trylock) return -11;
    if (task_sig_interrupts(pid)) return -4;
    *k.word = word | FUTEX_WAITERS;
    FWaiter w;
    w.next = w.prev = 0; w.ka = k.ka; w.kb = k.kb; w.thread = me; w.bitset = 0xFFFFFFFFU; w.woken = 0; w.pi = 1; w.owner = owner; w.pid = pid; w.uaddr = uaddr;
    fb_link(&w);
    pi_recompute(owner);
    int64_t r = block_waiter(pid, &w, timeout_ms);
    if (r != 0)
    {
        pi_recompute(w.owner);                                  /* this waiter no longer pushes its priority on the owner */
        /* last waiter gone: drop the WAITERS bit so the owner's fast unlock path works */
        int any = 0;
        unsigned i = fb_index(k.ka, k.kb);
        for (FWaiter* x = fb_head[i]; x; x = x->next) if (x->pi && x->ka == k.ka && x->kb == k.kb) { any = 1; break; }
        if (!any && (*k.word & FUTEX_TID_MASK) == owner_tid) *k.word &= ~FUTEX_WAITERS;
    }
    return r;
}

static int64_t do_unlock_pi(int pid, uint64_t uaddr)
{
    FKey k;
    int rc = fkey(pid, uaddr, &k);
    if (rc != 0) return rc;
    int me = thread_current();
    uint32_t word = *k.word;
    if ((word & FUTEX_TID_MASK) != thread_tid_of(me)) return -1;     /* EPERM */
    unsigned i = fb_index(k.ka, k.kb);
    FWaiter* best = 0;
    int others = 0;
    for (FWaiter* w = fb_head[i]; w; w = w->next)
    {
        if (!(w->pi && w->ka == k.ka && w->kb == k.kb)) continue;
        if (!best) { best = w; continue; }
        ++others;
        if (thread_effective_prio(w->thread) > thread_effective_prio(best->thread)) best = w;
    }
    if (!best) { *k.word = 0; pi_recompute(me); return 0; }
    /* count the waiters left after handing over */
    int remaining = 0;
    for (FWaiter* w = fb_head[i]; w; w = w->next)
        if (w != best && w->pi && w->ka == k.ka && w->kb == k.kb) { w->owner = best->thread; ++remaining; }
    (void)others;
    *k.word = thread_tid_of(best->thread) | (remaining ? FUTEX_WAITERS : 0U);
    best->woken = 1;
    fb_unlink(best);
    int nth = best->thread;
    (void)thread_wake(nth, THREAD_WAKE_NORMAL);
    pi_recompute(me);
    pi_recompute(nth);
    return 0;
}

int64_t futex_op(int pid, uint64_t uaddr, int op, uint64_t val, uint64_t timeout_ms, uint64_t uaddr2, uint64_t val3)
{
    op &= ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    switch (op)
    {
        case FUTEX_WAIT: return do_wait(pid, uaddr, (uint32_t)val, timeout_ms, 0xFFFFFFFFU);
        case FUTEX_WAIT_BITSET:
            if ((uint32_t)val3 == 0) return -22;
            return do_wait(pid, uaddr, (uint32_t)val, timeout_ms, (uint32_t)val3);
        case FUTEX_WAKE:
        case FUTEX_WAKE_BITSET:
        {
            uint32_t bs = op == FUTEX_WAKE ? 0xFFFFFFFFU : (uint32_t)val3;
            if (bs == 0) return -22;
            FKey k;
            int rc = fkey(pid, uaddr, &k);
            if (rc != 0) return rc;
            int n = (int)(int64_t)val; if (n < 0) n = 0x7FFFFFFF; if (val > 0x7FFFFFFFULL && (int64_t)val >= 0) n = 0x7FFFFFFF;
            return wake_key(k.ka, k.kb, n, bs);
        }
        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE:
        {
            int nw = (int)(uint32_t)val, nr = (int)(uint32_t)timeout_ms;
            if (nw < 0) nw = 0x7FFFFFFF;
            if (nr < 0) nr = 0x7FFFFFFF;
            return do_requeue(pid, uaddr, nw, nr, uaddr2, op == FUTEX_CMP_REQUEUE, (uint32_t)val3);
        }
        case FUTEX_WAKE_OP:
        {
            int nw = (int)(uint32_t)val, nw2 = (int)(uint32_t)timeout_ms;
            if (nw < 0) nw = 0x7FFFFFFF;
            if (nw2 < 0) nw2 = 0x7FFFFFFF;
            return do_wake_op(pid, uaddr, nw, nw2, uaddr2, (uint32_t)val3);
        }
        case FUTEX_LOCK_PI: return do_lock_pi(pid, uaddr, timeout_ms, 0);
        case FUTEX_TRYLOCK_PI: return do_lock_pi(pid, uaddr, 0, 1);
        case FUTEX_UNLOCK_PI: return do_unlock_pi(pid, uaddr);
        default: return -38;
    }
}

/* ---- thread exit: clear_child_tid (pthread_join) and the robust-mutex list ---- */

static int uread64(uint64_t a, uint64_t* v)
{
    if ((a & 7ULL) != 0 || !mmu_user_pointer_ok(a, 8, 0)) return -1;
    *v = *(volatile uint64_t*)a;
    return 0;
}

static void robust_release(int pid, uint64_t uaddr, uint32_t tid)
{
    if ((uaddr & 3ULL) != 0 || !mmu_user_pointer_ok(uaddr, 4, 1)) return;
    volatile uint32_t* w = (volatile uint32_t*)uaddr;
    uint32_t v = *w;
    if ((v & FUTEX_TID_MASK) != tid) return;
    *w = (v & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
    if (v & FUTEX_WAITERS) (void)futex_wake_user(pid, uaddr, 1);
}

/* A thread that dies owning priority-inheritance futexes hands each one to its best waiter, with OWNER_DIED set in the word. */
static void pi_owner_died(int th)
{
    for (;;)
    {
        FWaiter* first = 0;
        for (unsigned i = 0; i < FBUCKETS && !first; ++i)
            for (FWaiter* w = fb_head[i]; w; w = w->next)
                if (w->pi && w->owner == th) { first = w; break; }
        if (!first) return;
        uint64_t ka = first->ka, kb = first->kb;
        int wpid = first->pid;
        uint64_t wua = first->uaddr;
        unsigned i = fb_index(ka, kb);
        FWaiter* best = 0;
        for (FWaiter* w = fb_head[i]; w; w = w->next)
            if (w->pi && w->ka == ka && w->kb == kb && (!best || thread_effective_prio(w->thread) > thread_effective_prio(best->thread))) best = w;
        int remaining = 0;
        for (FWaiter* w = fb_head[i]; w; w = w->next)
            if (w != best && w->pi && w->ka == ka && w->kb == kb) { w->owner = best->thread; ++remaining; }
        uint32_t* wp = (uint32_t*)mmu_user_space_pointer(wpid, wua);
        if (wp) *wp = thread_tid_of(best->thread) | FUTEX_OWNER_DIED | (remaining ? FUTEX_WAITERS : 0U);
        int nth = best->thread;
        best->woken = 1;
        fb_unlink(best);
        (void)thread_wake(nth, THREAD_WAKE_NORMAL);
        pi_recompute(nth);
    }
}

void futex_thread_exit(int pid, int thread)
{
    uint32_t tid = thread_tid_of(thread);
    uint64_t head = thread_robust_of(thread);
    if (head)
    {
        /* struct robust_list_head { void* next; long futex_offset; void* pending; } -- the Linux layout */
        uint64_t next = 0, off = 0, pending = 0;
        if (uread64(head, &next) == 0 && uread64(head + 8, &off) == 0 && uread64(head + 16, &pending) == 0)
        {
            int limit = 1 << 20;
            while (next != head && next != 0 && limit-- > 0)
            {
                uint64_t after = 0;
                if (uread64(next, &after) != 0) break;
                if (next != pending) robust_release(pid, next + off, tid);
                next = after;
            }
            if (pending) robust_release(pid, pending + off, tid);
        }
    }
    uint64_t ct = thread_clear_tid_of(thread);
    if (ct && (ct & 3ULL) == 0 && mmu_user_pointer_ok(ct, 4, 1))
    {
        *(volatile uint32_t*)ct = 0;
        (void)futex_wake_user(pid, ct, 0x7FFFFFFF);
    }
    pi_owner_died(thread);
    (void)thread_set_boost(thread, 0);
}
