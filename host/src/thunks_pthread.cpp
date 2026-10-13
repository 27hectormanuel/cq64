// SPDX-License-Identifier: CC0-1.0
// pthread / semaphore imports. Guest sync objects are 4-byte words (bionic LP32);
// we store a handle to a host object in that word, creating it lazily for static initializers.
#include "abi.h"

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <vector>

namespace {
constexpr u32 HANDLE_BIT = 0x80000000u;
constexpr u32 MAX_OBJS = 1 << 16;

template <class Obj> struct HandleTable {
    std::mutex lock;
    Obj *objs[MAX_OBJS] = {};
    std::vector<u32> free_list;
    u32 next = 1;
    u32 add(Obj *o) {
        std::lock_guard<std::mutex> lk(lock);
        u32 i;
        if (!free_list.empty()) {
            i = free_list.back();
            free_list.pop_back();
        } else {
            if (next >= MAX_OBJS) fatal("guest sync object table full");
            i = next++;
        }
        objs[i] = o;
        return i;
    }
    void remove(u32 i) {
        std::lock_guard<std::mutex> lk(lock);
        delete objs[i];
        objs[i] = nullptr;
        free_list.push_back(i);
    }
};

// ---------------------------------------------------------------- mutex
HandleTable<pthread_mutex_t> g_mutexes;
std::mutex g_lazy_lock;

pthread_mutex_t *make_mutex(int type) {
    auto *m = new pthread_mutex_t;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type == 1 ? PTHREAD_MUTEX_RECURSIVE
                                            : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

pthread_mutex_t *get_mutex(u32 g) {
    u32 w = __atomic_load_n((u32 *)g2h(g), __ATOMIC_ACQUIRE);
    if (!(w & HANDLE_BIT)) {
        std::lock_guard<std::mutex> lk(g_lazy_lock);
        w = rd<u32>(g);
        if (!(w & HANDLE_BIT)) {
            // Static initializers: 0 normal, 0x4000 recursive, 0x8000 error-checking.
            int type = (w & 0x4000) ? 1 : (w & 0x8000) ? 2 : 0;
            w = HANDLE_BIT | g_mutexes.add(make_mutex(type));
            __atomic_store_n((u32 *)g2h(g), w, __ATOMIC_RELEASE);
        }
    }
    return g_mutexes.objs[w & ~HANDLE_BIT];
}

int t_mutexattr_init(G<s32> a) { *a.get() = 0; return 0; }
THUNK(pthread_mutexattr_init, t_mutexattr_init);
int t_mutexattr_destroy(G<s32>) { return 0; }
THUNK(pthread_mutexattr_destroy, t_mutexattr_destroy);
int t_mutexattr_settype(G<s32> a, int type) { *a.get() = type; return 0; }
THUNK(pthread_mutexattr_settype, t_mutexattr_settype);
int t_mutexattr_gettype(G<s32> a, G<s32> type) { *type.get() = *a.get(); return 0; }
THUNK(pthread_mutexattr_gettype, t_mutexattr_gettype);
int t_mutexattr_setpshared(G<s32>, int) { return 0; }
THUNK(pthread_mutexattr_setpshared, t_mutexattr_setpshared);

int t_mutex_init(u32 m, u32 attr) {
    int type = attr ? rd<s32>(attr) : 0;
    wr<u32>(m, HANDLE_BIT | g_mutexes.add(make_mutex(type)));
    return 0;
}
THUNK(pthread_mutex_init, t_mutex_init);
int t_mutex_destroy(u32 m) {
    u32 w = rd<u32>(m);
    if (w & HANDLE_BIT) {
        pthread_mutex_destroy(g_mutexes.objs[w & ~HANDLE_BIT]);
        g_mutexes.remove(w & ~HANDLE_BIT);
    }
    wr<u32>(m, 0);
    return 0;
}
THUNK(pthread_mutex_destroy, t_mutex_destroy);
int t_mutex_lock(u32 m) { return pthread_mutex_lock(get_mutex(m)); }
THUNK(pthread_mutex_lock, t_mutex_lock);
int t_mutex_unlock(u32 m) { return pthread_mutex_unlock(get_mutex(m)); }
THUNK(pthread_mutex_unlock, t_mutex_unlock);
int t_mutex_trylock(u32 m) { return pthread_mutex_trylock(get_mutex(m)); }
THUNK(pthread_mutex_trylock, t_mutex_trylock);

// ---------------------------------------------------------------- condition variables
HandleTable<pthread_cond_t> g_conds;
pthread_cond_t *get_cond(u32 g) {
    u32 w = __atomic_load_n((u32 *)g2h(g), __ATOMIC_ACQUIRE);
    if (!(w & HANDLE_BIT)) {
        std::lock_guard<std::mutex> lk(g_lazy_lock);
        w = rd<u32>(g);
        if (!(w & HANDLE_BIT)) {
            auto *c = new pthread_cond_t;
            pthread_cond_init(c, nullptr);
            w = HANDLE_BIT | g_conds.add(c);
            __atomic_store_n((u32 *)g2h(g), w, __ATOMIC_RELEASE);
        }
    }
    return g_conds.objs[w & ~HANDLE_BIT];
}
int t_cond_init(u32 c, u32) {
    auto *hc = new pthread_cond_t;
    pthread_cond_init(hc, nullptr);
    wr<u32>(c, HANDLE_BIT | g_conds.add(hc));
    return 0;
}
THUNK(pthread_cond_init, t_cond_init);
int t_cond_destroy(u32 c) {
    u32 w = rd<u32>(c);
    if (w & HANDLE_BIT) {
        pthread_cond_destroy(g_conds.objs[w & ~HANDLE_BIT]);
        g_conds.remove(w & ~HANDLE_BIT);
    }
    wr<u32>(c, 0);
    return 0;
}
THUNK(pthread_cond_destroy, t_cond_destroy);
int t_cond_signal(u32 c) { return pthread_cond_signal(get_cond(c)); }
THUNK(pthread_cond_signal, t_cond_signal);
int t_cond_broadcast(u32 c) { return pthread_cond_broadcast(get_cond(c)); }
THUNK(pthread_cond_broadcast, t_cond_broadcast);
int t_cond_wait(u32 c, u32 m) { return pthread_cond_wait(get_cond(c), get_mutex(m)); }
THUNK(pthread_cond_wait, t_cond_wait);
int t_cond_timedwait(u32 c, u32 m, u32 abstime) {
    struct timespec ts{rd<s32>(abstime), rd<s32>(abstime + 4)};
    return pthread_cond_timedwait(get_cond(c), get_mutex(m), &ts);
}
THUNK(pthread_cond_timedwait, t_cond_timedwait);
int t_condattr_noop(u32) { return 0; }
THUNK(pthread_condattr_init, t_condattr_noop);
THUNK(pthread_condattr_destroy, t_condattr_noop);

// ---------------------------------------------------------------- semaphores
HandleTable<sem_t> g_sems;
sem_t *get_sem(u32 g) {
    u32 w = rd<u32>(g);
    if (!(w & HANDLE_BIT)) fatal("use of uninitialized guest semaphore at 0x%08x", g);
    return g_sems.objs[w & ~HANDLE_BIT];
}
int t_sem_init(u32 s, int, u32 value) {
    auto *hs = new sem_t;
    sem_init(hs, 0, value);
    wr<u32>(s, HANDLE_BIT | g_sems.add(hs));
    return 0;
}
THUNK(sem_init, t_sem_init);
int t_sem_destroy(u32 s) {
    u32 w = rd<u32>(s);
    if (w & HANDLE_BIT) {
        sem_destroy(g_sems.objs[w & ~HANDLE_BIT]);
        g_sems.remove(w & ~HANDLE_BIT);
    }
    wr<u32>(s, 0);
    return 0;
}
THUNK(sem_destroy, t_sem_destroy);
int sem_result(int r) {
    if (r < 0) cur_thread().set_errno(errno);
    return r;
}
int t_sem_wait(u32 s) {
    int r;
    while ((r = sem_wait(get_sem(s))) < 0 && errno == EINTR) {}
    return sem_result(r);
}
THUNK(sem_wait, t_sem_wait);
int t_sem_trywait(u32 s) { return sem_result(sem_trywait(get_sem(s))); }
THUNK(sem_trywait, t_sem_trywait);
int t_sem_post(u32 s) { return sem_result(sem_post(get_sem(s))); }
THUNK(sem_post, t_sem_post);
int t_sem_getvalue(u32 s, G<s32> v) {
    int x;
    int r = sem_getvalue(get_sem(s), &x);
    *v.get() = x;
    return sem_result(r);
}
THUNK(sem_getvalue, t_sem_getvalue);

// ---------------------------------------------------------------- threads
struct ThreadRec {
    GuestThread *t = nullptr;
    pthread_t host{};
    u32 start = 0, arg = 0, retval = 0;
    u32 stack_size = 0;
    bool detached = false, joinable = false;
};
HandleTable<ThreadRec> g_threads;
constexpr u32 THREAD_HANDLE_BASE = 0x1000;  // keep pthread_t values non-zero and distinct

ThreadRec *rec_of(u32 h) {
    if (h < THREAD_HANDLE_BASE || h - THREAD_HANDLE_BASE >= MAX_OBJS) return nullptr;
    return g_threads.objs[h - THREAD_HANDLE_BASE];
}

// pthread keys
struct Key {
    bool used = false;
    u32 dtor = 0;
};
std::mutex g_keys_lock;
std::vector<Key> g_keys;

void run_tls_destructors(GuestThread &t) {
    for (int iter = 0; iter < 4; iter++) {
        bool any = false;
        for (size_t k = 0; k < t.tls_values.size(); k++) {
            u32 v = t.tls_values[k], dtor;
            {
                std::lock_guard<std::mutex> lk(g_keys_lock);
                dtor = k < g_keys.size() && g_keys[k].used ? g_keys[k].dtor : 0;
            }
            if (v && dtor) {
                t.tls_values[k] = 0;
                cpu::call(dtor, {v});
                any = true;
            }
        }
        if (!any) break;
    }
}

void *thread_main(void *p) {
    u32 h = (u32)(uintptr_t)p;
    ThreadRec *rec = rec_of(h);
    cpu_assign_handle(h);
    GuestThread &t = cur_thread();
    rec->t = &t;
    if (rec->stack_size > t.stack_size) {
        mem::unmap(t.stack_base, t.stack_size);
        t.stack_size = (rec->stack_size + 0xfff) & ~0xfffu;
        t.stack_base = mem::map(t.stack_size);
    }
    u32 r;
    try {
        r = cpu::call32(rec->start, {rec->arg});
    } catch (GuestThreadExit &e) {
        r = e.value;
    }
    run_tls_destructors(t);
    rec->retval = r;
    rec->t = nullptr;
    destroy_cur_thread();
    if (rec->detached) g_threads.remove(h - THREAD_HANDLE_BASE);
    return nullptr;
}

int t_pthread_create(G<u32> out, u32 attr, u32 start, u32 arg) {
    auto *rec = new ThreadRec;
    rec->start = start;
    rec->arg = arg;
    if (attr) {
        rec->detached = rd<u32>(attr) & 1;
        rec->stack_size = rd<u32>(attr + 8);
    }
    rec->joinable = !rec->detached;
    u32 h = THREAD_HANDLE_BASE + g_threads.add(rec);
    *out.get() = h;
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, 2 * 1024 * 1024);
    if (rec->detached) pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&rec->host, &ha, thread_main, (void *)(uintptr_t)h);
    pthread_attr_destroy(&ha);
    if (r) {
        g_threads.remove(h - THREAD_HANDLE_BASE);
        return r;
    }
    return 0;
}
THUNK(pthread_create, t_pthread_create);

