/**
 * Novus OS - AES-GCM AEAD (TLS 1.3/1.2 record protection).
 *
 * Only 96-bit IVs are accepted (what TLS uses). Returns 0 on success, -1 on
 * bad arguments or (for decryption) an authentication failure.
 */
#ifndef NOVUS_TLS_GCM_H
#define NOVUS_TLS_GCM_H

#include "aes.h"

INT32 gcm_encrypt(const UINT8 *key, UINT32 key_len, const UINT8 iv[12],
                  const UINT8 *aad, UINT32 aad_len,
                  const UINT8 *pt, UINT32 pt_len,
                  UINT8 *ct, UINT8 tag[16]);

INT32 gcm_decrypt(const UINT8 *key, UINT32 key_len, const UINT8 iv[12],
                  const UINT8 *aad, UINT32 aad_len,
                  const UINT8 *ct, UINT32 ct_len,
                  const UINT8 tag[16], UINT8 *pt);

#endif /* NOVUS_TLS_GCM_H */
