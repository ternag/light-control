#pragma once
#include <stdint.h>

// Ed25519 verify of a detached 64-byte signature over a 32-byte SHA-256 digest,
// against the compiled-in FW_PUBLIC_KEY. Returns true iff the signature is valid.
bool fwVerify(const uint8_t digest[32], const uint8_t sig[64]);
