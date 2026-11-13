/* Blowfish for xiv_args.c where there is no CommonCrypto (Android): ECB encryption of whole 8-byte
 * blocks, big-endian halves, as CommonCrypto's kCCAlgorithmBlowfish with kCCOptionECBMode. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Blowfish
{
    uint32_t p[18], s[4][256];
} Blowfish;

void blowfish_init(Blowfish* b, const uint8_t* key, size_t n);
/* n a multiple of 8, in place */
void blowfish_ecb_encrypt(const Blowfish* b, uint8_t* data, size_t n);
