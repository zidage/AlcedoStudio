//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * GUI-thread liveness watchdog. The probe emits a `heartbeat` event every 250 ms
 * from the GUI thread; a gap larger than the liveness threshold means the GUI
 * thread is deadlocked. This watchdog tracks the last heartbeat time and fires
 * the `onDeadlock` callback once when the gap exceeds the threshold.
 *
 * Heartbeat proves GUI-thread liveness only (a worker-thread deadlock that does
 * not block the GUI thread surfaces as a hung expect, not a deadlock verdict).
 */

/** Clock injection so tests can advance time deterministically. */
export type Clock = () => number;

/** Default check interval, matching the probe's heartbeat cadence. */
export const HEARTBEAT_CHECK_INTERVAL_MS = 250;

/** Watches for a heartbeat gap exceeding `thresholdMs` and fires `onDeadlock` once. */
export class LivenessWatchdog {
  private lastHeartbeatAt: number;
  private timer: NodeJS.Timeout | undefined;
  private deadlocked = false;

  /**
   * @param thresholdMs gap after which the GUI thread is considered deadlocked.
   * @param onDeadlock fired exactly once when the gap first exceeds the threshold.
   * @param clock injectable clock; defaults to `Date.now`.
   */
  constructor(
    private readonly thresholdMs: number,
    private readonly onDeadlock: () => void,
    private readonly clock: Clock = () => Date.now(),
  ) {
    this.lastHeartbeatAt = this.clock();
  }

  /** Begins periodic gap checking. Call after the run starts. */
  start(): void {
    if (this.timer !== undefined) return;
    this.timer = setInterval(() => this.check(), HEARTBEAT_CHECK_INTERVAL_MS);
  }

  /** Records a heartbeat. Called for every `heartbeat` probe event. */
  observeHeartbeat(): void {
    if (this.deadlocked) return;
    this.lastHeartbeatAt = this.clock();
  }

  /** Returns true once a heartbeat gap has exceeded the threshold. */
  get isDeadlocked(): boolean {
    return this.deadlocked;
  }

  /** Stops checking. Safe to call after deadlock or on run end. */
  stop(): void {
    if (this.timer !== undefined) {
      clearInterval(this.timer);
      this.timer = undefined;
    }
  }

  private check(): void {
    if (this.deadlocked) return;
    if (this.clock() - this.lastHeartbeatAt > this.thresholdMs) {
      this.deadlocked = true;
      this.stop();
      this.onDeadlock();
    }
  }
}