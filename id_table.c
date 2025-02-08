/* This file is included by symbol.c */

#include "id_table.h"
#include <string.h>

#ifndef ID_TABLE_DEBUG
#define ID_TABLE_DEBUG 0
#endif

#if ID_TABLE_DEBUG == 0
#undef NDEBUG
#define NDEBUG
#endif
#include "ruby_assert.h"
#include <pthread.h>

typedef rb_id_serial_t id_key_t;

static inline ID
key2id(id_key_t key)
{
    return rb_id_serial_to_id(key);
}

static inline id_key_t
id2key(ID id)
{
    return rb_id_to_serial(id);
}

/* simple open addressing with quadratic probing.
   uses mark-bit on collisions - need extra 1 bit,
   ID is strictly 3 bits larger than rb_id_serial_t */

typedef struct rb_id_item {
    id_key_t key;
#if SIZEOF_VALUE == 8
    int      collision;
#endif
    VALUE    val;
} item_t;

struct rb_id_table {
    int capa;
    int num;
    int used;
    item_t *items;
    pthread_mutex_t *mutex;
};

#if SIZEOF_VALUE == 8
#define ITEM_GET_KEY(tbl, i) ((tbl)->items[i].key)
#define ITEM_KEY_ISSET(tbl, i) ((tbl)->items[i].key)
#define ITEM_COLLIDED(tbl, i) ((tbl)->items[i].collision)
#define ITEM_SET_COLLIDED(tbl, i) ((tbl)->items[i].collision = 1)
static inline void
ITEM_SET_KEY(struct rb_id_table *tbl, int i, id_key_t key)
{
    tbl->items[i].key = key;
}
#else
#define ITEM_GET_KEY(tbl, i) ((tbl)->items[i].key >> 1)
#define ITEM_KEY_ISSET(tbl, i) ((tbl)->items[i].key > 1)
#define ITEM_COLLIDED(tbl, i) ((tbl)->items[i].key & 1)
#define ITEM_SET_COLLIDED(tbl, i) ((tbl)->items[i].key |= 1)
static inline void
ITEM_SET_KEY(struct rb_id_table *tbl, int i, id_key_t key)
{
    tbl->items[i].key = (key << 1) | ITEM_COLLIDED(tbl, i);
}
#endif

static inline int
round_capa(int capa)
{
    /* minsize is 4 */
    capa >>= 2;
    capa |= capa >> 1;
    capa |= capa >> 2;
    capa |= capa >> 4;
    capa |= capa >> 8;
    capa |= capa >> 16;
    return (capa + 1) << 2;
}

static struct rb_id_table *
rb_id_table_init(struct rb_id_table *tbl, int capa)
{
    MEMZERO(tbl, struct rb_id_table, 1);
    if (capa > 0) {
        capa = round_capa(capa);
        tbl->capa = (int)capa;
        tbl->items = ZALLOC_N(item_t, capa);
    }
    tbl->mutex = ALLOC(pthread_mutex_t);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    int err = pthread_mutex_init(tbl->mutex, &attr);
    // pthread_mutexattr_destroy(&attr);

    if (err) {
        fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(err));
    }
    return tbl;
}

struct rb_id_table *
rb_id_table_create(size_t capa)
{
    struct rb_id_table *tbl = ALLOC(struct rb_id_table);
    return rb_id_table_init(tbl, (int)capa);
}

void
rb_id_table_free(struct rb_id_table *tbl)
{
    pthread_mutex_destroy(tbl->mutex);
    xfree(tbl->mutex);
    xfree(tbl->items);
    xfree(tbl);
}

void
rb_id_table_clear(struct rb_id_table *tbl)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    tbl->num = 0;
    tbl->used = 0;
    MEMZERO(tbl->items, item_t, tbl->capa);
    pthread_mutex_unlock(tbl->mutex);
}

size_t
rb_id_table_size(const struct rb_id_table *tbl)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    size_t size = (size_t)tbl->num;
    pthread_mutex_unlock(tbl->mutex);
    return size;
}

size_t
rb_id_table_memsize(const struct rb_id_table *tbl)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    size_t size = sizeof(item_t) * tbl->capa + sizeof(struct rb_id_table);
    pthread_mutex_unlock(tbl->mutex);
    return size;
}

static int
hash_table_index(struct rb_id_table* tbl, id_key_t key)
{
    if (tbl->capa > 0) {
        int mask = tbl->capa - 1;
        int ix = key & mask;
        int d = 1;
        while (key != ITEM_GET_KEY(tbl, ix)) {
            if (!ITEM_COLLIDED(tbl, ix))
                return -1;
            ix = (ix + d) & mask;
            d++;
        }
        return ix;
    }
    return -1;
}

static void
hash_table_raw_insert(struct rb_id_table *tbl, id_key_t key, VALUE val)
{
    int mask = tbl->capa - 1;
    int ix = key & mask;
    int d = 1;
    RUBY_ASSERT(key != 0);
    while (ITEM_KEY_ISSET(tbl, ix)) {
        ITEM_SET_COLLIDED(tbl, ix);
        ix = (ix + d) & mask;
        d++;
    }
    tbl->num++;
    if (!ITEM_COLLIDED(tbl, ix)) {
        tbl->used++;
    }
    ITEM_SET_KEY(tbl, ix, key);
    tbl->items[ix].val = val;
}

