/**
 * Novus OS - minimal X.509 parser + signature verify (see x509.h).
 */
#include "x509.h"
#include "asn1.h"
#include "rsa.h"
#include "p256.h"

static const UINT8 OID_RSA_SHA256[9] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const UINT8 OID_ECDSA_SHA256[8] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const UINT8 OID_BASIC_CONSTRAINTS[3] = {0x55,0x1d,0x13};
static const UINT8 OID_SUBJECT_ALT_NAME[3] = {0x55,0x1d,0x11};

static x509_sig_t classify_sig_alg(const UINT8 *oid, UINT32 len) {
    if (len == 9 && __builtin_memcmp(oid, OID_RSA_SHA256, 9) == 0) {
        return X509_SIG_RSA_PKCS1_SHA256;
    }
    if (len == 8 && __builtin_memcmp(oid, OID_ECDSA_SHA256, 8) == 0) {
        return X509_SIG_ECDSA_SHA256;
    }
    return X509_SIG_UNKNOWN;
}

static void parse_extensions(const UINT8 *p, UINT32 len, x509_cert *c) {
    asn1_reader er, seq;
    UINT8 tag;
    asn1_init(&er, p, len);
    if (asn1_enter(&er, ASN1_SEQUENCE, &seq) != 0) return;
    while (asn1_peek(&seq, &tag) == 0) {
        asn1_reader ext;
        const UINT8 *oid, *val;
        UINT32 oid_len, val_len;
        if (asn1_enter(&seq, ASN1_SEQUENCE, &ext) != 0) break;
        if (asn1_next(&ext, &tag, &oid, &oid_len) != 0 || tag != ASN1_OID) continue;
        if (asn1_peek(&ext, &tag) == 0 && tag == ASN1_BOOLEAN) {
            const UINT8 *q;
            UINT32 l;
            asn1_next(&ext, &tag, &q, &l);
        }
        if (asn1_next(&ext, &tag, &val, &val_len) != 0 || tag != ASN1_OCTET_STRING) {
            continue;
        }
        if (oid_len == 3 && oid[0] == OID_BASIC_CONSTRAINTS[0] &&
            oid[1] == OID_BASIC_CONSTRAINTS[1] &&
            oid[2] == OID_BASIC_CONSTRAINTS[2]) {
            asn1_reader bc, bc2;
            asn1_init(&bc, val, val_len);
            if (asn1_enter(&bc, ASN1_SEQUENCE, &bc2) == 0) {
                if (asn1_peek(&bc2, &tag) == 0 && tag == ASN1_BOOLEAN) {
                    const UINT8 *q;
                    UINT32 l;
                    asn1_next(&bc2, &tag, &q, &l);
                    if (l >= 1 && q[0] != 0) c->is_ca = 1;
                }
            }
        } else if (oid_len == 3 && oid[0] == OID_SUBJECT_ALT_NAME[0] &&
                   oid[1] == OID_SUBJECT_ALT_NAME[1] &&
                   oid[2] == OID_SUBJECT_ALT_NAME[2]) {
            c->san = val;
            c->san_len = val_len;
        }
    }
}

