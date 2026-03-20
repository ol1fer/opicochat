// Cryptography module: X25519 key exchange, ChaCha20 stream cipher, SHA-256
// X25519 based on the TweetNaCl field-arithmetic approach (public domain, D.J.Bernstein et al.)

#include "crypto.hpp"
#include <cstring>
#include <cstdio>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
#else
  #include <unistd.h>
  #include <fcntl.h>
#endif

// ============================================================
// Random bytes
// ============================================================

void crypto::random_bytes(uint8_t* buf, size_t len) {
#ifdef _WIN32
    BCryptGenRandom(nullptr, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd >= 0) {
        size_t got = 0;
        while(got < len) {
            ssize_t n = read(fd, buf + got, len - got);
            if(n <= 0) break;
            got += (size_t)n;
        }
        close(fd);
    }
#endif
}

// ============================================================
// X25519 — field GF(2^255 - 19), 16-limb representation
// Each limb holds ~16 bits; actual value = sum(fe[i] * 2^(16*i)) mod p
// ============================================================

typedef int64_t gf[16];

static void gf_add(gf o, const gf a, const gf b) {
    for(int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
static void gf_sub(gf o, const gf a, const gf b) {
    for(int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
static void gf_copy(gf o, const gf a) {
    for(int i = 0; i < 16; ++i) o[i] = a[i];
}
static void gf_cswap(gf p, gf q, int b) {
    int64_t t, c = -(int64_t)b;
    for(int i = 0; i < 16; ++i) { t = c & (p[i]^q[i]); p[i] ^= t; q[i] ^= t; }
}

// Carry reduction: normalize each limb to [0, 65535], propagate overflow.
// The last limb wraps with factor 38 (since 2^256 ≡ 38 mod p).
static void gf_car(gf o) {
    int64_t c;
    for(int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i+1) & 15] += c - 1 + 37*(c-1)*(i==15);
        o[i] -= c << 16;
    }
}

// Field multiplication with reduction
static void gf_mul(gf o, const gf a, const gf b) {
    int64_t t[31] = {};
    for(int i = 0; i < 16; ++i)
        for(int j = 0; j < 16; ++j)
            t[i+j] += a[i]*b[j];
    for(int i = 0; i < 15; ++i) t[i] += 38*t[i+16];
    for(int i = 0; i < 16; ++i) o[i] = t[i];
    gf_car(o); gf_car(o);
}

static void gf_sqr(gf o, const gf a) { gf_mul(o, a, a); }

// Fermat inversion: a^(p-2) mod p  (p-2 = 2^255-21)
// Bits of p-2 are all 1 except positions 1 and 3.
static void gf_inv(gf o, const gf a) {
    gf c; gf_copy(c, a);
    for(int i = 253; i >= 0; --i) {
        gf_sqr(c, c);
        if(i != 2 && i != 4) gf_mul(c, c, a);
    }
    gf_copy(o, c);
}

// Unpack 32 little-endian bytes into field element
static void gf_unpack(gf o, const uint8_t m[32]) {
    for(int i = 0; i < 16; ++i)
        o[i] = (int64_t)m[2*i] | ((int64_t)m[2*i+1] << 8);
    o[15] &= 0x7fff; // clear bit 255
}

// Pack field element to 32 bytes (fully reduced)
static void gf_pack(uint8_t o[32], gf n) {
    gf m, t;
    gf_copy(t, n);
    gf_car(t); gf_car(t); gf_car(t);
    for(int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for(int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        gf_cswap(t, m, (int)(1-b));
    }
    for(int i = 0; i < 16; ++i) {
        o[2*i]   = (uint8_t)t[i];
        o[2*i+1] = (uint8_t)(t[i] >> 8);
    }
}

// a24 = 121665 = (486662-2)/4  for Curve25519
static const gf _121665 = {0xDB41, 1};

// X25519 Montgomery ladder (RFC 7748 §5)
static void x25519_scalarmult(uint8_t q[32], const uint8_t k[32], const uint8_t u[32]) {
    // Clamp the scalar
    uint8_t z[32];
    for(int i = 0; i < 32; ++i) z[i] = k[i];
    z[0]  &= 248;
    z[31] &= 127;
    z[31] |= 64;

    gf x1, x2, z2, x3, z3, tmp0, tmp1, A, AA, B, BB, E, C, D, DA, CB;
    gf_unpack(x1, u);
    // x2=1, z2=0, x3=u, z3=1
    memset(x2, 0, sizeof(x2)); x2[0] = 1;
    memset(z2, 0, sizeof(z2));
    gf_copy(x3, x1);
    memset(z3, 0, sizeof(z3)); z3[0] = 1;

    int swap = 0;
    for(int t = 254; t >= 0; --t) {
        int k_t = (z[(unsigned)t >> 3] >> ((unsigned)t & 7)) & 1;
        swap ^= k_t;
        gf_cswap(x2, x3, swap);
        gf_cswap(z2, z3, swap);
        swap = k_t;

        gf_add(A, x2, z2);
        gf_sub(B, x2, z2);
        gf_sqr(AA, A);
        gf_sqr(BB, B);
        gf_sub(E, AA, BB);
        gf_add(C, x3, z3);
        gf_sub(D, x3, z3);
        gf_mul(DA, D, A);
        gf_mul(CB, C, B);

        gf_add(tmp0, DA, CB); gf_sqr(x3, tmp0);
        gf_sub(tmp1, DA, CB); gf_sqr(tmp0, tmp1); gf_mul(z3, x1, tmp0);

        gf_mul(x2, AA, BB);
        gf_mul(tmp0, _121665, E); gf_add(tmp1, AA, tmp0); gf_mul(z2, E, tmp1);
    }
    gf_cswap(x2, x3, swap);
    gf_cswap(z2, z3, swap);

    gf_inv(z2, z2);
    gf_mul(x2, x2, z2);
    gf_pack(q, x2);
}

crypto::KeyPair crypto::keygen() {
    KeyPair kp;
    random_bytes(kp.priv, 32);
    kp.priv[0]  &= 248;
    kp.priv[31] &= 127;
    kp.priv[31] |= 64;
    uint8_t base[32] = {9}; // Curve25519 base point u=9
    x25519_scalarmult(kp.pub, kp.priv, base);
    return kp;
}

void crypto::exchange(uint8_t out[32], const uint8_t my_priv[32], const uint8_t peer_pub[32]) {
    x25519_scalarmult(out, my_priv, peer_pub);
}

// ============================================================
// SHA-256 (FIPS 180-4)
// ============================================================

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32-n)); }

