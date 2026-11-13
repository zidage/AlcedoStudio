//! Lazy load and idle unload of one encoder session.
//!
//! An [`EncoderResidency`] owns the slot of one loaded encoder (an ONNX Runtime session with
//! its input and output names, or a Core ML model). The first request that needs the encoder
//! runs the loader; later requests reuse the loaded value. [`EncoderResidency::unload_if_idle`]
//! drops the value when its last request finished more than the idle time ago. One mutex
//! guards the slot for the whole request, so a request and an unload never overlap, and
//! concurrent requests wait for one load.

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::Result;
use tracing::info;

/// When a loaded encoder is dropped after its last request.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IdleUnload {
    /// The encoder stays loaded for the process lifetime.
    Never,
    /// The encoder is dropped at the end of each request.
    AfterEachRequest,
    /// The encoder is dropped when it has been idle for longer than the duration.
    After(Duration),
}

impl IdleUnload {
    /// The rule for the process argument value @p seconds: -1 never, 0 after each request,
    /// a positive value after that many seconds. Other negative values are rejected.
    pub fn from_seconds(seconds: i64) -> Result<Self> {
        match seconds {
            -1 => Ok(Self::Never),
            0 => Ok(Self::AfterEachRequest),
            value if value > 0 => Ok(Self::After(Duration::from_secs(value as u64))),
            value => anyhow::bail!("idle unload time must be -1, 0, or positive, got {value}"),
        }
    }
}

/// Source of the current time, so tests can advance it.
pub trait Clock: Send + Sync {
    fn now(&self) -> Instant;
}

/// The monotonic system clock.
pub struct SystemClock;

impl Clock for SystemClock {
    fn now(&self) -> Instant {
        Instant::now()
    }
}

/// Residency of one encoder for status reports.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EncoderStatus {
    /// `text`, `image`, or `multimodal`.
    pub encoder: String,
    pub loaded: bool,
    /// The device the encoder loads on, as passed to the loader.
    pub device: String,
    /// Time since the last request finished; 0 before the first request.
    pub idle_ms: u64,
}

type Loader<T> = Box<dyn Fn() -> Result<T> + Send + Sync>;

struct Slot<T> {
    value: Option<T>,
    last_use: Option<Instant>,
}

/// One lazily loaded encoder. See the module documentation.
pub struct EncoderResidency<T> {
    name: &'static str,
    device: String,
    idle_unload: IdleUnload,
    loader: Loader<T>,
    clock: Arc<dyn Clock>,
    slot: Mutex<Slot<T>>,
}

impl<T> EncoderResidency<T> {
    pub fn new(
        name: &'static str,
        device: impl Into<String>,
        idle_unload: IdleUnload,
        loader: impl Fn() -> Result<T> + Send + Sync + 'static,
    ) -> Self {
        Self::with_clock(name, device, idle_unload, loader, Arc::new(SystemClock))
    }

    pub fn with_clock(
        name: &'static str,
        device: impl Into<String>,
        idle_unload: IdleUnload,
        loader: impl Fn() -> Result<T> + Send + Sync + 'static,
        clock: Arc<dyn Clock>,
    ) -> Self {
        Self {
            name,
            device: device.into(),
            idle_unload,
            loader: Box::new(loader),
            clock,
            slot: Mutex::new(Slot {
                value: None,
                last_use: None,
            }),
        }
    }

    /// Runs @p f on the loaded encoder, loading it first when the slot is empty.
    ///
    /// A load failure returns the loader error and leaves the slot empty, so the next request
    /// runs the loader again. The end of the request counts as the last use.
    pub fn with_session<R>(&self, f: impl FnOnce(&mut T) -> Result<R>) -> Result<R> {
        let mut slot = self
            .slot
            .lock()
            .map_err(|err| anyhow::anyhow!("{} encoder lock poisoned: {err}", self.name))?;
        if slot.value.is_none() {
            let started = Instant::now();
            let value = (self.loader)()?;
            info!(
                "loaded {} encoder on {} in {} ms",
                self.name,
                self.device,
                started.elapsed().as_millis()
            );
            slot.value = Some(value);
        }
        let result = match slot.value.as_mut() {
            Some(value) => f(value),
            None => unreachable!("the slot was filled above"),
        };
        slot.last_use = Some(self.clock.now());
        if self.idle_unload == IdleUnload::AfterEachRequest {
            slot.value = None;
            info!("unloaded {} encoder after the request", self.name);
        }
        result
    }