int t_pthread_join(u32 h, u32 ret) {
    ThreadRec *rec = rec_of(h);
    if (!rec || !rec->joinable) return ESRCH;
    pthread_join(rec->host, nullptr);
    if (ret) wr<u32>(ret, rec->retval);
    g_threads.remove(h - THREAD_HANDLE_BASE);
    return 0;
}
THUNK(pthread_join, t_pthread_join);
int t_pthread_detach(u32 h) {
    ThreadRec *rec = rec_of(h);
    if (!rec) return ESRCH;
    if (rec->joinable) pthread_detach(rec->host);
    rec->detached = true;
    rec->joinable = false;
    return 0;
}
THUNK(pthread_detach, t_pthread_detach);
[[noreturn]] void t_pthread_exit(u32 v) { throw GuestThreadExit{v}; }
THUNK(pthread_exit, t_pthread_exit);
u32 t_pthread_self() { return cur_thread().handle; }
THUNK(pthread_self, t_pthread_self);
int t_pthread_equal(u32 a, u32 b) { return a == b; }
THUNK(pthread_equal, t_pthread_equal);
int t_pthread_getschedparam(u32, G<s32> policy, G<s32> param) {
    if (policy) *policy.get() = SCHED_OTHER;
    if (param) *param.get() = 0;
    return 0;
}
THUNK(pthread_getschedparam, t_pthread_getschedparam);
int t_pthread_setschedparam(u32, int, u32) { return 0; }
THUNK(pthread_setschedparam, t_pthread_setschedparam);
int t_pthread_sigmask(int, u32, u32 old) {
    if (old) wr<u32>(old, 0);
    return 0;
}
THUNK(pthread_sigmask, t_pthread_sigmask);
int t_pthread_setname_np(u32, G<const char>) { return 0; }
THUNK(pthread_setname_np, t_pthread_setname_np);

