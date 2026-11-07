//! Registry of in-flight AI tasks keyed by `request_id`, supporting cooperative
//! cancellation.
//!
//! Phase 3: a task registers itself before starting work and `select!`s on
//! the returned `oneshot::Receiver<()>` alongside its work future.
//! `AiRuntimeService::CancelTask` looks up the `request_id` and fires the
//! sender; the task's cancel branch then resolves to a cancelled outcome.
//! Natural completion calls `complete` to drop the sender without signalling.

use std::collections::HashMap;
use std::sync::Mutex;
use tokio::sync::oneshot;

pub struct CancellationRegistry {
    senders: Mutex<HashMap<String, oneshot::Sender<()>>>,
}

impl Default for CancellationRegistry {
    fn default() -> Self {
        Self::new()
    }
}

impl CancellationRegistry {
    pub fn new() -> Self {
        Self {
            senders: Mutex::new(HashMap::new()),
        }
    }

    /// Register an in-flight task; returns the receiver the task selects on.
    /// If a sender already exists for `request_id`, it is replaced (the old
    /// task's receiver observes a closed channel, i.e. a natural drop).
    pub fn register(&self, request_id: &str) -> oneshot::Receiver<()> {
        let (tx, rx) = oneshot::channel();
        self.senders
            .lock()
            .expect("cancellation registry mutex poisoned")
            .insert(request_id.to_string(), tx);
        rx
    }

    /// Cancel an in-flight task. Returns `true` if a sender existed and was
    /// fired. The entry is removed so a second cancel returns `false`.
    pub fn cancel(&self, request_id: &str) -> bool {
        if let Some(tx) = self
            .senders
            .lock()
            .expect("cancellation registry mutex poisoned")
            .remove(request_id)
        {
            let _ = tx.send(());
            true
        } else {
            false
        }
    }

    /// Drop the sender on natural task completion (no cancel signal).
    pub fn complete(&self, request_id: &str) {
        self.senders
            .lock()
            .expect("cancellation registry mutex poisoned")
            .remove(request_id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::time::Duration;
    use tokio::time::timeout;

    #[tokio::test]
    async fn cancel_unknown_request_id_returns_false() {
        let registry = CancellationRegistry::new();
        assert!(!registry.cancel("nope"));
    }

    #[tokio::test]
    async fn cancel_fires_the_registered_receiver() {
        let registry = CancellationRegistry::new();
        let rx = registry.register("r");
        assert!(registry.cancel("r"));
        assert_eq!(rx.await, Ok(()));
    }

    #[tokio::test]
    async fn complete_drops_sender_without_firing_cancel() {
        let registry = CancellationRegistry::new();
        let rx = registry.register("r");
        registry.complete("r");
        // complete removed the sender, so a later cancel finds nothing.
        assert!(!registry.cancel("r"));
        // The receiver observes a closed channel (sender dropped), not a signal.
        assert!(rx.await.is_err(), "expected closed channel after complete");
    }

    // The delayed-op test (Phase 0 §4.6): a task that would otherwise run for a
    // long time completes only on cancel, well before its natural deadline.
    #[tokio::test]
    async fn cancelled_task_completes_only_on_cancel() {
        let registry = Arc::new(CancellationRegistry::new());
        let cancel_rx = registry.register("long-running");

        // Pretends to do 30s of work, but selects on the cancel receiver.
        let task = tokio::spawn(async move {
            tokio::select! {
                _ = tokio::time::sleep(Duration::from_secs(30)) => "completed-naturally",
                _ = cancel_rx => "cancelled",
            }
        });

        // Let the task park on the select, then cancel.
        tokio::time::sleep(Duration::from_millis(10)).await;
        assert!(registry.cancel("long-running"));

        // The task must resolve within 500ms (cancelled), not after 30s.
        let outcome = timeout(Duration::from_millis(500), task)
            .await
            .expect("task should resolve within 500ms after cancel, not 30s")
            .expect("task did not panic");
        assert_eq!(outcome, "cancelled");
    }
}
