#pragma once
// WPA2-PSK key derivation helpers (SHA-1, HMAC-SHA-1, PBKDF2, 802.11i PRF). No kernel dependencies.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace rtlcrypto {

static inline void secure_zero(void *p, size_t n) {
    volatile uint8_t *vp = static_cast<volatile uint8_t *>(p);
    while (n--) *vp++ = 0;
}

static inline bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

struct Sha1 { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; };

static inline uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static inline void sha1_block(Sha1 &c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d; c.h[4] += e;
}

static inline void sha1_init(Sha1 &c) {
    c.h[0] = 0x67452301; c.h[1] = 0xEFCDAB89; c.h[2] = 0x98BADCFE; c.h[3] = 0x10325476; c.h[4] = 0xC3D2E1F0;
    c.len = 0; c.n = 0;
}

static inline void sha1_update(Sha1 &c, const uint8_t *d, size_t len) {
    c.len += len;
    while (len) {
        size_t take = 64 - c.n;
        if (take > len) take = len;
        memcpy(c.buf + c.n, d, take);
        c.n += (uint32_t)take; d += take; len -= take;
        if (c.n == 64) { sha1_block(c, c.buf); c.n = 0; }
    }
}

static inline void sha1_final(Sha1 &c, uint8_t out[20]) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t z = 0;
    while (c.n != 56) sha1_update(c, &z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) {
        out[4*i] = (uint8_t)(c.h[i] >> 24); out[4*i+1] = (uint8_t)(c.h[i] >> 16);
        out[4*i+2] = (uint8_t)(c.h[i] >> 8); out[4*i+3] = (uint8_t)c.h[i];
    }
}

// HMAC-SHA-1 with the inner and outer pad states precomputed (used by PBKDF2, which calls it thousands of times)
struct Hmac { Sha1 inner, outer; };

static inline void hmac_init(Hmac &h, const uint8_t *key, size_t klen) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (klen > 64) { Sha1 s; sha1_init(s); sha1_update(s, key, klen); sha1_final(s, k); }
    else memcpy(k, key, klen);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    sha1_init(h.inner); sha1_update(h.inner, ip, 64);
    sha1_init(h.outer); sha1_update(h.outer, op, 64);
    secure_zero(k, sizeof(k)); secure_zero(ip, sizeof(ip)); secure_zero(op, sizeof(op));
}

static inline void hmac_mac(const Hmac &h, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[20]) {
    Sha1 s = h.inner;
    sha1_update(s, a, alen);
    if (blen) sha1_update(s, b, blen);
    uint8_t ih[20];
    sha1_final(s, ih);
    Sha1 o = h.outer;
    sha1_update(o, ih, 20);
    sha1_final(o, out);
}

static inline void hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *data, size_t dlen, uint8_t out[20]) {
    Hmac h; hmac_init(h, key, klen);
    hmac_mac(h, data, dlen, nullptr, 0, out);
}

// PBKDF2-HMAC-SHA1(passphrase, ssid, 4096 iterations) -> 32-byte PMK
static inline void pbkdf2_pmk(const char *pass, const uint8_t *ssid, size_t ssidLen, uint8_t pmk[32]) {
    Hmac h; hmac_init(h, (const uint8_t *)pass, strlen(pass));
    uint8_t out[40];
    for (uint32_t block = 1; block <= 2; block++) {
        uint8_t ctr[4] = {0, 0, 0, (uint8_t)block};
        uint8_t u[20], t[20];
        hmac_mac(h, ssid, ssidLen, ctr, 4, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            hmac_mac(h, u, 20, nullptr, 0, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        memcpy(out + (block - 1) * 20, t, 20);
    }
    memcpy(pmk, out, 32);
    secure_zero(out, sizeof(out)); secure_zero(&h, sizeof(h));
}

// IEEE 802.11i PRF: output = HMAC-SHA1(key, label || 0x00 || data || counter) blocks, truncated to outLen
static inline void prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen,
                       uint8_t *out, size_t outLen) {
    Hmac h; hmac_init(h, key, klen);
    uint8_t in[160];
    size_t ll = strlen(label);
    if (ll + 1 + dlen + 1 > sizeof(in)) {
        secure_zero(&h, sizeof(h));
        return;
    }
    memcpy(in, label, ll); in[ll] = 0;
    memcpy(in + ll + 1, data, dlen);
    size_t n = ll + 1 + dlen;
    uint8_t digest[20];
    for (size_t pos = 0, i = 0; pos < outLen; pos += 20, i++) {
        in[n] = (uint8_t)i;
        hmac_mac(h, in, n + 1, nullptr, 0, digest);
        size_t take = outLen - pos < 20 ? outLen - pos : 20;
        memcpy(out + pos, digest, take);
    }
    secure_zero(in, sizeof(in));
    secure_zero(digest, sizeof(digest));
    secure_zero(&h, sizeof(h));
}

static inline int cmp_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff;
}

// PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(ANonce,SNonce))
static inline void derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                              const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]) {
    uint8_t d[76];
    const uint8_t *a1 = cmp_bytes(aa, spa, 6) < 0 ? aa : spa;
    const uint8_t *a2 = cmp_bytes(aa, spa, 6) < 0 ? spa : aa;
    const uint8_t *n1 = cmp_bytes(anonce, snonce, 32) < 0 ? anonce : snonce;
    const uint8_t *n2 = cmp_bytes(anonce, snonce, 32) < 0 ? snonce : anonce;
    memcpy(d, a1, 6); memcpy(d + 6, a2, 6); memcpy(d + 12, n1, 32); memcpy(d + 44, n2, 32);
    prf(pmk, 32, "Pairwise key expansion", d, sizeof(d), ptk, 48);
}

