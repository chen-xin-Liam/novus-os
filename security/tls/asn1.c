/**
 * Novus OS - bounded DER reader (see asn1.h).
 */
#include "asn1.h"

void asn1_init(asn1_reader *r, const UINT8 *buf, UINT32 len) {
    r->p = buf;
    r->end = buf + len;
}

static int read_len(const UINT8 **p, const UINT8 *end, UINT32 *len) {
    UINT8 b;
    if (*p >= end) return -1;
    b = *(*p)++;
    if (b < 0x80u) {
        *len = b;
        return 0;
    }
    {
        UINT8 n = (UINT8)(b & 0x7fu);
        UINT32 v = 0;
        UINT8 i;
        if (n == 0 || n > 4) return -1; /* indefinite or too large */
        for (i = 0; i < n; i++) {
            if (*p >= end) return -1;
            v = (v << 8) | *(*p)++;
        }
        *len = v;
        return 0;
    }
}

int asn1_next(asn1_reader *r, UINT8 *tag, const UINT8 **val, UINT32 *len) {
    UINT8 t;
    UINT32 l;
    if (r->p >= r->end) return -1;
    t = *r->p++;
    if (read_len(&r->p, r->end, &l) != 0) return -1;
    if (l > (UINT32)(r->end - r->p)) return -1;
    *tag = t;
    *val = r->p;
    *len = l;
    r->p += l;
    return 0;
}

int asn1_enter(asn1_reader *r, UINT8 tag, asn1_reader *out) {
    UINT8 t;
    const UINT8 *v;
    UINT32 l;
    if (asn1_next(r, &t, &v, &l) != 0) return -1;
    if (t != tag) return -1;
    out->p = v;
    out->end = v + l;
    return 0;
}

int asn1_next_full(asn1_reader *r, UINT8 *tag, const UINT8 **val, UINT32 *len,
                   const UINT8 **tlv, UINT32 *tlv_len) {
    const UINT8 *start = r->p;
    const UINT8 *v;
    UINT32 l;
    if (asn1_next(r, tag, &v, &l) != 0) return -1;
    *val = v;
    *len = l;
    *tlv = start;
    *tlv_len = (UINT32)(r->p - start);
    return 0;
}

int asn1_peek(const asn1_reader *r, UINT8 *tag) {
    if (r->p >= r->end) return -1;
    *tag = *r->p;
    return 0;
}

int asn1_uint(const UINT8 *val, UINT32 len, UINT64 *out) {
    UINT64 v = 0;
    UINT32 i = 0;
    if (val == (const UINT8 *)0 || out == (UINT64 *)0) return -1;
    if (len == 0) { *out = 0; return 0; }
    if (len > 8) return -1;
    for (i = 0; i < len; i++) v = (v << 8) | val[i];
    *out = v;
    return 0;
}