INT32 tls_x509_parse(const UINT8 *der, UINT32 len, x509_cert *out) {
    asn1_reader top, cert, tbs_r, alg;
    const UINT8 *tlv, *val, *q;
    UINT32 tlv_len, vlen, l;
    UINT8 tag;

    if (der == (const UINT8 *)0 || out == (x509_cert *)0) return -1;
    __builtin_memset(out, 0, sizeof(*out));
    out->der = der;
    out->der_len = len;

    asn1_init(&top, der, len);
    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -1;

    /* tbsCertificate (whole TLV needed for the signature hash) */
    if (asn1_next_full(&cert, &tag, &val, &vlen, &tlv, &tlv_len) != 0 ||
        tag != ASN1_SEQUENCE) {
        return -1;
    }
    out->tbs = tlv;
    out->tbs_len = tlv_len;

    asn1_init(&tbs_r, val, vlen);
    /* optional [0] version */
    if (asn1_peek(&tbs_r, &tag) == 0 && tag == 0xA0u) {
        asn1_next(&tbs_r, &tag, &q, &l);
    }
    /* serialNumber */
    if (asn1_next(&tbs_r, &tag, &q, &l) != 0 || tag != ASN1_INTEGER) return -1;
    /* signature algorithm */
    if (asn1_next(&tbs_r, &tag, &q, &l) != 0 || tag != ASN1_SEQUENCE) return -1;
    /* issuer (whole Name TLV, for trust-anchor matching) */
    {
        const UINT8 *name_tlv;
        UINT32 name_tlv_len;
        if (asn1_next_full(&tbs_r, &tag, &q, &l, &name_tlv,
                           &name_tlv_len) != 0 || tag != ASN1_SEQUENCE) return -1;
        out->issuer = name_tlv;
        out->issuer_len = name_tlv_len;
    }
    /* validity */
    {
        asn1_reader v;
        if (asn1_enter(&tbs_r, ASN1_SEQUENCE, &v) != 0) return -1;
        if (asn1_next(&v, &tag, &q, &l) != 0) return -1;
        out->not_before = q;
        out->nb_len = l;
        if (asn1_next(&v, &tag, &q, &l) != 0) return -1;
        out->not_after = q;
        out->na_len = l;
    }
    /* subject (whole Name TLV) */
    {
        const UINT8 *name_tlv;
        UINT32 name_tlv_len;
        if (asn1_next_full(&tbs_r, &tag, &q, &l, &name_tlv,
                           &name_tlv_len) != 0 || tag != ASN1_SEQUENCE) return -1;
        out->subject = name_tlv;
        out->subject_len = name_tlv_len;
    }
    /* subjectPublicKeyInfo (whole TLV) */
    if (asn1_next_full(&tbs_r, &tag, &val, &vlen, &tlv, &tlv_len) != 0 ||
        tag != ASN1_SEQUENCE) {
        return -1;
    }
    out->spki = tlv;
    out->spki_len = tlv_len;

    /* optional extensions */
    while (asn1_peek(&tbs_r, &tag) == 0) {
        if (asn1_next(&tbs_r, &tag, &val, &vlen) != 0) break;
        if (tag == 0xA3u) {
            parse_extensions(val, vlen, out);
        }
    }

    /* outer signatureAlgorithm */
    if (asn1_enter(&cert, ASN1_SEQUENCE, &alg) != 0) return -1;
    {
        const UINT8 *oid;
        UINT32 oid_len;
        if (asn1_next(&alg, &tag, &oid, &oid_len) != 0 || tag != ASN1_OID) {
            return -1;
        }
        out->sig_alg = classify_sig_alg(oid, oid_len);
    }
    /* signatureValue BIT STRING (first byte is the unused-bit count) */
    if (asn1_next(&cert, &tag, &val, &vlen) != 0 || tag != ASN1_BIT_STRING) {
        return -1;
    }
    if (vlen < 1) return -1;
    out->sig = val + 1;
    out->sig_len = vlen - 1;
    return 0;
}

