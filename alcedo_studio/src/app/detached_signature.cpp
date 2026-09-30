//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/detached_signature.hpp"

extern "C" {
#include <ed25519.h>
}

namespace alcedo {

auto VerifyDetachedSignature(const QByteArray& signed_bytes, const QByteArray& signature_text,
                             const QByteArray& public_key) -> DetachedSignatureCheck {
  if (public_key.size() != 32) {
    return DetachedSignatureCheck::kInvalidPublicKey;
  }
  const QByteArray signature =
      QByteArray::fromBase64(signature_text.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
  if (signature.size() != 64) {
    return DetachedSignatureCheck::kInvalidSignatureFormat;
  }
  if (ed25519_verify(reinterpret_cast<const unsigned char*>(signature.constData()),
                     reinterpret_cast<const unsigned char*>(signed_bytes.constData()),
                     static_cast<size_t>(signed_bytes.size()),
                     reinterpret_cast<const unsigned char*>(public_key.constData())) != 1) {
    return DetachedSignatureCheck::kSignatureMismatch;
  }
  return DetachedSignatureCheck::kValid;
}

}  // namespace alcedo
