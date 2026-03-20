#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace crypto {

// Fill buf with cryptographically random bytes
void random_bytes(uint8_t* buf, size_t len);

// X25519 Diffie-Hellman key exchange (Curve25519)
struct KeyPair { uint8_t pub[32]; uint8_t priv[32]; };
KeyPair keygen();
void    exchange(uint8_t out[32], const uint8_t my_priv[32], const uint8_t peer_pub[32]);

// SHA-256
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// Hex encoding helpers
std::string to_hex(const uint8_t* b, size_t n);
bool from_hex(const std::string& s, uint8_t* b, size_t n); // false on bad input

// Per-connection ChaCha20 stream state.
// One CipherStream is shared by both send and recv directions;
// send_ctr and recv_ctr are maintained independently.
struct CipherStream {
    bool     active   = false;
    uint8_t  key[32]  = {};
    uint64_t send_ctr = 0;
    uint64_t recv_ctr = 0;

    // Derive key from X25519 shared secret and activate.
    void init(const uint8_t shared_secret[32]);

    // Encrypt plaintext -> hex string
    std::string encrypt_line(const std::string& plain);

    // Decrypt hex string -> plaintext ("" on bad hex or inactive)
    std::string decrypt_line(const std::string& hex);
};

} // namespace crypto