// bionic LP32 pthread_attr_t: flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
int t_attr_init(u32 a) {
    wr<u32>(a + 0, 0);
    wr<u32>(a + 4, 0);
    wr<u32>(a + 8, 1024 * 1024);
    wr<u32>(a + 12, 4096);
    wr<s32>(a + 16, 0);
    wr<s32>(a + 20, 0);
    return 0;
}
THUNK(pthread_attr_init, t_attr_init);
int t_attr_destroy(u32) { return 0; }
THUNK(pthread_attr_destroy, t_attr_destroy);
int t_attr_setdetachstate(u32 a, int s) {
    wr<u32>(a, (rd<u32>(a) & ~1u) | (s ? 1u : 0u));
    return 0;
}
THUNK(pthread_attr_setdetachstate, t_attr_setdetachstate);
int t_attr_setstacksize(u32 a, u32 sz) {
    wr<u32>(a + 8, sz);
    return 0;
}
THUNK(pthread_attr_setstacksize, t_attr_setstacksize);
int t_attr_setschedparam(u32 a, u32 param) {
    wr<s32>(a + 20, rd<s32>(param));
    return 0;
}
THUNK(pthread_attr_setschedparam, t_attr_setschedparam);
int t_attr_setschedpolicy(u32 a, int p) {
    wr<s32>(a + 16, p);
    return 0;
}
THUNK(pthread_attr_setschedpolicy, t_attr_setschedpolicy);

