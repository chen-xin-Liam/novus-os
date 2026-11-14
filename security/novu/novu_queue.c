/**
 * Novus OS - five-level priority update queue (see novu_queue.h).
 */
#include "novu_queue.h"

static novu_queue_entry_t g_queue[NOVU_QUEUE_MAX];
static UINT32 g_count;
static UINT8 g_dev_visible; /* 0 = dev channel hidden */

void novu_queue_init(void) {
    g_count = 0;
    g_dev_visible = 0;
    __builtin_memset(g_queue, 0, sizeof(g_queue));
}

static int uuid_eq(const UINT8 *a, const UINT8 *b) {
    return __builtin_memcmp(a, b, 8) == 0;
}

static UINT32 novu_strlen(const char *s) {
    UINT32 n = 0;
    if (s == (const char *)0) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

INT32 novu_queue_rank(const novus_update_header_t *h) {
    /* 5 = highest. Hot patch forces urgent regardless of level field. */
    if (h->file_type == NOVU_TYPE_HOT_PATCH || h->update_level == UPDATE_LVL_URGENT) {
        return 5;
    }
    if (h->update_level == UPDATE_LVL_RECOMMEND) {
        return 4;
    }
    if (h->update_level == UPDATE_LVL_PREVIEW) {
        return 3;
    }
    if (h->update_level == UPDATE_LVL_OPTIONAL) {
        return 2;
    }
    return 1; /* DEV */
}

INT32 novu_queue_add(const novus_update_header_t *h, const char *src,
                     const char *name) {
    UINT32 i;
    INT32 rank = novu_queue_rank(h);

    if (g_count >= NOVU_QUEUE_MAX) {
        return -1;
    }
    /* de-duplicate by uuid */
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_queue[i].uuid, h->patch_uuid_bin)) {
            return (INT32)i;
        }
    }
    /* insert keeping descending rank, FIFO within a rank */
    {
        UINT32 j = g_count;
        while (j > 0) {
            novus_update_header_t hj;
            __builtin_memset(&hj, 0, sizeof(hj));
            hj.file_type = g_queue[j - 1].type;
            hj.update_level = g_queue[j - 1].level;
            if (rank <= novu_queue_rank(&hj)) {
                break;
            }
            g_queue[j] = g_queue[j - 1];
            j--;
        }
        {
            novu_queue_entry_t *e = &g_queue[j];
            __builtin_memset(e, 0, sizeof(*e));
            __builtin_memcpy(e->uuid, h->patch_uuid_bin, 8);
            e->type = h->file_type;
            e->level = h->update_level;
            e->state = 0;
            e->data_len = h->data_len;
            e->after_ver = h->target_os_ver;
            if (name) {
                UINT32 n = novu_strlen(name);
                if (n > 47) n = 47;
                __builtin_memcpy(e->name, name, n);
            }
            if (src) {
                UINT32 n = novu_strlen(src);
                if (n > 199) n = 199;
                __builtin_memcpy(e->src, src, n);
            }
            /* dev channel entries start hidden unless dev mode is on */
            if (h->update_level == UPDATE_LVL_DEV && !g_dev_visible) {
                e->hidden_dev = 1;
            }
        }
        g_count++;
    }
    return (INT32)g_count - 1;
}

INT32 novu_queue_next(UINT8 *uuid_out) {
    UINT32 i;
    INT32 best = -1;
    INT32 best_rank = -1;
    for (i = 0; i < g_count; i++) {
        novu_queue_entry_t *e = &g_queue[i];
        if (e->deferred || e->ignored || e->hidden_dev) {
            continue;
        }
        {
            novus_update_header_t h;
            __builtin_memset(&h, 0, sizeof(h));
            h.file_type = e->type;
            h.update_level = e->level;
            INT32 r = novu_queue_rank(&h);
            if (r > best_rank) {
                best_rank = r;
                best = (INT32)i;
            }
        }
    }
    if (best < 0) {
        return -1;
    }
    if (uuid_out) {
        __builtin_memcpy(uuid_out, g_queue[best].uuid, 8);
    }
    return best;
}

void novu_queue_remove(const UINT8 *uuid) {
    UINT32 i;
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_queue[i].uuid, uuid)) {
            UINT32 j;
            for (j = i; j + 1 < g_count; j++) {
                g_queue[j] = g_queue[j + 1];
            }
            __builtin_memset(&g_queue[g_count - 1], 0, sizeof(g_queue[0]));
            g_count--;
            return;
        }
    }
}

void novu_queue_defer(const UINT8 *uuid) {
    UINT32 i;
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_queue[i].uuid, uuid)) {
            /* urgent / hotfix may never be deferred */
            if (g_queue[i].level == UPDATE_LVL_URGENT ||
                g_queue[i].type == NOVU_TYPE_HOT_PATCH) {
                return;
            }
            g_queue[i].deferred = 1;
            return;
        }
    }
}

void novu_queue_ignore(const UINT8 *uuid) {
    UINT32 i;
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_queue[i].uuid, uuid)) {
            if (g_queue[i].type == NOVU_TYPE_HOT_PATCH) {
                return; /* hotfix cannot be ignored */
            }
            g_queue[i].ignored = 1;
            return;
        }
    }
}

void novu_queue_set_dev_visible(UINT8 visible) {
    UINT32 i;
    g_dev_visible = visible ? 1 : 0;
    for (i = 0; i < g_count; i++) {
        if (g_queue[i].level == UPDATE_LVL_DEV) {
            g_queue[i].hidden_dev = g_dev_visible ? 0 : 1;
        }
    }
}

UINT32 novu_queue_count(void) {
    return g_count;
}

const novu_queue_entry_t *novu_queue_get(UINT32 i) {
    if (i >= g_count) {
        return (const novu_queue_entry_t *)0;
    }
    return &g_queue[i];
}