// EAPOL-Key MIC for key descriptor version 2: HMAC-SHA1(KCK, frame with MIC field zero), first 16 bytes
static inline void eapol_mic(const uint8_t kck[16], const uint8_t *frame, size_t len, uint8_t mic[16]) {
    uint8_t d[20];
    hmac_sha1(kck, 16, frame, len, d);
    memcpy(mic, d, 16);
}

// ---- AES-128 (decrypt only, S-box computed at run time) and RFC 3394 key unwrap, for the GTK in EAPOL message 3 ----
struct Aes { uint8_t sbox[256]; uint8_t isbox[256]; uint8_t rk[176]; };

static inline uint8_t aes_rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }
static inline uint8_t aes_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static inline uint8_t aes_gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) { if (b & 1) p ^= a; a = aes_xtime(a); b >>= 1; }
    return p;
}

static inline void aes_init(Aes &a, const uint8_t key[16]) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));              // p *= 3
        q = (uint8_t)(q ^ (uint8_t)(q << 1));
        q = (uint8_t)(q ^ (uint8_t)(q << 2));
        q = (uint8_t)(q ^ (uint8_t)(q << 4));
        if (q & 0x80) q ^= 0x09;                                                        // q /= 3
        uint8_t x = (uint8_t)(q ^ aes_rotl8(q, 1) ^ aes_rotl8(q, 2) ^ aes_rotl8(q, 3) ^ aes_rotl8(q, 4));
        a.sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    a.sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) a.isbox[a.sbox[i]] = (uint8_t)i;
    memcpy(a.rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a.rk + i - 4, 4);
        if ((i % 16) == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(a.sbox[t[1]] ^ rcon); t[1] = a.sbox[t[2]]; t[2] = a.sbox[t[3]]; t[3] = a.sbox[u];
            rcon = aes_xtime(rcon);
        }
        for (int j = 0; j < 4; j++) a.rk[i + j] = (uint8_t)(a.rk[i - 16 + j] ^ t[j]);
    }
}

static inline void aes_decrypt_block(const Aes &a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ a.rk[160 + i]);
    for (int round = 9; round >= 0; round--) {
        for (int c = 0; c < 4; c++)                                  // inverse ShiftRows
            for (int r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
        for (int i = 0; i < 16; i++) s[i] = a.isbox[t[i]];             // inverse SubBytes
        for (int i = 0; i < 16; i++) s[i] ^= a.rk[16 * round + i];     // AddRoundKey
        if (round > 0) {                                             // inverse MixColumns
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[4*c], a1 = s[4*c+1], a2 = s[4*c+2], a3 = s[4*c+3];
                s[4*c]   = (uint8_t)(aes_gmul(a0,14) ^ aes_gmul(a1,11) ^ aes_gmul(a2,13) ^ aes_gmul(a3,9));
                s[4*c+1] = (uint8_t)(aes_gmul(a0,9)  ^ aes_gmul(a1,14) ^ aes_gmul(a2,11) ^ aes_gmul(a3,13));
                s[4*c+2] = (uint8_t)(aes_gmul(a0,13) ^ aes_gmul(a1,9)  ^ aes_gmul(a2,14) ^ aes_gmul(a3,11));
                s[4*c+3] = (uint8_t)(aes_gmul(a0,11) ^ aes_gmul(a1,13) ^ aes_gmul(a2,9)  ^ aes_gmul(a3,14));
            }
        }
    }
    memcpy(out, s, 16);
}

// RFC 3394 AES key unwrap with a 128-bit KEK. inLen is a multiple of 8, at least 24 and at most 208.
// Writes inLen - 8 bytes to out; returns true when the integrity value 0xA6A6A6A6A6A6A6A6 checks out.
static inline bool aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t inLen, uint8_t *out) {
    if (inLen < 24 || inLen > 208 || (inLen % 8)) return false;
    Aes a; aes_init(a, kek);
    size_t n = inLen / 8 - 1;
    uint8_t A[8], R[200], B[16];
    memcpy(A, in, 8);
    memcpy(R, in + 8, n * 8);
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            uint32_t t = (uint32_t)(n * (size_t)j + i);
            uint8_t ax[8];
            memcpy(ax, A, 8);
            ax[7] ^= (uint8_t)t; ax[6] ^= (uint8_t)(t >> 8); ax[5] ^= (uint8_t)(t >> 16); ax[4] ^= (uint8_t)(t >> 24);
            memcpy(B, ax, 8);
            memcpy(B + 8, R + (i - 1) * 8, 8);
            uint8_t o[16];
            aes_decrypt_block(a, B, o);
            memcpy(A, o, 8);
            memcpy(R + (i - 1) * 8, o + 8, 8);
        }
    }
    memcpy(out, R, n * 8);
    static const uint8_t kIv[8] = {0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6};
    bool ok = ct_equal(A, kIv, 8);
    secure_zero(&a, sizeof(a)); secure_zero(A, sizeof(A)); secure_zero(R, sizeof(R)); secure_zero(B, sizeof(B));
    return ok;
}

} // namespace rtlcrypto
