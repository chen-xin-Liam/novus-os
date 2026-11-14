/**
 * Novus OS - update index registry (see novu_index.h).
 *
 * Wire format (one record per line, space separated):
 *   <uuid16hex> <type> <level> <state> <lock> <installed> <expires>
 *   <before> <after> <revocable> <namehex>
 * namehex is 0..48 bytes hex-encoded (so names with spaces round-trip).
 */
#include "novu_index.h"

static novus_index_record_t g_index[NOVU_INDEX_MAX];
static UINT32 g_count;

void novu_index_init(void) {
    g_count = 0;
    __builtin_memset(g_index, 0, sizeof(g_index));
}

static int uuid_eq(const UINT8 *a, const UINT8 *b) {
    return __builtin_memcmp(a, b, 8) == 0;
}

INT32 novu_index_add(const novus_index_record_t *rec) {
    UINT32 i;
    if (rec == (const novus_index_record_t *)0) {
        return -1;
    }
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_index[i].uuid, rec->uuid)) {
            g_index[i] = *rec; /* replace keeps history stable */
            return (INT32)i;
        }
    }
    if (g_count >= NOVU_INDEX_MAX) {
        return -1;
    }
    g_index[g_count] = *rec;
    return (INT32)g_count++;
}

const novus_index_record_t *novu_index_find(const UINT8 *uuid) {
    UINT32 i;
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_index[i].uuid, uuid)) {
            return &g_index[i];
        }
    }
    return (const novus_index_record_t *)0;
}

INT32 novu_index_remove(const UINT8 *uuid) {
    UINT32 i;
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_index[i].uuid, uuid)) {
            UINT32 j;
            for (j = i; j + 1 < g_count; j++) {
                g_index[j] = g_index[j + 1];
            }
            __builtin_memset(&g_index[g_count - 1], 0, sizeof(g_index[0]));
            g_count--;
            return 0;
        }
    }
    return -1;
}

INT32 novu_index_set_state(const UINT8 *uuid, UINT8 state) {
    UINT32 i;
    for (i = 0; i < g_count; i++) {
        if (uuid_eq(g_index[i].uuid, uuid)) {
            g_index[i].state = state;
            return 0;
        }
    }
    return -1;
}

UINT32 novu_index_count(void) {
    return g_count;
}

const novus_index_record_t *novu_index_get(UINT32 i) {
    if (i >= g_count) {
        return (const novus_index_record_t *)0;
    }
    return &g_index[i];
}

/* ---- text codec helpers ---- */

static UINT32 put_dec(char *b, UINT32 cap, UINT32 pos, UINT64 v) {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        if (pos < cap) b[pos++] = '0';
        return pos;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        if (pos >= cap) break;
        b[pos++] = tmp[--n];
    }
    return pos;
}

static char hexd(UINT8 x) {
    return (char)(x < 10 ? ('0' + x) : ('a' + (x - 10)));
}

static UINT32 put_hex(char *b, UINT32 cap, UINT32 pos, const UINT8 *p,
                      UINT32 len) {
    UINT32 i;
    for (i = 0; i < len; i++) {
        if (pos + 1 >= cap) break;
        b[pos++] = hexd((UINT8)(p[i] >> 4));
        b[pos++] = hexd((UINT8)(p[i] & 0x0F));
    }
    return pos;
}

UINT32 novu_index_serialize(char *buf, UINT32 cap) {
    UINT32 pos = 0;
    UINT32 i;
    if (buf == (char *)0 || cap == 0) {
        return 0;
    }
    for (i = 0; i < g_count; i++) {
        const novus_index_record_t *r = &g_index[i];
        UINT32 nlen = 0;
        while (nlen < 48 && r->name[nlen] != 0) nlen++;
        pos = put_hex(buf, cap, pos, r->uuid, 8);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->type);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->level);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->state);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->lock);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->installed_at);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->expires_at);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->before_ver);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->after_ver);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_dec(buf, cap, pos, r->revocable);
        if (pos < cap) buf[pos++] = ' ';
        pos = put_hex(buf, cap, pos, (const UINT8 *)r->name, nlen);
        if (pos < cap) buf[pos++] = '\n';
        if (pos >= cap) {
            return 0; /* truncated: caller must enlarge */
        }
    }
    if (pos < cap) buf[pos] = 0;
    return pos;
}

static UINT64 parse_dec(const char **pp) {
    UINT64 v = 0;
    while (**pp >= '0' && **pp <= '9') {
        v = v * 10u + (UINT64)(**pp - '0');
        (*pp)++;
    }
    return v;
}

static UINT8 hexval(char c) {
    if (c >= '0' && c <= '9') return (UINT8)(c - '0');
    if (c >= 'a' && c <= 'f') return (UINT8)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (UINT8)(c - 'A' + 10);
    return 0;
}

static void parse_hex(const char **pp, UINT8 *out, UINT32 len) {
    UINT32 i;
    for (i = 0; i < len; i++) {
        UINT8 hi = hexval((*pp)[0]);
        UINT8 lo = hexval((*pp)[1]);
        out[i] = (UINT8)((hi << 4) | lo);
        *pp += 2;
    }
}

INT32 novu_index_parse(const char *buf, UINT32 len) {
    const char *p = buf;
    const char *end = buf + len;
    novu_index_init();
    while (p < end) {
        novus_index_record_t r;
        if (*p == '\n') { p++; continue; }
        if (*p == 0) break;
        __builtin_memset(&r, 0, sizeof(r));
        parse_hex(&p, r.uuid, 8);
        if (*p == ' ') p++;
        r.type = (UINT8)parse_dec(&p);
        if (*p == ' ') p++;
        r.level = (UINT8)parse_dec(&p);
        if (*p == ' ') p++;
        r.state = (UINT8)parse_dec(&p);
        if (*p == ' ') p++;
        r.lock = (UINT8)parse_dec(&p);
        if (*p == ' ') p++;
        r.installed_at = (UINT32)parse_dec(&p);
        if (*p == ' ') p++;
        r.expires_at = (UINT32)parse_dec(&p);
        if (*p == ' ') p++;
        r.before_ver = parse_dec(&p);
        if (*p == ' ') p++;
        r.after_ver = parse_dec(&p);
        if (*p == ' ') p++;
        r.revocable = (UINT8)parse_dec(&p);
        if (*p == ' ') p++;
        {
            UINT32 n = 0;
            while (p + 1 < end && *p != '\n' && *p != 0 && n < 48) {
                r.name[n++] = (char)((hexval(p[0]) << 4) | hexval(p[1]));
                p += 2;
            }
        }
        if (g_count < NOVU_INDEX_MAX) {
            g_index[g_count++] = r;
        }
        if (p < end && *p == '\n') p++;
    }
    return (INT32)g_count;
}
