#include "nd_heuristic.h"

static int nd_contains(const uint8_t *buf, uint32_t len, const char *needle) {
    uint32_t i, j, n = 0;
    while (needle[n] != 0) n++;
    if (n == 0u || n > len) return 0;
    for (i = 0; i + n <= len; i++) {
        for (j = 0; j < n; j++) {
            if (buf[i + j] != (uint8_t)needle[j]) break;
        }
        if (j == n) return 1;
    }
    return 0;
}

int32_t nd_heuristic_score(const uint8_t *buf, uint32_t len, uint32_t *score) {
    static const char *suspicious[] = {
        "VirtualAlloc", "CreateRemoteThread", "WriteProcessMemory",
        "cmd.exe", "powershell", "shellcode", "keylog", "rundll32",
        "HKEY_LOCAL_MACHINE", "AdjustTokenPrivileges"
    };
    uint32_t seen[256];
    uint32_t i, k, printable = 0, distinct = 0, score_v = 0;
    if (score == NULL) return ND_ERR_INVAL;
    *score = 0;
    if (buf == NULL || len == 0u) return ND_OK;
    for (i = 0; i < 256u; i++) seen[i] = 0u;
    for (i = 0; i < len; i++) {
        if (seen[buf[i]] == 0u) distinct++;
        seen[buf[i]] = 1u;
        if (buf[i] >= 0x20u && buf[i] < 0x7Fu) printable++;
    }
    /* Integer approximation of byte spread / entropy, scaled to 0..40. */
    score_v += distinct * 40u / 256u;
    if (printable * 2u > len) score_v += 10u;
    for (k = 0; k < sizeof(suspicious) / sizeof(suspicious[0]); k++) {
        if (nd_contains(buf, len, suspicious[k])) {
            score_v += 8u;
            if (score_v > 100u) break;
        }
    }
    if (len >= 2u && buf[0] == 0x4Du && buf[1] == 0x5Au) score_v += 10u;
    if (len >= 4u && buf[0] == 0x7Fu && buf[1] == 0x45u && buf[2] == 0x4Cu && buf[3] == 0x46u) score_v += 10u;
    if (len >= 2u && buf[0] == 0x23u && buf[1] == 0x21u) score_v += 5u;
    *score = score_v > 100u ? 100u : score_v;
    return ND_OK;
}
