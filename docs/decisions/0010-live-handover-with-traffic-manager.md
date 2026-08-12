# ADR 0010 — Live handover with one tick owner and Traffic Manager

Status: accepted for M6.2 implementation

## Context

The demonstration needs to switch repeatedly between operator control and
automatic driving without replacing the vehicle, restarting the scene, or
interrupting telemetry. The M5 BehaviorAgent controller and M6 external-
control controller are separate processes, and each is designed to own the
ego actor and synchronous clock. Running both for a live handover would create
ambiguous tick and vehicle ownership.

## Decision

Extend the M6 external-control process with three explicit modes: `safe_stop`,
`manual`, and `autopilot`. The process remains the sole actor and tick owner in
all modes. Manual commands continue through the authenticated local contract;
automatic mode registers the existing actor with CARLA's synchronous Traffic
Manager on a configured port.

Protocol version 2 adds `set_mode`. Version 1 remains accepted for existing M6
clients and retains its manual-on-acquire behaviour. The command sequence is
monotonic for the entire ownership session and never resets at a handover.

Traffic Manager activation is gated by distance and heading relative to a
driving-lane waypoint. Automatic-to-manual handover disables Traffic Manager
and interpolates its last control into the current manual command for a short,
bounded interval. Loss of the external session always disables Traffic Manager
and selects the existing full-brake safe stop.

## Consequences

- The actor ID, physics state, sensors, chase camera, and VISS stream survive
  every mode change.
- No competing BehaviorAgent process or second synchronous tick owner is
  introduced.
- The automatic demonstration follows Traffic Manager behaviour rather than
  the fixed M5 BehaviorAgent route.
- The local control panel must keep heartbeats active in every mode.
- Route rejoin, remote control, and higher-level mission selection remain
  separate future work.
