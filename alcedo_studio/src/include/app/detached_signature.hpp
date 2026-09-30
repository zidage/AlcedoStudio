//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>

namespace alcedo {

enum class DetachedSignatureCheck {
  kValid,
  kInvalidPublicKey,
  kInvalidSignatureFormat,
  kSignatureMismatch,
};

/// Verify a detached base64 Ed25519 signature over the exact @p signed_bytes.
///
/// @p public_key is the raw 32-byte key. @p signature_text is the base64 text
/// written by `alcedo_update_signer sign` (surrounding whitespace is ignored).
/// Used by the software-update feed and the LUT package feed; each caller keeps
/// its own schema parsing and error wording. Pure function.
[[nodiscard]] auto VerifyDetachedSignature(const QByteArray& signed_bytes,
                                           const QByteArray& signature_text,
                                           const QByteArray& public_key) -> DetachedSignatureCheck;

}  // namespace alcedo
