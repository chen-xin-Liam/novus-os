/**
 * Novus OS - bounded DER (ASN.1) reader for X.509.
 *
 * Linear, non-recursive and allocation-free. Only definite lengths up to
 * 2^32-1 are accepted (indefinite length is rejected). Every accessor is
 * bounds-checked against the enclosing reader, so a malformed certificate
 * cannot read past its buffer.
 */
#ifndef NOVUS_TLS_ASN1_H
#define NOVUS_TLS_ASN1_H

#include "../novu/novu_format.h"

typedef struct {
    const UINT8 *p;
    const UINT8 *end;
} asn1_reader;

/* DER tags used by X.509. */
#define ASN1_BOOLEAN        0x01u
#define ASN1_INTEGER        0x02u
#define ASN1_BIT_STRING     0x03u
#define ASN1_OCTET_STRING   0x04u
#define ASN1_NULL           0x05u
#define ASN1_OID            0x06u
#define ASN1_UTF8_STRING    0x0Cu
#define ASN1_PRINTABLE      0x13u
#define ASN1_UTCTIME        0x17u
#define ASN1_GENTIME        0x18u
#define ASN1_SEQUENCE       0x30u
#define ASN1_SET            0x31u

/* Take a top-level reader over `buf[0..len)`. */
void asn1_init(asn1_reader *r, const UINT8 *buf, UINT32 len);

/* Read the next TLV. On success writes the tag, value pointer and value
 * length, and advances the reader past it. Returns 0 or -1. */
int asn1_next(asn1_reader *r, UINT8 *tag, const UINT8 **val, UINT32 *len);

/* Read one TLV expecting `tag` and set `out` to its contents. */
int asn1_enter(asn1_reader *r, UINT8 tag, asn1_reader *out);

/* Peek the next tag without consuming it. Returns 0 or -1. */
int asn1_peek(const asn1_reader *r, UINT8 *tag);

/* Read the next TLV and also report the whole TLV span (header+content),
 * which X.509 needs to hash the exact tbsCertificate bytes. */
int asn1_next_full(asn1_reader *r, UINT8 *tag, const UINT8 **val, UINT32 *len,
                   const UINT8 **tlv, UINT32 *tlv_len);

/* Decode an unsigned INTEGER value (big-endian, <= 8 bytes kept). */
int asn1_uint(const UINT8 *val, UINT32 len, UINT64 *out);

#endif /* NOVUS_TLS_ASN1_H */
