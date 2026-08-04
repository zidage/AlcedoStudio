//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { LivenessWatchdog } from "../src/liveness.js";

describe("LivenessWatchdog", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  it("does not fire before the heartbeat gap reaches the threshold", () => {
    const onDeadlock = vi.fn();
    const watchdog = new LivenessWatchdog(5000, onDeadlock);
    watchdog.start();
    watchdog.observeHeartbeat();
    vi.advanceTimersByTime(4000);
    expect(onDeadlock).not.toHaveBeenCalled();
    expect(watchdog.isDeadlocked).toBe(false);
    watchdog.stop();
  });

  it("fires onDeadlock once after the gap exceeds the threshold", () => {
    const onDeadlock = vi.fn();
    const watchdog = new LivenessWatchdog(5000, onDeadlock);
    watchdog.start();
    watchdog.observeHeartbeat();
    vi.advanceTimersByTime(6000);
    expect(onDeadlock).toHaveBeenCalledTimes(1);
    expect(watchdog.isDeadlocked).toBe(true);
  });

  it("stops checking after firing so onDeadlock is never called twice", () => {
    const onDeadlock = vi.fn();
    const watchdog = new LivenessWatchdog(5000, onDeadlock);
    watchdog.start();
    watchdog.observeHeartbeat();
    vi.advanceTimersByTime(6000);
    vi.advanceTimersByTime(6000);
    expect(onDeadlock).toHaveBeenCalledTimes(1);
  });

  it("resets the gap when a heartbeat arrives", () => {
    const onDeadlock = vi.fn();
    const watchdog = new LivenessWatchdog(5000, onDeadlock);
    watchdog.start();
    watchdog.observeHeartbeat();
    vi.advanceTimersByTime(4000);
    watchdog.observeHeartbeat();
    vi.advanceTimersByTime(4000);
    expect(onDeadlock).not.toHaveBeenCalled();
    watchdog.stop();
  });
});