static void sha256_block(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for(int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[4*i]<<24)|((uint32_t)block[4*i+1]<<16)|((uint32_t)block[4*i+2]<<8)|block[4*i+3];
    for(int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
        uint32_t ch = (e&f)^(~e&g);
        uint32_t t1 = hh+S1+ch+SHA256_K[i]+w[i];
        uint32_t S0 = rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
        uint32_t maj = (a&b)^(a&c)^(b&c);
        uint32_t t2 = S0+maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void crypto::sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint8_t block[64];
    size_t pos = 0;
    // process full blocks
    while(pos + 64 <= len) { sha256_block(h, data+pos); pos += 64; }
    // final block(s) with padding
    size_t tail = len - pos;
    memcpy(block, data+pos, tail);
    block[tail++] = 0x80;
    if(tail > 56) {
        memset(block+tail, 0, 64-tail);
        sha256_block(h, block);
        tail = 0;
    }
    memset(block+tail, 0, 56-tail);
    uint64_t bits = (uint64_t)len * 8;
    for(int i = 0; i < 8; ++i) block[56+i] = (uint8_t)(bits >> (56-8*i));
    sha256_block(h, block);
    for(int i = 0; i < 8; ++i) {
        out[4*i]   = (uint8_t)(h[i]>>24);
        out[4*i+1] = (uint8_t)(h[i]>>16);
        out[4*i+2] = (uint8_t)(h[i]>>8);
        out[4*i+3] = (uint8_t)h[i];
    }
}

// ============================================================
// Hex encoding
// ============================================================

std::string crypto::to_hex(const uint8_t* b, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string s(n*2, 0);
    for(size_t i = 0; i < n; ++i) {
        s[2*i]   = hex[b[i] >> 4];
        s[2*i+1] = hex[b[i] & 0xf];
    }
    return s;
}

bool crypto::from_hex(const std::string& s, uint8_t* b, size_t n) {
    if(s.size() != 2*n) return false;
    for(size_t i = 0; i < n; ++i) {
        auto unhex = [](char c) -> int {
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return 10+c-'a';
            if(c>='A'&&c<='F') return 10+c-'A';
            return -1;
        };
        int hi = unhex(s[2*i]), lo = unhex(s[2*i+1]);
        if(hi < 0 || lo < 0) return false;
        b[i] = (uint8_t)((hi<<4)|lo);
    }
    return true;
}

// ============================================================
// ChaCha20 stream cipher (RFC 8439)
// ============================================================

static inline uint32_t u8to32le(const uint8_t* p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline void u32to8le(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

#define QROUND(a,b,c,d) \
    a+=b; d^=a; d=(d<<16)|(d>>16); \
    c+=d; b^=c; b=(b<<12)|(b>>20); \
    a+=b; d^=a; d=(d<< 8)|(d>>24); \
    c+=d; b^=c; b=(b<< 7)|(b>>25);

static void chacha20_block(uint8_t out[64], const uint8_t key[32],
                           const uint8_t nonce[12], uint32_t counter) {
    uint32_t s[16] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
        u8to32le(key+0),  u8to32le(key+4),  u8to32le(key+8),  u8to32le(key+12),
        u8to32le(key+16), u8to32le(key+20), u8to32le(key+24), u8to32le(key+28),
        counter,
        u8to32le(nonce+0), u8to32le(nonce+4), u8to32le(nonce+8)
    };
    uint32_t w[16];
    for(int i = 0; i < 16; ++i) w[i] = s[i];
    for(int i = 0; i < 10; ++i) {
        QROUND(w[0],w[4],w[8], w[12])
        QROUND(w[1],w[5],w[9], w[13])
        QROUND(w[2],w[6],w[10],w[14])
        QROUND(w[3],w[7],w[11],w[15])
        QROUND(w[0],w[5],w[10],w[15])
        QROUND(w[1],w[6],w[11],w[12])
        QROUND(w[2],w[7],w[8], w[13])
        QROUND(w[3],w[4],w[9], w[14])
    }
    for(int i = 0; i < 16; ++i) u32to8le(out+4*i, w[i]+s[i]);
}
#undef QROUND

// Generate len bytes of keystream into buf.
// nonce_ctr is used as the 8-byte nonce (occupying nonce bytes 0..7, bytes 8..11 = 0).
static void chacha20_stream(uint8_t* buf, size_t len,
                             const uint8_t key[32], uint64_t nonce_ctr) {
    uint8_t nonce[12] = {};
    nonce[0]=(uint8_t)nonce_ctr;       nonce[1]=(uint8_t)(nonce_ctr>>8);
    nonce[2]=(uint8_t)(nonce_ctr>>16); nonce[3]=(uint8_t)(nonce_ctr>>24);
    nonce[4]=(uint8_t)(nonce_ctr>>32); nonce[5]=(uint8_t)(nonce_ctr>>40);
    nonce[6]=(uint8_t)(nonce_ctr>>48); nonce[7]=(uint8_t)(nonce_ctr>>56);
    uint32_t block_ctr = 0;
    uint8_t  block[64];
    size_t   pos = 0;
    while(pos < len) {
        chacha20_block(block, key, nonce, block_ctr++);
        size_t take = (len - pos < 64) ? (len - pos) : 64;
        for(size_t i = 0; i < take; ++i) buf[pos+i] ^= block[i];
        pos += take;
    }
}

// ============================================================
// CipherStream
// ============================================================

void crypto::CipherStream::init(const uint8_t shared[32]) {
    // Derive key = SHA256(shared_secret || "opicochat")
    uint8_t input[32+9];
    memcpy(input, shared, 32);
    memcpy(input+32, "opicochat", 9);
    sha256(input, 41, key);
    send_ctr = 0;
    recv_ctr = 0;
    active   = true;
}

std::string crypto::CipherStream::encrypt_line(const std::string& plain) {
    if(!active) return plain;
    size_t n = plain.size();
    std::vector<uint8_t> buf(n, 0);
    for(size_t i = 0; i < n; ++i) buf[i] = (uint8_t)plain[i];
    chacha20_stream(buf.data(), n, key, send_ctr++);
    return to_hex(buf.data(), n);
}

std::string crypto::CipherStream::decrypt_line(const std::string& hex) {
    if(!active) return hex;
    if(hex.size() % 2 != 0) return "";
    size_t n = hex.size() / 2;
    std::vector<uint8_t> buf(n, 0);
    if(!from_hex(hex, buf.data(), n)) return "";
    chacha20_stream(buf.data(), n, key, recv_ctr++);
    return std::string(buf.begin(), buf.end());
}
