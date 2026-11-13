//! In-memory credential vault for the AI sidecar.
//!
//! Phase 3: the sidecar holds long-lived provider secrets (e.g. remote LLM API
//! keys) only in memory, behind opaque UUID handles. The C++ host registers a
//! secret via `RegisterCredential`, receives a handle, and passes only that
//! handle in `AiRequestHeader.credential_ref` on subsequent task calls. The
//! secret itself never enters the sidecar's command-line arguments, persistent
//! logs, or crash messages.
//!
//! Persistence of credentials is intentionally NOT implemented here — the C++
//! settings/UI layer decides later whether and how to store user credentials.
//! The vault evicts expired entries lazily on `resolve`, so an expired handle
//! cannot silently outlive its intended session.

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use zeroize::Zeroizing;

/// A string whose contents are never exposed through `Debug` or `Display`.
///
/// Any `{}` or `{:?}` formatting yields the literal `[REDACTED]`; the only way
/// to read the raw value is the explicit `expose()` call. This is the redaction
/// grep target: code that accidentally logs a `SecretString` prints
/// `[REDACTED]`, not the key. The inner buffer is zeroed on drop via
/// `Zeroizing<String>`.
#[derive(Clone)]
pub struct SecretString(Zeroizing<String>);

impl SecretString {
    pub fn new(inner: String) -> Self {
        Self(Zeroizing::new(inner))
    }

    /// Explicit, call-site-visible access to the raw secret.
    /// Never implement `Display`/`Debug` for this type.
    pub fn expose(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Debug for SecretString {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("[REDACTED]")
    }
}

impl std::fmt::Display for SecretString {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("[REDACTED]")
    }
}

/// Why a credential handle could not be resolved. The `Display` messages are
/// deliberately generic and contain neither the handle nor the secret.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum CredentialError {
    #[error("credential not found")]
    NotFound,
    #[error("credential expired")]
    Expired,
    #[error("credential revoked")]
    Revoked,
}

struct CredentialEntry {
    // Retained for future provider-scoped routing/redaction (Phase 5); not read
    // in Phase 3, hence the allow.
    #[allow(dead_code)]
    provider_id: String,
    secret: SecretString,
    expires_at: Option<Instant>,
    revoked: bool,
}

pub struct CredentialVault {
    default_ttl: Option<Duration>,
    entries: Mutex<HashMap<String, CredentialEntry>>,
}

impl CredentialVault {
    pub fn new(default_ttl: Option<Duration>) -> Self {
        Self {
            default_ttl,
            entries: Mutex::new(HashMap::new()),
        }
    }

    /// Register a secret and return an opaque UUID v4 handle.
    ///
    /// `ttl=None` falls back to the vault's `default_ttl`; if that is also
    /// `None` the credential never expires (use only when an operator has
    /// explicitly configured a no-expiry default).
    pub fn register(&self, provider_id: &str, secret: String, ttl: Option<Duration>) -> String {
        let handle = uuid::Uuid::new_v4().to_string();
        let expires_at = ttl.or(self.default_ttl).map(|d| Instant::now() + d);
        self.entries
            .lock()
            .expect("credential vault mutex poisoned")
            .insert(
                handle.clone(),
                CredentialEntry {
                    provider_id: provider_id.to_string(),
                    secret: SecretString::new(secret),
                    expires_at,
                    revoked: false,
                },
            );
        handle
    }

    /// Resolve a handle to its secret, lazy-evicting expired entries.
    pub fn resolve(&self, handle: &str) -> Result<SecretString, CredentialError> {
        let mut entries = self
            .entries
            .lock()
            .expect("credential vault mutex poisoned");
        let entry = entries.get(handle).ok_or(CredentialError::NotFound)?;
        if entry.revoked {
            return Err(CredentialError::Revoked);
        }
        if let Some(exp) = entry.expires_at {
            if Instant::now() >= exp {
                entries.remove(handle);
                return Err(CredentialError::Expired);
            }
        }
        Ok(entry.secret.clone())
    }

    /// Mark a handle revoked. The entry is retained so the handle stays dead;
    /// `resolve` thereafter returns `Revoked`.
    pub fn revoke(&self, handle: &str) {
        if let Some(entry) = self
            .entries
            .lock()
            .expect("credential vault mutex poisoned")
            .get_mut(handle)
        {
            entry.revoked = true;
        }
    }

