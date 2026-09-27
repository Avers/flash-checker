#include "flashcheck/pool.h"

#include <pthread.h>

#include "flashcheck/util.h"

enum { SLOT_FREE = 0, SLOT_FILLING = 1, SLOT_READY = 2, SLOT_BUSY = 3 };

typedef struct {
    uint8_t *buf;
    uint64_t off;
    uint32_t pass;
    int state;
} pool_slot;

struct gen_pool {
    pattern_kind kind;
    uint64_t test_id;
    uint64_t chunk;
    uint64_t capacity;
    int nslots;
    int has_worker;
    pool_slot *slots;
    pthread_mutex_t mu;
    pthread_cond_t cv_work;
    pthread_cond_t cv_done;
    pthread_t worker;
    int stop;
    int req_valid;
    uint64_t req_off;
    uint32_t req_pass;
    int fill_valid;
    uint64_t fill_off;
    uint32_t fill_pass;
    uint64_t gen_ns;
};

static size_t pool_len(const gen_pool *g, uint64_t off)
{
    uint64_t rem = g->capacity - off;
    return (size_t)FC_MIN(g->chunk, rem);
}

static void pool_fill(gen_pool *g, pool_slot *s, uint64_t off, uint32_t pass)
{
    pattern_id id;
    uint64_t t0, dt;

    id.test_id = g->test_id;
    id.pass = pass;
    id.chunk_index = off / g->chunk;
    t0 = now_ns();
    pattern_fill(g->kind, s->buf, pool_len(g, off), &id, 0);
    dt = now_ns() - t0;
    __atomic_add_fetch(&g->gen_ns, dt, __ATOMIC_RELAXED);
}

static int find_locked(const gen_pool *g, int state, uint64_t off, uint32_t pass)
{
    for (int i = 0; i < g->nslots; i++) {
        if (g->slots[i].state != state)
            continue;
        if (state == SLOT_READY && (g->slots[i].off != off || g->slots[i].pass != pass))
            continue;
        return i;
    }
    return -1;
}

static int find_state_locked(const gen_pool *g, int state)
{
    for (int i = 0; i < g->nslots; i++) {
        if (g->slots[i].state == state)
            return i;
    }
    return -1;
}

static void *worker_main(void *arg)
{
    gen_pool *g = arg;

    pthread_mutex_lock(&g->mu);
    for (;;) {
        int si;
        uint64_t off;
        uint32_t pass;

        while (!g->stop && !g->req_valid)
            pthread_cond_wait(&g->cv_work, &g->mu);
        if (g->stop)
            break;

        off = g->req_off;
        pass = g->req_pass;
        g->req_valid = 0;

        si = find_state_locked(g, SLOT_FREE);
        if (si < 0)
            si = find_state_locked(g, SLOT_READY);
        if (si < 0) {
            g->req_valid = 1;
            g->req_off = off;
            g->req_pass = pass;
            pthread_cond_broadcast(&g->cv_done);
            pthread_cond_wait(&g->cv_done, &g->mu);
            continue;
        }

        g->slots[si].state = SLOT_FILLING;
        g->fill_valid = 1;
        g->fill_off = off;
        g->fill_pass = pass;
        pthread_mutex_unlock(&g->mu);

        pool_fill(g, &g->slots[si], off, pass);

        pthread_mutex_lock(&g->mu);
        g->slots[si].state = SLOT_READY;
        g->slots[si].off = off;
        g->slots[si].pass = pass;
        g->fill_valid = 0;
        pthread_cond_broadcast(&g->cv_done);
    }
    pthread_mutex_unlock(&g->mu);
    return NULL;
}

