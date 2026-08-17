<!-- Copyright (c) 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# ADR 0011 — Reuse the M6.2 controller for hybrid brake scenarios

Status: accepted

## Context

The Brake Health demonstration needs both a reproducible stationary-obstacle
event and operator-generated braking. Starting the standalone scenario
controller beside the M6.2 manual controller would create two competing owners
of the ego actor and synchronous simulation clock. Restarting the actor or VISS
runtime at every handover would also interrupt telemetry and make the visual
story less representative of an in-vehicle system.

## Decision

Extend the M6.2 external-control process with an optional `scenario` capability
and keep it as the sole actor and tick owner. Protocol version 3 advertises the
configured modes and adds a restart generation for scripted mode while
retaining version 1 and 2 compatibility.

The hybrid profile spawns one stationary obstacle and one collision sensor for
the session. Entering scripted mode resets only scenario-owned physical state
and starts the deterministic state machine. Manual or autopilot selection keeps
the same ego actor and telemetry session. Manual takeover marks an unfinished
scripted attempt `ABORTED`. Automatic completion records `PASS` or `FAIL`,
selects safe stop, and leaves the interactive session available for manual use
or restart.

The standalone scenario controller remains the qualification path for exactly
one unattended attempt.

## Consequences

- The dashboard observes the same VISS stream through every handover.
- Manual braking generates the same physical and wheel telemetry as scripted
  braking.
- Only uninterrupted scripted attempts are eligible for automatic acceptance.
- Scenario reset is an explicit simulator operation and is never represented as
  a vehicle-platform or AosEdge update.
- AosVM, KUKSA, Vehicle Data Provider, authorization, and Cloud lifecycles remain
  outside this increment.
