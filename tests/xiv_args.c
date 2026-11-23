/* The game's launch argument (tools/xiv_args.py, build()): " T =<tick> /LANG =en-us /REGION =2
 * /SERVER_UTC =1356916742 /SESSION_ID =<id>" and its NUL, Blowfish (ECB, key the 8 hex digits of
 * tick & ~0xFFFF) over the whole 8-byte blocks with each 32-bit word's bytes swapped before and
 * after (the game's little-endian halves), the tail left plain; base64 with - and _, as
 * sqex0002<that>!////. Blowfish is CommonCrypto's on Apple systems, blowfish.c elsewhere. */
#include "xiv_args.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#else
#include "blowfish.h"
#endif
#include <stdio.h>
#include <string.h>

static void swap32(uint8_t* p, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4)
    {
        uint8_t a = p[i], b = p[i + 1];
        p[i] = p[i + 3], p[i + 1] = p[i + 2], p[i + 2] = b, p[i + 3] = a;
    }
}

int xiv_launch_arg(const char* session_id, uint32_t tick, char* out, size_t n)
{
    if (!session_id || strlen(session_id) != 56)
        return 0;
    uint8_t buf[256];
    int len = snprintf((char*)buf, sizeof buf, " T =%u /LANG =en-us /REGION =2 /SERVER_UTC =1356916742 /SESSION_ID =%s",
        tick, session_id);
    if (len < 0 || (size_t)len + 1 > sizeof buf)
        return 0;
    size_t total = (size_t)len + 1; /* the NUL is enciphered too */
    size_t full = total & ~(size_t)7;
    char key[9];
    snprintf(key, sizeof key, "%08x", tick & ~0xFFFFu);
    swap32(buf, full);
#if defined(__APPLE__)
    uint8_t enc[256];
    size_t moved = 0;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmBlowfish, kCCOptionECBMode, key, 8, NULL, buf, full, enc, sizeof enc, &moved) !=
            kCCSuccess || moved != full)
        return 0;
    memcpy(buf, enc, full);
#else
    Blowfish bf;
    blowfish_init(&bf, (const uint8_t*)key, 8);
    blowfish_ecb_encrypt(&bf, buf, full);
#endif
    swap32(buf, full);
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = (size_t)snprintf(out, n, "sqex0002");
    for (size_t i = 0; i < total; i += 3)
    {
        uint32_t v = (uint32_t)buf[i] << 16 | (i + 1 < total ? (uint32_t)buf[i + 1] << 8 : 0) | (i + 2 < total ? buf[i + 2] : 0);
        char q[4] = { B64[v >> 18 & 63], B64[v >> 12 & 63], i + 1 < total ? B64[v >> 6 & 63] : '=', i + 2 < total ? B64[v & 63] : '=' };
        if (o + 4 >= n)
            return 0;
        memcpy(out + o, q, 4), o += 4;
    }
    if (o + 6 > n)
        return 0;
    memcpy(out + o, "!////", 6);
    return 1;
}
