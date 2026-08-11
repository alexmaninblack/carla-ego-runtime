# ADR 0001: Separate public runtime repository

- Status: Accepted
- Date: 2026-08-11

## Context

The CARLA simulator and its Unreal Engine dependency are large platform source
trees with different access and redistribution rules. The ego-vehicle runtime
is an application with its own lifecycle, telemetry contract, tests, and
release cadence.

## Decision

Develop the application in a standalone public repository named
`alexmaninblack/carla-ego-runtime`. Link against an installed LibCarla package;
do not vendor CARLA or Unreal Engine and do not use either as a Git submodule.

## Consequences

The application can evolve independently and remain publicly redistributable.
Compatibility with CARLA must be explicit and tested against a pinned commit.