gen_pool *gen_pool_create(pattern_kind kind, uint64_t test_id, uint64_t chunk, uint64_t capacity,
                          int depth)
{
    gen_pool *g;

    if (depth < 0)
        depth = 0;
    if (depth > GEN_POOL_MAX_DEPTH)
        depth = GEN_POOL_MAX_DEPTH;

    g = xalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->kind = kind;
    g->test_id = test_id;
    g->chunk = chunk;
    g->capacity = capacity;
    g->nslots = depth >= 2 ? depth : 1;
    g->slots = xalloc((size_t)g->nslots * sizeof *g->slots);
    memset(g->slots, 0, (size_t)g->nslots * sizeof *g->slots);
    for (int i = 0; i < g->nslots; i++)
        g->slots[i].buf = xalloc_aligned(FC_ALIGN, (size_t)chunk);

    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv_work, NULL);
    pthread_cond_init(&g->cv_done, NULL);

    if (g->nslots >= 2 && pthread_create(&g->worker, NULL, worker_main, g) == 0)
        g->has_worker = 1;
    else if (g->nslots >= 2)
        log_warn("cannot start generator thread, falling back to inline generation");

    return g;
}

void gen_pool_destroy(gen_pool *g)
{
    if (g == NULL)
        return;
    if (g->has_worker) {
        pthread_mutex_lock(&g->mu);
        g->stop = 1;
        pthread_cond_broadcast(&g->cv_work);
        pthread_mutex_unlock(&g->mu);
        pthread_join(g->worker, NULL);
    }
    pthread_mutex_destroy(&g->mu);
    pthread_cond_destroy(&g->cv_work);
    pthread_cond_destroy(&g->cv_done);
    for (int i = 0; i < g->nslots; i++)
        free(g->slots[i].buf);
    free(g->slots);
    free(g);
}

void gen_pool_prefetch(gen_pool *g, uint64_t off, uint32_t pass)
{
    if (g == NULL || !g->has_worker || off >= g->capacity)
        return;

    pthread_mutex_lock(&g->mu);
    for (int i = 0; i < g->nslots; i++) {
        if (g->slots[i].state == SLOT_READY && g->slots[i].off == off &&
            g->slots[i].pass == pass) {
            pthread_mutex_unlock(&g->mu);
            return;
        }
    }
    g->req_valid = 1;
    g->req_off = off;
    g->req_pass = pass;
    pthread_cond_signal(&g->cv_work);
    pthread_mutex_unlock(&g->mu);
}

uint8_t *gen_pool_take(gen_pool *g, uint64_t off, uint32_t pass)
{
    int si;

    if (g == NULL)
        return NULL;

    pthread_mutex_lock(&g->mu);
    for (;;) {
        si = find_locked(g, SLOT_READY, off, pass);
        if (si >= 0) {
            g->slots[si].state = SLOT_BUSY;
            pthread_mutex_unlock(&g->mu);
            return g->slots[si].buf;
        }

        if (g->fill_valid && g->fill_off == off && g->fill_pass == pass) {
            pthread_cond_wait(&g->cv_done, &g->mu);
            continue;
        }

        si = find_state_locked(g, SLOT_FREE);
        if (si < 0)
            si = find_state_locked(g, SLOT_READY);
        if (si >= 0) {
            g->slots[si].state = SLOT_FILLING;
            g->slots[si].off = off;
            g->slots[si].pass = pass;
            pthread_mutex_unlock(&g->mu);

            pool_fill(g, &g->slots[si], off, pass);

            pthread_mutex_lock(&g->mu);
            g->slots[si].state = SLOT_BUSY;
            pthread_mutex_unlock(&g->mu);
            return g->slots[si].buf;
        }

        pthread_cond_wait(&g->cv_done, &g->mu);
    }
}

void gen_pool_release(gen_pool *g, uint8_t *buf)
{
    if (g == NULL || buf == NULL)
        return;

    pthread_mutex_lock(&g->mu);
    for (int i = 0; i < g->nslots; i++) {
        if (g->slots[i].buf == buf) {
            g->slots[i].state = SLOT_FREE;
            break;
        }
    }
    pthread_cond_broadcast(&g->cv_done);
    pthread_mutex_unlock(&g->mu);
}

uint64_t gen_pool_gen_ns(gen_pool *g)
{
    return g == NULL ? 0 : __atomic_load_n(&g->gen_ns, __ATOMIC_RELAXED);
}
