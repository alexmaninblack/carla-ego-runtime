# M6.2 live manual and automatic handover

## Outcome

M6.2 keeps one ego vehicle in one continuous Town10HD simulation while the
operator moves among manual driving, CARLA Traffic Manager autopilot, and a
full-brake safe stop. The actor ID, chase camera, VSS telemetry session, GNSS
sensor, and 30 Hz tick owner remain unchanged during every handover.

## Operator controls

The native panel presents three complete mode buttons and equivalent keys:

- **Manual Control** — click the button or press M/Enter, then use the arrow
  keys for throttle, brake, and steering.
- **Autopilot** — click the button or press A; the vehicle continues driving
  when the panel loses focus.
- **Safe Stop** — click the button or press Space; automatic control is disabled
  and full brake is applied.
- **Exit** — press Escape or close the window to stop and clean up the session.

The panel starts in safe stop. Focus loss selects safe stop only while manual
mode is active, because the operator is then the active command source. It does
not stop an intentionally selected autopilot demonstration.

## Handover implementation

The external-control process is the only owner of the ego actor and synchronous
simulation clock. In manual and safe-stop modes it applies `VehicleControl`
directly. In autopilot mode it registers that same actor with a synchronous
Traffic Manager instance. Traffic Manager uses a fixed random seed, reduced
target speed, and disabled automatic lane changes for a predictable demo.

Autopilot can be selected only near an aligned driving lane. Automatic-to-
manual handover unregisters the actor from Traffic Manager and blends from the
last automatic control to the incoming manual control for 0.3 seconds. This
reduces the visible steering or pedal step without hiding the operator's new
command.

Unreal applies spectator transforms only on physics boundaries, so the camera
derives its target from the frame-aligned world snapshot and performs exactly
one smoothing update per 30 Hz physics tick. It no longer runs a competing 60
Hz RPC thread or issues catch-up camera updates. A 40 Hz live test was rejected
because Town10HD could not sustain it on the target Mac; the stable 30 Hz
cadence avoids the resulting long frames.

## Safety rules

- Client disconnect, ownership heartbeat expiry, panel close, and shutdown
  always disable autopilot and select safe stop.
- Manual commands expire after 250 ms; heartbeats cannot keep an old manual
  command active.
- Autopilot still requires the 1,000 ms ownership heartbeat, but does not
  require meaningless manual commands.
- Only one authenticated local client can own control.
- VISS stays read-only and carries telemetry, not actuator commands.

## Acceptance

Automated acceptance covers protocol v1 compatibility, all v2 transitions,
idempotent selections, monotonic command sequencing across modes, disconnect
safe stop, lane/alignment gating, pedal-safe handover blending, configuration
validation, product language, and native Swift type checking.

The operator-observed acceptance uses the desktop shortcut from a clean CARLA
state. It must show one vehicle driving manually, continuing automatically
after A or the Autopilot button, returning to responsive arrow-key control with
M/Enter, and stopping with Space. VSS telemetry must remain connected and the
final run manifest must report clean removal of the actor, socket, and token.

This acceptance completed on the pinned Apple Silicon baseline on 2026-08-12.
The operator accepted the live controls, automatic drive, and frame-locked
camera visually. The final cold-start session reached its first VSS frame in
26.5 seconds, maintained a measured 30.0 Hz average simulation rate, and ended
with VISS `CONNECTED` and `LIVE`. The runtime, controller, and keyboard client
all exited with code zero; safe stop was selected, ownership was released, and
the per-run socket and token were removed.