static int
hash_delete_index(struct rb_id_table *tbl, int ix)
{
    if (ix >= 0) {
        if (!ITEM_COLLIDED(tbl, ix)) {
            tbl->used--;
        }
        tbl->num--;
        ITEM_SET_KEY(tbl, ix, 0);
        tbl->items[ix].val = 0;
        return TRUE;
    }
    else {
        return FALSE;
    }
}

static void
hash_table_extend(struct rb_id_table* tbl)
{
    if (tbl->used + (tbl->used >> 1) >= tbl->capa) {
        int new_cap = round_capa(tbl->num + (tbl->num >> 1));
        int i;
        item_t* old;
        struct rb_id_table tmp_tbl = {0};
        if (new_cap < tbl->capa) {
            new_cap = round_capa(tbl->used + (tbl->used >> 1));
        }
        tmp_tbl.capa = new_cap;
        tmp_tbl.items = ZALLOC_N(item_t, new_cap);
        tmp_tbl.mutex = tbl->mutex;

        for (i = 0; i < tbl->capa; i++) {
            id_key_t key = ITEM_GET_KEY(tbl, i);
            if (key != 0) {
                hash_table_raw_insert(&tmp_tbl, key, tbl->items[i].val);
            }
        }
        old = tbl->items;
        *tbl = tmp_tbl;
        xfree(old);
    }
}

#if ID_TABLE_DEBUG && 0
static void
hash_table_show(struct rb_id_table *tbl)
{
    const id_key_t *keys = tbl->keys;
    const int capa = tbl->capa;
    int i;

    fprintf(stderr, "tbl: %p (capa: %d, num: %d, used: %d)\n", tbl, tbl->capa, tbl->num, tbl->used);
    for (i=0; i<capa; i++) {
        if (ITEM_KEY_ISSET(tbl, i)) {
            fprintf(stderr, " -> [%d] %s %d\n", i, rb_id2name(key2id(keys[i])), (int)keys[i]);
        }
    }
}
#endif

int
rb_id_table_lookup(struct rb_id_table *tbl, ID id, VALUE *valp)
{
    int ret;
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    id_key_t key = id2key(id);
    int index = hash_table_index(tbl, key);
    if (index >= 0) {
        *valp = tbl->items[index].val;
        ret = TRUE;
    }
    else {
        ret = FALSE;
    }
    pthread_mutex_unlock(tbl->mutex);
    return ret;
}

static int
rb_id_table_insert_key(struct rb_id_table *tbl, const id_key_t key, const VALUE val)
{
    const int index = hash_table_index(tbl, key);

    if (index >= 0) {
        tbl->items[index].val = val;
    }
    else {
        hash_table_extend(tbl);
        hash_table_raw_insert(tbl, key, val);
    }
    return TRUE;
}

int
rb_id_table_insert(struct rb_id_table *tbl, ID id, VALUE val)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    int ret = rb_id_table_insert_key(tbl, id2key(id), val);
    pthread_mutex_unlock(tbl->mutex);
    return ret;
}

int
rb_id_table_delete(struct rb_id_table *tbl, ID id)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    const id_key_t key = id2key(id);
    int index = hash_table_index(tbl, key);
    int ret = hash_delete_index(tbl, index);
    pthread_mutex_unlock(tbl->mutex);
    return ret;
}

void
rb_id_table_foreach(struct rb_id_table *tbl, rb_id_table_foreach_func_t *func, void *data)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    int i, capa = tbl->capa;

    for (i=0; i<capa; i++) {
        if (ITEM_KEY_ISSET(tbl, i)) {
            const id_key_t key = ITEM_GET_KEY(tbl, i);
            enum rb_id_table_iterator_result ret = (*func)(key2id(key), tbl->items[i].val, data);
            RUBY_ASSERT(key != 0);

            if (ret == ID_TABLE_DELETE) {
                hash_delete_index(tbl, i);
            }
            else if (ret == ID_TABLE_STOP) {
                pthread_mutex_unlock(tbl->mutex);
                return;
            }
        }
    }
    pthread_mutex_unlock(tbl->mutex);
}

void
rb_id_table_foreach_values(struct rb_id_table *tbl, rb_id_table_foreach_values_func_t *func, void *data)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    int i, capa = tbl->capa;

    for (i=0; i<capa; i++) {
        if (ITEM_KEY_ISSET(tbl, i)) {
            enum rb_id_table_iterator_result ret = (*func)(tbl->items[i].val, data);

            if (ret == ID_TABLE_DELETE) {
                hash_delete_index(tbl, i);
            }
            else if (ret == ID_TABLE_STOP) {
                pthread_mutex_unlock(tbl->mutex);
                return;
            }
        }
    }
    pthread_mutex_unlock(tbl->mutex);
}

void
rb_id_table_foreach_values_with_replace(struct rb_id_table *tbl, rb_id_table_foreach_values_func_t *func, rb_id_table_update_value_callback_func_t *replace, void *data)
{
    int err = pthread_mutex_lock(tbl->mutex);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
    }
    int i, capa = tbl->capa;

    for (i = 0; i < capa; i++) {
        if (ITEM_KEY_ISSET(tbl, i)) {
            enum rb_id_table_iterator_result ret = (*func)(tbl->items[i].val, data);

            if (ret == ID_TABLE_REPLACE) {
                VALUE val = tbl->items[i].val;
                ret = (*replace)(&val, data, TRUE);
                tbl->items[i].val = val;
            }

            if (ret == ID_TABLE_STOP) {
                pthread_mutex_unlock(tbl->mutex);
                return;
            }
        }
    }
    pthread_mutex_unlock(tbl->mutex);
}