    /// Drops the encoder when it is loaded and idle for longer than its idle time. Returns
    /// true when it was dropped. Waits for a running request, which then counts as the last
    /// use.
    pub fn unload_if_idle(&self) -> bool {
        let idle_time = match self.idle_unload {
            IdleUnload::After(idle_time) => idle_time,
            IdleUnload::Never | IdleUnload::AfterEachRequest => return false,
        };
        let Ok(mut slot) = self.slot.lock() else {
            return false;
        };
        if slot.value.is_none() {
            return false;
        }
        let idle = slot
            .last_use
            .map(|last_use| self.clock.now().saturating_duration_since(last_use))
            .unwrap_or_default();
        if idle <= idle_time {
            return false;
        }
        slot.value = None;
        info!(
            "unloaded {} encoder after {} s idle",
            self.name,
            idle.as_secs()
        );
        true
    }

    /// Whether the encoder is loaded, its device, and the time since its last use.
    pub fn status(&self) -> EncoderStatus {
        let (loaded, idle_ms) = match self.slot.lock() {
            Ok(slot) => (
                slot.value.is_some(),
                slot.last_use
                    .map(|last_use| {
                        self.clock
                            .now()
                            .saturating_duration_since(last_use)
                            .as_millis() as u64
                    })
                    .unwrap_or(0),
            ),
            Err(_) => (false, 0),
        };
        EncoderStatus {
            encoder: self.name.to_string(),
            loaded,
            device: self.device.clone(),
            idle_ms,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
    use std::sync::mpsc;
    use std::thread;

    /// A clock that moves only when the test advances it.
    struct ManualClock {
        start: Instant,
        offset_ms: AtomicU64,
    }

    impl ManualClock {
        fn new() -> Arc<Self> {
            Arc::new(Self {
                start: Instant::now(),
                offset_ms: AtomicU64::new(0),
            })
        }

        fn advance(&self, duration: Duration) {
            self.offset_ms
                .fetch_add(duration.as_millis() as u64, Ordering::SeqCst);
        }
    }

    impl Clock for ManualClock {
        fn now(&self) -> Instant {
            self.start + Duration::from_millis(self.offset_ms.load(Ordering::SeqCst))
        }
    }

    fn counting_residency(
        idle_unload: IdleUnload,
        clock: Arc<dyn Clock>,
    ) -> (Arc<EncoderResidency<u32>>, Arc<AtomicUsize>) {
        let loads = Arc::new(AtomicUsize::new(0));
        let loader_loads = loads.clone();
        let residency = EncoderResidency::with_clock(
            "text",
            "cpu",
            idle_unload,
            move || {
                loader_loads.fetch_add(1, Ordering::SeqCst);
                Ok(7u32)
            },
            clock,
        );
        (Arc::new(residency), loads)
    }

    #[test]
    fn residency_loads_once_for_concurrent_requests() {
        let loads = Arc::new(AtomicUsize::new(0));
        let loader_loads = loads.clone();
        let residency = Arc::new(EncoderResidency::new(
            "text",
            "cpu",
            IdleUnload::Never,
            move || {
                loader_loads.fetch_add(1, Ordering::SeqCst);
                // A slow load, so the second request arrives while it runs.
                thread::sleep(Duration::from_millis(100));
                Ok(5u32)
            },
        ));

        let handles: Vec<_> = (0..2)
            .map(|_| {
                let residency = residency.clone();
                thread::spawn(move || residency.with_session(|value| Ok(*value)))
            })
            .collect();
        for handle in handles {
            assert_eq!(handle.join().expect("request thread").expect("request"), 5);
        }
        assert_eq!(loads.load(Ordering::SeqCst), 1);
        assert!(residency.status().loaded);
    }

    #[test]
    fn residency_unloads_after_idle_time() {
        let clock = ManualClock::new();
        let (residency, loads) =
            counting_residency(IdleUnload::After(Duration::from_secs(60)), clock.clone());
        residency.with_session(|_| Ok(())).expect("request");

        clock.advance(Duration::from_secs(30));
        assert!(!residency.unload_if_idle());
        let status = residency.status();
        assert!(status.loaded);
        assert_eq!(status.idle_ms, 30_000);

        clock.advance(Duration::from_secs(31));
        assert!(residency.unload_if_idle());
        let status = residency.status();
        assert!(!status.loaded);
        assert_eq!(status.device, "cpu");

        residency
            .with_session(|_| Ok(()))
            .expect("request after unload");
        assert_eq!(loads.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn residency_keeps_session_while_a_request_runs() {
        let clock = ManualClock::new();
        let (residency, _loads) =
            counting_residency(IdleUnload::After(Duration::from_secs(60)), clock.clone());
        residency.with_session(|_| Ok(())).expect("first request");
        clock.advance(Duration::from_secs(120));

        // A request is running when the unload check comes; the check waits for it, and the
        // request end counts as the last use.
        let (started_tx, started_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel::<()>();
        let request = {
            let residency = residency.clone();
            thread::spawn(move || {
                residency.with_session(|_| {
                    started_tx.send(()).expect("signal start");
                    release_rx.recv().expect("wait for release");
                    Ok(())
                })
            })
        };
        started_rx.recv().expect("request started");
        let unload = {
            let residency = residency.clone();
            thread::spawn(move || residency.unload_if_idle())
        };
        thread::sleep(Duration::from_millis(50));
        release_tx.send(()).expect("release request");
        request.join().expect("request thread").expect("request");
        assert!(!unload.join().expect("unload thread"));
        assert!(residency.status().loaded);
    }

    #[test]
    fn residency_failed_load_leaves_slot_empty() {
        let attempts = Arc::new(AtomicUsize::new(0));
        let loader_attempts = attempts.clone();
        let residency = EncoderResidency::new(
            "image",
            "directml",
            IdleUnload::After(Duration::from_secs(60)),
            move || -> Result<u32> {
                loader_attempts.fetch_add(1, Ordering::SeqCst);
                anyhow::bail!("bad model file")
            },
        );

        let err = residency.with_session(|_| Ok(())).expect_err("load fails");
        assert!(err.to_string().contains("bad model file"), "{err}");
        assert!(!residency.status().loaded);
        assert!(residency.with_session(|_| Ok(())).is_err());
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn negative_idle_time_never_unloads() {
        let clock = ManualClock::new();
        let (residency, loads) = counting_residency(
            IdleUnload::from_seconds(-1).expect("-1 is valid"),
            clock.clone(),
        );
        residency.with_session(|_| Ok(())).expect("request");
        clock.advance(Duration::from_secs(365 * 24 * 3600));
        assert!(!residency.unload_if_idle());
        assert!(residency.status().loaded);
        assert_eq!(loads.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn zero_idle_time_unloads_after_each_request() {
        let clock = ManualClock::new();
        let (residency, loads) =
            counting_residency(IdleUnload::from_seconds(0).expect("0 is valid"), clock);
        residency.with_session(|_| Ok(())).expect("first request");
        assert!(!residency.status().loaded);
        residency.with_session(|_| Ok(())).expect("second request");
        assert_eq!(loads.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn idle_unload_rejects_negative_values_other_than_minus_one() {
        assert!(IdleUnload::from_seconds(-2).is_err());
        assert_eq!(
            IdleUnload::from_seconds(60).expect("60 is valid"),
            IdleUnload::After(Duration::from_secs(60))
        );
    }
}