INT32 tls_x509_pubkey_rsa(const x509_cert *c, const UINT8 **n, UINT32 *n_len,
                          UINT32 *e) {
    asn1_reader spki, r, k;
    const UINT8 *q, *val;
    UINT32 l, vlen;
    UINT8 tag;

    if (c == (const x509_cert *)0 || c->spki == (const UINT8 *)0) return -1;
    asn1_init(&spki, c->spki, c->spki_len);
    if (asn1_enter(&spki, ASN1_SEQUENCE, &r) != 0) return -1; /* SPKI content */
    if (asn1_next(&r, &tag, &q, &l) != 0) return -1;          /* algorithm */
    if (asn1_next(&r, &tag, &val, &vlen) != 0 || tag != ASN1_BIT_STRING) return -1;
    if (vlen < 2) return -1;
    asn1_init(&k, val + 1, vlen - 1);
    if (asn1_enter(&k, ASN1_SEQUENCE, &r) != 0) return -1;
    if (asn1_next(&r, &tag, &val, &vlen) != 0 || tag != ASN1_INTEGER) return -1;
    while (vlen > 1 && val[0] == 0) { val++; vlen--; }
    *n = val;
    *n_len = vlen;
    if (asn1_next(&r, &tag, &val, &vlen) != 0 || tag != ASN1_INTEGER) return -1;
    {
        UINT64 ev = 0;
        if (asn1_uint(val, vlen, &ev) != 0) return -1;
        *e = (UINT32)ev;
    }
    return 0;
}

INT32 tls_x509_pubkey_ec(const x509_cert *c, UINT8 qx[32], UINT8 qy[32]) {
    asn1_reader spki, r;
    const UINT8 *q, *val;
    UINT32 l, vlen;
    UINT8 tag;

    if (c == (const x509_cert *)0 || c->spki == (const UINT8 *)0) return -1;
    asn1_init(&spki, c->spki, c->spki_len);
    if (asn1_enter(&spki, ASN1_SEQUENCE, &r) != 0) return -1;
    if (asn1_next(&r, &tag, &q, &l) != 0) return -1; /* algorithm */
    if (asn1_next(&r, &tag, &val, &vlen) != 0 || tag != ASN1_BIT_STRING) return -1;
    if (vlen < 65 || val[0] != 0 || val[1] != 0x04) return -1;
    for (l = 0; l < 32; l++) qx[l] = val[2 + l];
    for (l = 0; l < 32; l++) qy[l] = val[34 + l];
    return 0;
}

static void to32(UINT8 out[32], const UINT8 *src, UINT32 len) {
    UINT32 i;
    while (len > 32) { src++; len--; }
    for (i = 0; i < 32; i++) out[i] = 0;
    for (i = 0; i < len; i++) out[32 - len + i] = src[i];
}

INT32 tls_x509_verify_rsa(const x509_cert *c, const UINT8 *n, UINT32 n_len,
                          UINT32 e,
                          void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    UINT8 digest[32];
    if (sha256 == (void *)0) return -1;
    sha256(c->tbs, c->tbs_len, digest);
    return tls_rsa_pkcs1_sha256_verify(n, n_len, e, c->sig, c->sig_len, digest);
}

INT32 tls_x509_verify_ecdsa(const x509_cert *c, const UINT8 qx[32],
                            const UINT8 qy[32],
                            void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    asn1_reader s, seq;
    const UINT8 *rd, *sd;
    UINT32 rl, sl;
    UINT8 tag, r[32], ss[32], digest[32];

    if (sha256 == (void *)0) return -1;
    asn1_init(&s, c->sig, c->sig_len);
    if (asn1_enter(&s, ASN1_SEQUENCE, &seq) != 0) return -1;
    if (asn1_next(&seq, &tag, &rd, &rl) != 0 || tag != ASN1_INTEGER) return -1;
    if (asn1_next(&seq, &tag, &sd, &sl) != 0 || tag != ASN1_INTEGER) return -1;
    while (rl > 1 && rd[0] == 0) { rd++; rl--; }
    while (sl > 1 && sd[0] == 0) { sd++; sl--; }
    to32(r, rd, rl);
    to32(ss, sd, sl);
    sha256(c->tbs, c->tbs_len, digest);
    return tls_p256_ecdsa_verify(qx, qy, r, ss, digest);
}

/* Days since 1970-01-01 (Howard Hinnant's civil-from-days). */
static UINT64 days_from_civil(int y, int m, int d) {
    int era, yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (UINT64)(era * 146097 + doe - 719468);
}

static int two_digits(const UINT8 *s) {
    return (s[0] - '0') * 10 + (s[1] - '0');
}

