<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# Current scope and remaining work

The Gateway supplies vehicle telemetry, exclusive driving control and typed
advisory integration for SDV Lab. Its maintained interfaces are described in
the [architecture](architecture.md), [telemetry contract](telemetry-contract.md)
and [external-control contract](external-control-contract.md).

## Implemented capabilities

- Native CARLA connection with version checks and ownership-aware cleanup.
- Frame-aligned VSS projection, GNSS, wheel dynamics and bounded latest-value
  telemetry through the TLS VISS endpoint.
- Authenticated external control, Manual/Autopilot/Safe Stop handover and
  bounded fail-safe behavior when a control owner expires or disconnects.
- Native Driving Control/Telemetry, repeatable driving maneuvers, brake-event
  scenarios and return-to-road behavior used by the integrated lab.
- Independent Brake/Tire advisory projection under the declared role and
  authority boundaries.

These are source capabilities, not a claim that every environment or failure
matrix has been qualified. The integration release's
[current baseline](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/5ea40f03b714fea924f6af88027abbbd1e018127/docs/qualification/current-baseline.md)
identifies the actual package, target and scoped API/UI evidence.

## Installed lab versus standalone development

The installed lab already supplies standalone CARLA and its matching Gateway,
without Unreal Editor. Packaging and operator startup belong to SDV Lab Setup
and Demo Control, not another standalone launcher in this repository.

The [native development setup](carla-setup-macos.md) and
[standalone launchers](macos-launchers.md) are specialist component tools.
Some retained standalone workflows use an Editor-based development environment;
they are not the installed lab's default path. Do not start them over an
installed simulator/controller session.

## Remaining boundaries

- Independent VISS exposure to another machine still requires its deployment
  trust/threat review and applicable remote-consumer proof. Local tests alone
  do not close that boundary.
- Cameras, LiDAR, radar and ultrasonic modeling are outside the current basic
  telemetry/control scope.
- ROS 2 remains optional and needs a concrete consumer requirement before
  adding an adapter.
- Broader recovery, negative and performance proofs remain tied to an exact
  product scenario; do not infer them from older milestone checklists.

Completed M0-M6 development diaries and their per-run claims remain in Git
history. Accepted [architecture decisions](decisions/) and current interface
contracts are retained because their rationale and obligations still apply.