int t_key_create(G<u32> key, u32 dtor) {
    std::lock_guard<std::mutex> lk(g_keys_lock);
    size_t i = 0;
    while (i < g_keys.size() && g_keys[i].used) i++;
    if (i == g_keys.size()) g_keys.emplace_back();
    g_keys[i] = {true, dtor};
    *key.get() = (u32)i;
    return 0;
}
THUNK(pthread_key_create, t_key_create);
int t_key_delete(u32 k) {
    std::lock_guard<std::mutex> lk(g_keys_lock);
    if (k < g_keys.size()) g_keys[k] = {};
    return 0;
}
THUNK(pthread_key_delete, t_key_delete);
u32 t_getspecific(u32 k) {
    auto &v = cur_thread().tls_values;
    return k < v.size() ? v[k] : 0;
}
THUNK(pthread_getspecific, t_getspecific);
int t_setspecific(u32 k, u32 val) {
    auto &v = cur_thread().tls_values;
    if (k >= v.size()) v.resize(k + 1);
    v[k] = val;
    return 0;
}
THUNK(pthread_setspecific, t_setspecific);

// pthread_once: 0 = not run, 1 = running, 2 = done
int t_pthread_once(u32 once, u32 fn) {
    s32 *w = (s32 *)g2h(once);
    s32 expected = 0;
    if (__atomic_compare_exchange_n(w, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        cpu::call(fn, {});
        __atomic_store_n(w, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(w, __ATOMIC_ACQUIRE) != 2) sched_yield();
    return 0;
}
THUNK(pthread_once, t_pthread_once);
}  // namespace

u32 pthread_register_foreign(GuestThread *t) {
    auto *rec = new ThreadRec;
    rec->t = t;
    rec->host = pthread_self();
    return THREAD_HANDLE_BASE + g_threads.add(rec);
}
