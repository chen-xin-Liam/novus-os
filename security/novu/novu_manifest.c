/**
 * Novus OS - update manifest codec (see novu_manifest.h).
 */
#include "novu_manifest.h"

static int starts(const char *p, const char *end, const char *pfx) {
    const char *q = pfx;
    while (*q) {
        if (p >= end || *p != *q) return 0;
        p++;
        q++;
    }
    return 1;
}

static int parse_u32(const char **pp, const char *end, UINT32 *out) {
    UINT64 v = 0;
    int any = 0;
    while (*pp < end && **pp >= '0' && **pp <= '9') {
        v = v * 10u + (UINT64)(**pp - '0');
        if (v > 0xFFFFFFFFu) return 0;
        (*pp)++;
        any = 1;
    }
    if (!any) return 0;
    *out = (UINT32)v;
    return 1;
}

INT32 novu_manifest_parse(const char *text, UINT32 len,
                          novus_manifest_entry_t *out, UINT32 cap) {
    const char *p = text;
    const char *end = text + len;
    UINT32 n = 0;

    while (p < end && n < cap) {
        const char *line = p;
        const char *le;
        novus_manifest_entry_t e;
        __builtin_memset(&e, 0, sizeof(e));

        while (p < end && *p != '\n') p++;
        le = p;
        if (p < end) p++; /* skip newline */

        if (le == line) continue;

        if (starts(line, le, "PUT ")) {
            const char *q = line + 4;
            UINT32 off = 0, len = 0;
            if (!parse_u32(&q, le, &off)) continue;
            if (q < le && *q == ' ') q++; else continue;
            if (!parse_u32(&q, le, &len)) continue;
            if (q < le && *q == ' ') q++; else continue;
            e.off = off;
            e.len = len;
            e.op = NOVU_MANIFEST_PUT;
            {
                UINT32 dl = (UINT32)(le - q);
                if (dl > 120) dl = 120;
                __builtin_memcpy(e.dst, q, dl);
                e.dst_len = (UINT8)dl;
            }
            out[n++] = e;
        } else if (starts(line, le, "DEL ")) {
            const char *q = line + 4;
            UINT32 dl = (UINT32)(le - q);
            if (dl > 120) dl = 120;
            e.op = NOVU_MANIFEST_DEL;
            __builtin_memcpy(e.dst, q, dl);
            e.dst_len = (UINT8)dl;
            out[n++] = e;
        }
    }
    return (INT32)n;
}

static UINT32 put_dec(char *b, UINT32 cap, UINT32 pos, UINT32 v) {
    char tmp[12];
    int k = 0;
    if (v == 0) {
        if (pos < cap) b[pos++] = '0';
        return pos;
    }
    while (v > 0) { tmp[k++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (k > 0) { if (pos >= cap) break; b[pos++] = tmp[--k]; }
    return pos;
}

UINT32 novu_manifest_serialize(const novus_manifest_entry_t *e, UINT32 n,
                               char *buf, UINT32 cap) {
    UINT32 pos = 0;
    UINT32 i;
    if (buf == (char *)0 || cap == 0) return 0;
    for (i = 0; i < n; i++) {
        UINT32 dl = e[i].dst_len;
        if (e[i].op == NOVU_MANIFEST_PUT) {
            if (pos + 4 + 12 + 12 + dl + 2 >= cap) return 0;
            __builtin_memcpy(buf + pos, "PUT ", 4); pos += 4;
            pos = put_dec(buf, cap, pos, e[i].off);
            if (pos < cap) buf[pos++] = ' ';
            pos = put_dec(buf, cap, pos, e[i].len);
            if (pos < cap) buf[pos++] = ' ';
            __builtin_memcpy(buf + pos, e[i].dst, dl); pos += dl;
        } else {
            if (pos + 4 + dl + 1 >= cap) return 0;
            __builtin_memcpy(buf + pos, "DEL ", 4); pos += 4;
            __builtin_memcpy(buf + pos, e[i].dst, dl); pos += dl;
        }
        if (pos < cap) buf[pos++] = '\n';
    }
    if (pos < cap) buf[pos] = 0;
    return pos;
}