    /// Replace any substring equal to a currently-held secret with
    /// `[REDACTED]`. Used before placing provider-derived error text into
    /// `AiResponseHeader.error_message` (Phase 0 §1.3: "redacted of secrets").
    /// Only secret material is redacted; provider ids are not secrets and may
    /// appear legitimately in error text.
    pub fn redact_error_message(&self, msg: &str) -> String {
        let entries = self
            .entries
            .lock()
            .expect("credential vault mutex poisoned");
        let mut out = msg.to_string();
        for entry in entries.values() {
            let raw = entry.secret.expose();
            if !raw.is_empty() && out.contains(raw) {
                out = out.replace(raw, "[REDACTED]");
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;

    const SHORT_TTL: Duration = Duration::from_millis(50);
    const TTL_MARGIN: Duration = Duration::from_millis(100);

    #[test]
    fn register_and_resolve_returns_secret() {
        let vault = CredentialVault::new(None);
        let handle = vault.register("remote", "sk-test-123".to_string(), None);
        assert!(!handle.is_empty());
        let resolved = vault.resolve(&handle).expect("registered handle resolves");
        assert_eq!(resolved.expose(), "sk-test-123");
    }

    #[test]
    fn resolve_unknown_handle_returns_not_found() {
        let vault = CredentialVault::new(None);
        let err = vault.resolve("does-not-exist").unwrap_err();
        assert_eq!(err, CredentialError::NotFound);
    }

    #[test]
    fn revoke_then_resolve_returns_revoked() {
        let vault = CredentialVault::new(None);
        let handle = vault.register("remote", "sk-test".to_string(), None);
        vault.revoke(&handle);
        let err = vault.resolve(&handle).unwrap_err();
        assert_eq!(err, CredentialError::Revoked);
    }

    #[test]
    fn expired_handle_returns_expired_then_not_found() {
        let vault = CredentialVault::new(None);
        let handle = vault.register("remote", "sk-test".to_string(), Some(SHORT_TTL));
        thread::sleep(SHORT_TTL + TTL_MARGIN);
        let err = vault.resolve(&handle).unwrap_err();
        assert_eq!(err, CredentialError::Expired);
        // Lazy eviction: the entry is gone, so a second resolve reports NotFound.
        let err2 = vault.resolve(&handle).unwrap_err();
        assert_eq!(err2, CredentialError::NotFound);
    }

    #[test]
    fn default_ttl_applies_when_request_omits_one() {
        let vault = CredentialVault::new(Some(SHORT_TTL));
        let handle = vault.register("remote", "sk-test".to_string(), None);
        thread::sleep(SHORT_TTL + TTL_MARGIN);
        assert_eq!(
            vault.resolve(&handle).unwrap_err(),
            CredentialError::Expired
        );
    }

    #[test]
    fn explicit_ttl_overrides_default() {
        // Long default, short explicit: the explicit one wins and the handle
        // expires quickly.
        let vault = CredentialVault::new(Some(Duration::from_secs(60)));
        let handle = vault.register("remote", "sk-test".to_string(), Some(SHORT_TTL));
        thread::sleep(SHORT_TTL + TTL_MARGIN);
        assert_eq!(
            vault.resolve(&handle).unwrap_err(),
            CredentialError::Expired
        );
    }

    #[test]
    fn no_ttl_and_no_default_means_no_expiry() {
        let vault = CredentialVault::new(None);
        let handle = vault.register("remote", "sk-test".to_string(), None);
        vault
            .resolve(&handle)
            .expect("no-expiry handle resolves immediately");
    }

    #[test]
    fn secret_string_redacts_debug_and_display() {
        let secret = SecretString::new("sk-real-value".to_string());
        let dbg = format!("{:?}", secret);
        let disp = format!("{}", secret);
        assert_eq!(dbg, "[REDACTED]");
        assert_eq!(disp, "[REDACTED]");
        assert!(!dbg.contains("sk-real-value"));
        assert!(!disp.contains("sk-real-value"));
    }

    #[test]
    fn redact_error_message_strips_known_secret() {
        let vault = CredentialVault::new(None);
        vault.register("remote", "sk-real".to_string(), None);
        let redacted = vault.redact_error_message("auth failed for sk-real key");
        assert_eq!(redacted, "auth failed for [REDACTED] key");
        assert!(!redacted.contains("sk-real"));
    }

    #[test]
    fn credential_error_messages_do_not_leak_handle_or_secret() {
        let vault = CredentialVault::new(None);
        let handle = vault.register("remote", "sk-super-secret".to_string(), None);
        for err in [
            CredentialError::NotFound,
            CredentialError::Expired,
            CredentialError::Revoked,
        ] {
            let msg = format!("{err}");
            assert!(
                !msg.contains("sk-super-secret"),
                "error leaked secret: {msg}"
            );
            assert!(!msg.contains(&handle), "error leaked handle: {msg}");
        }
    }

    #[test]
    fn redact_does_not_touch_provider_ids() {
        // provider_id is not a secret and may legitimately appear in error text.
        let vault = CredentialVault::new(None);
        vault.register("openai", "sk-real".to_string(), None);
        let redacted = vault.redact_error_message("provider openai returned 429");
        assert_eq!(redacted, "provider openai returned 429");
        assert!(redacted.contains("openai"));
        assert!(!redacted.contains("sk-real"));
    }
}