static int parse_time(const UINT8 *s, UINT32 len, UINT64 *out) {
    int y, mo, d, h, mi, se;
    if (len == 13 && s[12] == 'Z') {              /* UTCTime YYMMDDHHMMSSZ */
        y = two_digits(s);
        y += (y >= 50) ? 1900 : 2000;
        mo = two_digits(s + 2);
        d = two_digits(s + 4);
        h = two_digits(s + 6);
        mi = two_digits(s + 8);
        se = two_digits(s + 10);
    } else if (len == 15 && s[14] == 'Z') {        /* GeneralizedTime */
        y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 +
            (s[3] - '0');
        mo = two_digits(s + 4);
        d = two_digits(s + 6);
        h = two_digits(s + 8);
        mi = two_digits(s + 10);
        se = two_digits(s + 12);
    } else {
        return -1;
    }
    *out = days_from_civil(y, mo, d) * 86400u + (UINT64)h * 3600u +
           (UINT64)mi * 60u + (UINT64)se;
    return 0;
}

int tls_x509_valid_at(const x509_cert *c, UINT64 now_unix) {
    UINT64 nb, na;
    if (c == (const x509_cert *)0) return -1;
    if (parse_time(c->not_before, c->nb_len, &nb) != 0) return -1;
    if (parse_time(c->not_after, c->na_len, &na) != 0) return -1;
    return (now_unix >= nb && now_unix <= na) ? 1 : 0;
}

static int ci_eq(const UINT8 *a, UINT32 n, const char *b, UINT32 m) {
    UINT32 i;
    if (n != m) return 0;
    for (i = 0; i < n; i++) {
        int ca = a[i], cb = (UINT8)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

/* Parse "a.b.c.d" into four wire-order bytes; -1 when it is not an IPv4
 * literal (a DNS name). */
static int parse_ipv4_bytes(const char *host, UINT8 out[4]) {
    UINT32 part = 0, dots = 0, i = 0;
    while (host[i] != 0) {
        char ch = host[i];
        if (ch >= '0' && ch <= '9') {
            part = part * 10u + (UINT32)(ch - '0');
            if (part > 255u) return -1;
        } else if (ch == '.') {
            if (dots >= 3u) return -1;
            out[dots++] = (UINT8)part;
            part = 0;
        } else {
            return -1;
        }
        i++;
        if (i > 15u) return -1;
    }
    if (dots != 3u) return -1;
    out[3] = (UINT8)part;
    return 0;
}

int tls_x509_hostname_match(const x509_cert *c, const char *host) {
    asn1_reader s, seq;
    const UINT8 *val;
    UINT32 len, hl = 0;
    UINT8 tag;

    if (c == (const x509_cert *)0 || c->san == (const UINT8 *)0 ||
        c->san_len == 0 || host == (const char *)0) {
        return 0;
    }
    while (host[hl]) hl++;
    asn1_init(&s, c->san, c->san_len);
    if (asn1_enter(&s, ASN1_SEQUENCE, &seq) != 0) return 0;
    while (asn1_peek(&seq, &tag) == 0) {
        if (asn1_next(&seq, &tag, &val, &len) != 0) break;
        if (tag == 0x87u && len == 4u) { /* iPAddress */
            UINT8 ip[4];
            if (parse_ipv4_bytes(host, ip) == 0 &&
                val[0] == ip[0] && val[1] == ip[1] &&
                val[2] == ip[2] && val[3] == ip[3]) {
                return 1;
            }
            continue;
        }
        if (tag != 0x82u) continue; /* dNSName */
        if (ci_eq(val, len, host, hl)) return 1;
        if (len >= 2 && val[0] == '*' && val[1] == '.') {
            UINT32 i = 0;
            while (i < hl && host[i] != '.') i++;
            if (i < hl && ci_eq(val + 1, len - 1, host + i, hl - i)) return 1;
        }
    }
    return 0;
}
