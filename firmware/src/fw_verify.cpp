#include "fw_verify.h"
#include <Ed25519.h>
#include "../include/fw_public_key.h"

bool fwVerify(const uint8_t digest[32], const uint8_t sig[64]) {
  return Ed25519::verify(sig, FW_PUBLIC_KEY, digest, 32);
}
