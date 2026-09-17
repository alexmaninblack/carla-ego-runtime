import AppKit
import Foundation
import Darwin

struct Control: Encodable {
    let throttle: Double
    let brake: Double
    let steering: Double
}

func jsonLine(_ event: String, fields: [String: Any] = [:]) {
    var record = fields
    record["source"] = "keyboard_control_ui"
    record["event"] = event
    if let data = try? JSONSerialization.data(withJSONObject: record, options: [.sortedKeys]),
       let text = String(data: data, encoding: .utf8) {
        print(text)
        fflush(stdout)
    }
}

final class ControlView: NSView {
    // Fixed palette shared with native telemetry, independent of macOS theme.
    private let ink = NSColor(calibratedWhite: 0.94, alpha: 1)
    private let muted = NSColor(calibratedRed: 0.64, green: 0.71, blue: 0.77, alpha: 1)
    private let surface = NSColor(calibratedRed: 0.10, green: 0.14, blue: 0.18, alpha: 1)
    var mode = "safe_stop"
    var awaitingOperator = false
    var availableModes = Set(["safe_stop", "manual", "autopilot"])
    var connected = false
    var statusDetail = "CONNECTING..."
    var throttle = 0.0
    var brake = 1.0
    var steering = 0.0
    var pressed = Set<UInt16>()
    var onControl: ((Control) -> Void)?
    var onMode: ((String) -> Void)?
    var onExit: (() -> Void)?
    var onConnectivity: (() -> Void)?
    var externalState = "UNKNOWN"
    var externalReadAt: TimeInterval = 0
    var externalBusy = false
    private var connectivityRect: NSRect {
        NSRect(x: 18, y: availableModes.contains("scenario") ? 152 : 112,
               width: 484, height: availableModes.contains("scenario") ? 30 : 48)
    }
    private var lastUpdate = ProcessInfo.processInfo.systemUptime

    private let statusRect = NSRect(x: 24, y: 526, width: 472, height: 52)
    private let scenarioButtonRect = NSRect(x: 18, y: 94, width: 484, height: 54)
    private let manualButtonRect = NSRect(x: 18, y: 22, width: 150, height: 48)
    private let autopilotButtonRect = NSRect(x: 185, y: 22, width: 150, height: 48)
    private let stopButtonRect = NSRect(x: 352, y: 22, width: 150, height: 48)
    private let throttleKeyRect = NSRect(x: 190, y: 456, width: 140, height: 44)
    private let steerLeftKeyRect = NSRect(x: 24, y: 406, width: 140, height: 44)
    private let steerRightKeyRect = NSRect(x: 356, y: 406, width: 140, height: 44)
    private let brakeKeyRect = NSRect(x: 190, y: 356, width: 140, height: 44)

    override var acceptsFirstResponder: Bool { true }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        // Keep the existing drawing and hit-test coordinates in one logical canvas.
        setBoundsSize(NSSize(width: 520, height: 600))
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
        Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 36, 76, 46:
            if availableModes.contains("manual") { onMode?("manual") }  // Enter or M
        case 0:
            if availableModes.contains("autopilot") { onMode?("autopilot") }  // A
        case 1:
            if availableModes.contains("scenario") { onMode?("scenario") }  // S
        case 49: onMode?("safe_stop")            // Space
        case 53: onExit?()                        // Escape
        case 123, 124, 125, 126:
            if mode == "manual" && !awaitingOperator { pressed.insert(event.keyCode) }
        default: super.keyDown(with: event)
        }
    }

    override func keyUp(with event: NSEvent) {
        pressed.remove(event.keyCode)
    }

    func setConnected() {
        connected = true
        setMode("safe_stop")
    }

    func setAvailableModes(_ modes: [String]) {
        availableModes = Set(modes)
        availableModes.insert("safe_stop")
        needsDisplay = true
    }

    func setMode(_ selected: String, reason: String = "") {
        mode = selected
        awaitingOperator = selected == "manual" && reason == "manual_ready"
        pressed.removeAll()
        if selected == "manual" {
            statusDetail = awaitingOperator
                ? "READY — SELECT MANUAL OR AUTOPILOT"
                : "MANUAL CONTROL — ARROWS ACTIVE"
            throttle = 0
            brake = awaitingOperator ? 1 : 0
            steering = 0
        } else if selected == "autopilot" {
            statusDetail = "AUTOPILOT — MODE SELECTED"
            throttle = 0
            brake = 0
            steering = 0
        } else if selected == "scenario" {
            statusDetail = reason == "exercise_tire"
                ? "TIRE TEST MANEUVER — RUNNING"
                : "SCRIPTED BRAKE SCENARIO — RUNNING"
            throttle = 0
            brake = 0
            steering = 0
        } else {
            if reason == "scenario_complete" {
                statusDetail = "SCENARIO PASSED — PRESS M, A, OR S TO RESTART"
            } else if reason == "scenario_failed" {
                statusDetail = "SCENARIO FAILED — SAFE STOP ACTIVE"
            } else {
                statusDetail = connected
                    ? "SAFE STOP — SELECT A DRIVING MODE"
                    : "SAFE STOP — CONNECTION LOST"
            }
            throttle = 0
            brake = 1
            steering = 0
        }
        needsDisplay = true
    }

    func requestingMode(_ requested: String) {
        statusDetail = requested == "scenario"
            ? "RESETTING AND STARTING SCRIPTED SCENARIO..."
            : requested == "autopilot"
            ? "SWITCHING TO AUTOPILOT..."
            : requested == "manual"
                ? "SWITCHING TO MANUAL CONTROL..."
                : "SELECTING SAFE STOP..."
        needsDisplay = true
    }

    func rejectMode(_ rejected: String) {
        if rejected == "autopilot" {
            statusDetail = "AUTOPILOT UNAVAILABLE — RETURN TO ROAD"
        } else if rejected == "scenario" {
            statusDetail = "SCRIPTED SCENARIO UNAVAILABLE"
        } else {
            statusDetail = "MODE CHANGE FAILED"
        }
        needsDisplay = true
    }

    func connectionLost() {
        connected = false
        setMode("safe_stop")
    }

    private func approach(_ current: Double, _ target: Double, _ delta: Double) -> Double {
        target > current ? min(target, current + delta) : max(target, current - delta)
    }

    private func tick() {
        let now = ProcessInfo.processInfo.systemUptime
        let elapsed = max(0, min(now - lastUpdate, 0.25))
        lastUpdate = now
        // The controller holds full brake until an explicit driving-mode choice.
        // No neutral command may release that brake or end manual_ready.
        if awaitingOperator { return }
        if mode == "manual" && !(window?.isKeyWindow ?? false) {
            onMode?("safe_stop")
            return
        }
        if mode == "manual" {
            let braking = pressed.contains(125)
            let throttleTarget = pressed.contains(126) && !braking ? 0.55 : 0.0
            let brakeTarget = braking ? 0.75 : 0.0
            brake = approach(brake, brakeTarget, 3.0 * elapsed)
            // The protocol forbids overlapping pedals, including the release
            // ramp after the operator has already let go of the brake key.
            throttle = braking || brake > 0 ? 0 : approach(throttle, throttleTarget, 1.25 * elapsed)
            let steeringTarget: Double
            if pressed.contains(123) && !pressed.contains(124) {
                steeringTarget = -0.55
            } else if pressed.contains(124) && !pressed.contains(123) {
                steeringTarget = 0.55
            } else {
                steeringTarget = 0
            }
            steering = approach(
                steering,
                steeringTarget,
                (steeringTarget == 0 ? 2.8 : 1.6) * elapsed
            )
            onControl?(Control(throttle: throttle, brake: brake, steering: steering))
        }
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if manualButtonRect.contains(point) {
            onMode?("manual")
        } else if autopilotButtonRect.contains(point) {
            onMode?("autopilot")
        } else if stopButtonRect.contains(point) {
            onMode?("safe_stop")
        } else if availableModes.contains("scenario") && scenarioButtonRect.contains(point) {
            onMode?("scenario")
        } else if onConnectivity != nil && !externalBusy && connectivityRect.contains(point) {
            onConnectivity?()
        }
    }

    private func text(
        _ value: String,
        x: CGFloat,
        y: CGFloat,
        size: CGFloat,
        color: NSColor,
        bold: Bool = false,
        alignment: NSTextAlignment = .left,
        width: CGFloat = 432
    ) {
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = alignment
        let font = bold ? NSFont.boldSystemFont(ofSize: size) : NSFont.systemFont(ofSize: size)
        value.draw(
            in: NSRect(x: x, y: y, width: width, height: 50),
            withAttributes: [.font: font, .foregroundColor: color, .paragraphStyle: paragraph]
        )
    }

    private func centeredText(
        _ value: String,
        in rect: NSRect,
        size: CGFloat,
        color: NSColor,
        bold: Bool = false
    ) {
        let font = bold ? NSFont.boldSystemFont(ofSize: size) : NSFont.systemFont(ofSize: size)
        let attributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: color,
        ]
        let measured = value.size(withAttributes: attributes)
        value.draw(
            at: NSPoint(x: rect.midX - measured.width / 2, y: rect.midY - measured.height / 2),
            withAttributes: attributes
        )
    }

    private func roundedCard(
        _ rect: NSRect,
        fill: NSColor,
        border: NSColor,
        lineWidth: CGFloat = 1
    ) {
        let path = NSBezierPath(roundedRect: rect, xRadius: 8, yRadius: 8)
        fill.setFill()
        path.fill()
        border.setStroke()
        path.lineWidth = lineWidth
        path.stroke()
    }

    private func keycap(_ title: String, rect: NSRect, enabled: Bool) {
        roundedCard(
            rect,
            fill: enabled
                ? NSColor(calibratedRed: 0.16, green: 0.23, blue: 0.29, alpha: 1)
                : surface,
            border: enabled
                ? NSColor(calibratedRed: 0.39, green: 0.56, blue: 0.68, alpha: 1)
                : NSColor(calibratedWhite: 0.23, alpha: 1),
            lineWidth: enabled ? 1.5 : 1
        )
        centeredText(
            title,
            in: rect,
            size: 13,
            color: enabled ? ink : muted,
            bold: true
        )
    }

    private func actionButton(
        _ title: String,
        rect: NSRect,
        fill: NSColor,
        border: NSColor,
        textColor: NSColor,
        selected: Bool
    ) {
        let background = selected ? border.withAlphaComponent(0.28) : surface
        roundedCard(rect, fill: background, border: border, lineWidth: selected ? 3 : 1.5)
        centeredText(title, in: rect, size: 12, color: ink, bold: true)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(calibratedRed: 0.065, green: 0.085, blue: 0.11, alpha: 1).setFill()
        bounds.fill()

        let statusFill: NSColor
        let statusBorder: NSColor
        if mode == "manual" {
            statusFill = NSColor(calibratedRed: 0.82, green: 0.93, blue: 0.85, alpha: 1)
            statusBorder = NSColor(calibratedRed: 0.25, green: 0.61, blue: 0.34, alpha: 1)
        } else if mode == "autopilot" {
            statusFill = NSColor(calibratedRed: 0.82, green: 0.89, blue: 0.97, alpha: 1)
            statusBorder = NSColor(calibratedRed: 0.20, green: 0.46, blue: 0.75, alpha: 1)
        } else if mode == "scenario" {
            statusFill = NSColor(calibratedRed: 0.93, green: 0.87, blue: 0.98, alpha: 1)
            statusBorder = NSColor(calibratedRed: 0.48, green: 0.28, blue: 0.68, alpha: 1)
        } else {
            statusFill = NSColor(calibratedRed: 0.96, green: 0.84, blue: 0.84, alpha: 1)
            statusBorder = NSColor(calibratedRed: 0.82, green: 0.34, blue: 0.34, alpha: 1)
        }
        roundedCard(statusRect, fill: statusFill.withAlphaComponent(0.10), border: statusBorder, lineWidth: 1.5)
        centeredText(statusDetail, in: statusRect, size: 15, color: ink, bold: true)

        let arrowsEnabled = mode == "manual"
        keycap("↑  THROTTLE", rect: throttleKeyRect, enabled: arrowsEnabled)
        keycap("←  STEER LEFT", rect: steerLeftKeyRect, enabled: arrowsEnabled)
        keycap("STEER RIGHT  →", rect: steerRightKeyRect, enabled: arrowsEnabled)
        keycap("↓  BRAKE", rect: brakeKeyRect, enabled: arrowsEnabled)

        if mode == "autopilot" {
            text(
                "VEHICLE CONTROLLED BY TRAFFIC MANAGER",
                x: 44,
                y: 270,
                size: 12,
                color: muted,
                bold: true,
                alignment: .center
            )
        } else if mode == "scenario" {
            text(
                "VEHICLE CONTROLLED BY THE BRAKE-EVENT STATE MACHINE",
                x: 44,
                y: 270,
                size: 12,
                color: muted,
                bold: true,
                alignment: .center
            )
        } else {
            text("THROTTLE", x: 46, y: 302, size: 12, color: muted)
            text(
                String(format: "%.2f", throttle),
                x: 44,
                y: 302,
                size: 13,
                color: ink,
                bold: true,
                alignment: .right
            )
            text("BRAKE", x: 46, y: 274, size: 12, color: muted)
            text(
                String(format: "%.2f", brake),
                x: 44,
                y: 274,
                size: 13,
                color: ink,
                bold: true,
                alignment: .right
            )
            text("STEERING", x: 46, y: 246, size: 12, color: muted)
            text(
                String(format: "%+.2f", steering),
                x: 44,
                y: 246,
                size: 13,
                color: ink,
                bold: true,
                alignment: .right
            )
        }

        text(
            availableModes.contains("scenario")
                ? "S: SCRIPTED/RESTART   M: MANUAL   A: AUTOPILOT   SPACE: STOP   ESC: EXIT"
                : "M / ENTER: MANUAL     A: AUTOPILOT     SPACE: SAFE STOP     ESC: EXIT",
            x: 44,
            y: 205,
            size: 9,
            color: muted,
            alignment: .center
        )
        text(
            "Focus loss stops manual control; scripted and autopilot modes continue.",
            x: 44,
            y: 187,
            size: 10,
            color: muted,
            alignment: .center
        )

        if onConnectivity != nil {
            let fresh = externalReadAt > 0 && ProcessInfo.processInfo.systemUptime - externalReadAt <= 15
            let title = externalBusy ? "EXTERNAL NETWORK · CHANGING…" :
                !fresh && ["ON", "OFF"].contains(externalState) ? "EXTERNAL NETWORK: STALE · CHECK" :
                externalState == "ON" ? "EXTERNAL NETWORK: ON · DISCONNECT" :
                externalState == "OFF" ? "EXTERNAL NETWORK: OFF · RECONNECT" :
                externalState == "NO_VEHICLE" ? "EXTERNAL NETWORK · SELECT A VEHICLE" :
                "EXTERNAL NETWORK: UNKNOWN · CHECK"
            let offline = fresh && externalState == "OFF"
            roundedCard(connectivityRect,
                fill: offline ? NSColor(calibratedRed: 0.30, green: 0.22, blue: 0.12, alpha: 1) : surface,
                border: offline ? .systemOrange : .gray, lineWidth: 1.5)
            centeredText(title, in: connectivityRect, size: 14, color: ink, bold: true)
        }

        if availableModes.contains("scenario") {
            actionButton(
                mode == "scenario" ? "RESTART SCRIPTED SCENARIO" : "START SCRIPTED SCENARIO",
                rect: scenarioButtonRect,
                fill: NSColor(calibratedRed: 0.90, green: 0.82, blue: 0.97, alpha: 1),
                border: NSColor(calibratedRed: 0.48, green: 0.28, blue: 0.68, alpha: 1),
                textColor: NSColor(calibratedRed: 0.34, green: 0.16, blue: 0.54, alpha: 1),
                selected: mode == "scenario"
            )
        }

        actionButton(
            "MANUAL CONTROL",
            rect: manualButtonRect,
            fill: NSColor(calibratedRed: 0.78, green: 0.91, blue: 0.81, alpha: 1),
            border: NSColor(calibratedRed: 0.26, green: 0.62, blue: 0.34, alpha: 1),
            textColor: NSColor(calibratedRed: 0.10, green: 0.42, blue: 0.18, alpha: 1),
            selected: mode == "manual"
        )
        actionButton(
            "AUTOPILOT",
            rect: autopilotButtonRect,
            fill: NSColor(calibratedRed: 0.78, green: 0.87, blue: 0.97, alpha: 1),
            border: NSColor(calibratedRed: 0.20, green: 0.46, blue: 0.75, alpha: 1),
            textColor: NSColor(calibratedRed: 0.10, green: 0.31, blue: 0.58, alpha: 1),
            selected: mode == "autopilot"
        )
        actionButton(
            "SAFE STOP",
            rect: stopButtonRect,
            fill: NSColor(calibratedRed: 0.95, green: 0.80, blue: 0.80, alpha: 1),
            border: NSColor(calibratedRed: 0.78, green: 0.27, blue: 0.27, alpha: 1),
            textColor: NSColor(calibratedRed: 0.65, green: 0.10, blue: 0.10, alpha: 1),
            selected: mode == "safe_stop"
        )
    }
}

final class TelemetryView: NSView {
    var sample: [String: Any] = [:]
    var receivedAt: TimeInterval = 0
    var disconnected = false
    private let tabs = NSSegmentedControl()
    private var selectedPage = 0
    var onPageSelected: (() -> Void)?
    private var lastState = ""
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { false }

    override init(frame: NSRect) {
        super.init(frame: frame)
        tabs.segmentCount = 3
        for (index, title) in ["Dashboard", "Vehicle", "Data"].enumerated() {
            tabs.setLabel(title, forSegment: index)
        }
        tabs.selectedSegment = 0
        tabs.trackingMode = .selectOne
        tabs.segmentStyle = .rounded
        tabs.appearance = NSAppearance(named: .darkAqua)
        tabs.font = NSFont.systemFont(ofSize: 14, weight: .medium)
        tabs.target = self
        tabs.action = #selector(selectPage(_:))
        tabs.setAccessibilityLabel("Telemetry sections")
        addSubview(tabs)
        layoutTabs()
    }
    required init?(coder: NSCoder) { fatalError("Not supported") }
    private func layoutTabs() {
        let width = max(0, bounds.width - 40)
        tabs.frame = NSRect(x: 20, y: 50, width: width, height: 36)
        for index in 0..<3 { tabs.setWidth(max(0, (width - 6) / 3), forSegment: index) }
    }
    @objc private func selectPage(_ sender: NSSegmentedControl) {
        selectedPage = sender.selectedSegment
        needsDisplay = true
        // Keep driving shortcuts with the controller after changing a display tab.
        onPageSelected?()
    }

    var dataState: String {
        if disconnected { return "DISCONNECTED" }
        if receivedAt == 0 { return "WAITING" }
        if ProcessInfo.processInfo.systemUptime - receivedAt > 5 { return "STALE" }
        return sample["state"] as? String ?? "WAITING"
    }
    override func setFrameSize(_ size: NSSize) {
        super.setFrameSize(size)
        setBoundsSize(size)
        layoutTabs()
    }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            let state = self.dataState
            if state != self.lastState {
                self.lastState = state
                jsonLine("telemetry_display_state", fields: ["state": state])
            }
            self.needsDisplay = true
        }
    }
    func accept(_ value: [String: Any]) {
        guard value["schemaVersion"] as? Int == 1,
              value["source"] as? String == "gateway-viss",
              let state = value["state"] as? String,
              ["WAITING", "LIVE", "STALE", "DISCONNECTED"].contains(state),
              let signals = value["signals"] as? [String: String], signals.count <= 128
        else { return }
        sample = value
        receivedAt = ProcessInfo.processInfo.systemUptime
        disconnected = state == "DISCONNECTED"
        needsDisplay = true
    }
    private func text(_ value: String, _ x: CGFloat, _ y: CGFloat, _ size: CGFloat = 16,
                      _ color: NSColor = .white, width: CGFloat = 550, bold: Bool = false,
                      alignment: NSTextAlignment = .left) {
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byTruncatingTail
        paragraph.alignment = alignment
        value.draw(in: NSRect(x: x, y: y, width: width, height: size * 1.6),
                   withAttributes: [.font: bold ? NSFont.boldSystemFont(ofSize: size) : NSFont.monospacedDigitSystemFont(ofSize: size, weight: .regular),
                                    .foregroundColor: color, .paragraphStyle: paragraph])
    }
    private func value(_ path: String, _ precision: Int = 1) -> String {
        guard let raw = (sample["signals"] as? [String: String])?["Vehicle." + path],
              let number = Double(raw), number.isFinite else { return "—" }
        return String(format: "%.*f", precision, number)
    }
    private var motionText: String {
        guard dataState == "LIVE" else { return "NO FRESH DATA" }
        let signals = sample["signals"] as? [String: String] ?? [:]
        guard let raw = signals["Vehicle.Speed"], let speed = Double(raw), speed.isFinite,
              signals["Vehicle.CarlaSimulation.Reset.InProgress"] != "true",
              signals["Vehicle.CarlaSimulation.Reset.Discontinuity"] != "true"
        else { return "MOTION UNKNOWN" }
        // Physical movement only; no update permission or platform state is inferred.
        if abs(speed) <= 0.3 { return "STOPPED" }
        return signals["Vehicle.CarlaSimulation.Control.ActiveMode"] == "SAFE_STOP"
            ? "STOPPING" : "MOVING"
    }
    private func pedal(_ title: String, _ path: String, _ y: CGFloat, _ color: NSColor) {
        let signals = sample["signals"] as? [String: String] ?? [:]
        let raw = Double(signals["Vehicle." + path] ?? "")
        let ink: NSColor = dataState == "LIVE" ? .white : .gray
        text(title, 20, y, 15, .lightGray, width: 110)
        let track = NSRect(x: 132, y: y + 6, width: max(0, bounds.width - 210), height: 10)
        NSColor(calibratedWhite: 0.24, alpha: 1).setFill()
        NSBezierPath(roundedRect: track, xRadius: 5, yRadius: 5).fill()
        if let raw = raw, raw.isFinite {
            (dataState == "LIVE" ? color : NSColor.gray).setFill()
            NSBezierPath(roundedRect: NSRect(x: track.minX, y: track.minY,
                width: track.width * CGFloat(min(100, max(0, raw))) / 100, height: track.height), xRadius: 5, yRadius: 5).fill()
        }
        let amount = value(path, 0)
        text(amount == "—" ? amount : amount + "%", bounds.width - 70, y, 16, ink, width: 50, alignment: .right)
    }
    private func separator(_ y: CGFloat) {
        NSColor(calibratedWhite: 0.23, alpha: 1).setFill()
        NSRect(x: 20, y: y, width: max(0, bounds.width - 40), height: 1).fill()
    }
    private func metric(_ title: String, _ amount: String, _ index: Int, _ ink: NSColor) {
        let width = (bounds.width - 64) / 3
        let x = 20 + CGFloat(index) * (width + 12)
        text(title, x, 170, 14, .lightGray, width: width)
        text(amount, x, 196, 20, ink, width: width, bold: true)
    }
    private func dataRow(_ title: String, _ amount: String, _ y: CGFloat, _ ink: NSColor) {
        text(title, 20, y, 14, .lightGray, width: 142)
        text(amount, 166, y, 15, ink, width: bounds.width - 186, alignment: .right)
    }
    private func advisory(_ team: String) -> String {
        // Read only the existing dashboard contract. No Cloud/VM read and no
        // backend-synthetic result may turn into a vehicle advisory.
        guard dataState == "LIVE", let raw = (sample["advisory"] as? [String: String])?[team] else { return "Unavailable" }
        return ["NOT_AVAILABLE": "Not available", "WAITING_FOR_SERVICE": "Waiting for service",
                "MONITORING": "Monitoring", "UNAVAILABLE": "Unavailable", "NONE": "None",
                "EXPIRED": "Expired",
                "INSPECTION_RECOMMENDED": "Inspection recommended",
                "TIRE_INSPECTION_RECOMMENDED": "Inspection recommended",
                "TIRE_REPLACEMENT_RECOMMENDED": "Replacement recommended"][raw] ?? "Unavailable"
    }
    private func advisoryColor(_ team: String) -> NSColor {
        guard dataState == "LIVE" else { return .lightGray }
        let state = (sample["advisory"] as? [String: String])?[team] ?? "UNAVAILABLE"
        if state == "MONITORING" { return .systemCyan }
        if state == "NOT_AVAILABLE" || state == "WAITING_FOR_SERVICE" { return .lightGray }
        return .systemOrange
    }
    private func drawVehicle(_ ink: NSColor) {
        let steering = value("Chassis.Axle.Row1.SteeringAngle")
        let rpm = value("Powertrain.CombustionEngine.Speed", 0)
        metric("Steering", steering == "—" ? steering : steering + "°", 0, ink)
        metric("Gear", value("Powertrain.Transmission.CurrentGear", 0), 1, ink)
        metric("Engine", rpm == "—" ? rpm : rpm + " RPM", 2, ink)
        text("Wheel speed", 20, 234, 15, .white, width: 180)
        text("km/h", bounds.width - 90, 234, 14, .lightGray, width: 70, alignment: .right)
        let area = NSRect(x: 20, y: 264, width: bounds.width - 40, height: max(140, bounds.height - 284))
        NSColor(calibratedRed: 0.105, green: 0.15, blue: 0.195, alpha: 1).setFill()
        NSBezierPath(roundedRect: area, xRadius: 12, yRadius: 12).fill()
        let car = NSRect(x: area.midX - 41, y: area.minY + 12, width: 82, height: area.height - 24)
        let line = NSColor(calibratedRed: 0.27, green: 0.35, blue: 0.42, alpha: 1)
        line.setStroke()
        let outline = NSBezierPath(roundedRect: car, xRadius: 20, yRadius: 20)
        outline.lineWidth = 2
        outline.stroke()
        line.setFill()
        NSBezierPath(roundedRect: NSRect(x: car.minX + 14, y: car.minY + 14, width: 54, height: 23), xRadius: 6, yRadius: 6).fill()
        NSBezierPath(roundedRect: NSRect(x: car.minX + 14, y: car.maxY - 27, width: 54, height: 11), xRadius: 3, yRadius: 3).fill()
        text("↑", car.minX, car.midY - 12, 20, .lightGray, width: car.width, alignment: .center)
        let column = (area.width - 106) / 2
        let wheels = [("Front left", "Row1.Wheel.Left"), ("Front right", "Row1.Wheel.Right"),
                      ("Rear left", "Row2.Wheel.Left"), ("Rear right", "Row2.Wheel.Right")]
        for (index, wheel) in wheels.enumerated() {
            let x = index % 2 == 0 ? area.minX : area.midX + 53
            let y = area.minY + CGFloat(index / 2) * area.height / 2 + (area.height / 2 - 54) / 2
            text(wheel.0, x, y, 14, .lightGray, width: column, alignment: .center)
            text(value("Chassis.Axle." + wheel.1 + ".Speed"), x, y + 24, 22, ink, width: column, bold: true, alignment: .center)
        }
    }
    private func drawData(_ ink: NSColor) {
        let metrics = sample["metrics"] as? [String: String] ?? [:]
        metric("Simulation", metrics["simulation"] ?? "—", 0, ink)
        metric("Received", metrics["delivery"] ?? "—", 1, ink)
        metric("Latency", metrics["latency"] ?? "—", 2, ink)
        separator(234)
        dataRow("Latitude", value("CurrentLocation.Latitude", 5), 252, ink)
        dataRow("Longitude", value("CurrentLocation.Longitude", 5), 288, ink)
        let exercise = sample["exercise"] as? String ?? "—"
        dataRow("Session", exercise.components(separatedBy: " / ").first ?? "—", 324, ink)
        dataRow("Generation", value("CarlaSimulation.Reset.Generation", 0), 360, ink)
        let updated = (sample["updatedAt"] as? String ?? "—").replacingOccurrences(of: "T", with: " ").replacingOccurrences(of: "Z", with: "")
        dataRow("Last event · UTC", updated, 396, ink)
    }
    override func draw(_ dirtyRect: NSRect) {
        NSColor(calibratedRed: 0.065, green: 0.085, blue: 0.11, alpha: 1).setFill()
        bounds.fill()
        let live = dataState == "LIVE"
        let accent = live ? NSColor.systemGreen : NSColor.systemOrange
        let ink: NSColor = live ? .white : .gray
        text(sample["vehicle"] as? String ?? "Waiting for vehicle", 20, 18, 16, .white, width: bounds.width - 162)
        text(dataState, bounds.width - 142, 18, 14, accent, width: 122, bold: true, alignment: .right)
        text(value("Speed"), 20, 105, 30, ink, width: 120, bold: true)
        text("km/h", 145, 119, 14, .lightGray, width: 60)
        let mode = (sample["signals"] as? [String: String])?["Vehicle.CarlaSimulation.Control.ActiveMode"] ?? "UNKNOWN"
        text(live ? mode.replacingOccurrences(of: "_", with: " ") : "MODE UNKNOWN", 224, 103, 16, ink, width: bounds.width - 244, alignment: .right)
        text(motionText, 224, 128, 14, ink, width: bounds.width - 244, alignment: .right)
        separator(156)
        if selectedPage == 1 {
            drawVehicle(ink)
        } else if selectedPage == 2 {
            drawData(ink)
        } else {
            pedal("Accelerator", "Chassis.Accelerator.PedalPosition", 182, .systemGreen)
            pedal("Brake", "Chassis.Brake.PedalPosition", 222, .systemRed)
            separator(265)
            text("DRIVER ADVISORY", 20, 286, 17, .white, bold: true)
            dataRow("Brake", advisory("brake"), 329, advisoryColor("brake"))
            dataRow("Tire", advisory("tire"), 371, advisoryColor("tire"))
        }
    }
}

final class DrivingWorkspace: NSView {
    let control = ControlView(frame: .zero)
    let telemetry = TelemetryView(frame: .zero)
    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(control)
        addSubview(telemetry)
        telemetry.onPageSelected = { [weak self] in
            guard let self = self else { return }
            self.window?.makeFirstResponder(self.control)
        }
        resizeSubviews(withOldSize: .zero)
    }
    required init?(coder: NSCoder) { fatalError("Not supported") }
    override func resizeSubviews(withOldSize oldSize: NSSize) {
        let left = bounds.width * 0.44
        control.frame = NSRect(x: 0, y: 0, width: left, height: bounds.height)
        telemetry.frame = NSRect(x: left, y: 0, width: bounds.width - left, height: bounds.height)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let command: String
    var process: Process?
    var input: FileHandle?
    var output: FileHandle?
    var outputBuffer = ""
    var view: ControlView!
    var window: NSWindow!
    var closing = false
    var signalSources: [DispatchSourceSignal] = []
    let telemetryCommand: [String]
    var telemetryProcess: Process?
    var telemetryOutput: FileHandle?
    var telemetryBuffer = Data()
    var telemetryView: TelemetryView?
    let connectivityCommand: [String]
    var connectivityProcess: Process?
    var connectivityAction = "status"
    var connectivityTarget: String?
    var connectivityReadAt: TimeInterval = 0

    init(command: String, telemetryCommand: [String] = [], connectivityCommand: [String] = []) {
        self.command = command
        self.telemetryCommand = telemetryCommand
        self.connectivityCommand = connectivityCommand
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        let combined = !telemetryCommand.isEmpty
        let content: NSView
        if combined {
            let workspace = DrivingWorkspace(frame: NSRect(x: 0, y: 0, width: 1040, height: 600))
            view = workspace.control
            telemetryView = workspace.telemetry
            content = workspace
        } else {
            view = ControlView(frame: NSRect(x: 0, y: 0, width: 520, height: 600))
            content = view
        }
        window = NSWindow(
            contentRect: content.frame,
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = combined ? "CARLA — Driving Control & Telemetry" : "CARLA — Live Driving Control"
        window.contentView = content
        content.autoresizingMask = [.width, .height]
        window.contentMinSize = combined ? NSSize(width: 900, height: 470) : NSSize(width: 360, height: 390)
        window.delegate = self
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            window.setFrameOrigin(NSPoint(x: frame.maxX - 550, y: frame.maxY - 640))
        }
        view.onControl = { [weak self] control in self?.send(control) }
        view.onMode = { [weak self] mode in self?.selectMode(mode) }
        view.onExit = { [weak self] in self?.finish() }
        if !connectivityCommand.isEmpty {
            view.onConnectivity = { [weak self] in self?.toggleConnectivity() }
        }
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(lostFocus),
            name: NSWindow.didResignKeyNotification,
            object: window
        )
        for number in [SIGINT, SIGTERM] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { [weak self] in self?.finish() }
            source.resume()
            signalSources.append(source)
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        startBridge()
        startTelemetry()
        if !connectivityCommand.isEmpty {
            requestConnectivity("status")
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
                self?.requestConnectivity("status")
            }
        }
        jsonLine("keyboard_ui_ready", fields: ["state": "safe_stop"])
    }

    func toggleConnectivity() {
        let fresh = ProcessInfo.processInfo.systemUptime - connectivityReadAt <= 15
        let action = !fresh ? "status" : view.externalState == "ON" ? "off" : view.externalState == "OFF" ? "on" : "status"
        requestConnectivity(action)
    }

    func requestConnectivity(_ action: String) {
        guard !closing, let executable = connectivityCommand.first else { return }
        if let running = connectivityProcess {
            guard action != "status", connectivityAction == "status" else { return }
            if running.isRunning { running.terminate() }
            connectivityProcess = nil
        }
        let task = Process()
        let pipe = Pipe()
        task.executableURL = URL(fileURLWithPath: executable)
        task.arguments = Array(connectivityCommand.dropFirst()) + [action]
        if action != "status" {
            guard let target = connectivityTarget, ["test", "production"].contains(target) else { return }
            task.arguments! += ["--target", target]
        }
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        task.standardInput = FileHandle.nullDevice
        connectivityProcess = task
        connectivityAction = action
        view.externalBusy = action != "status"
        do { try task.run() } catch {
            connectivityProcess = nil
            view.externalState = "UNKNOWN"
            view.externalBusy = false
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self, weak task] in
            guard let self = self, let task = task, self.connectivityProcess === task, task.isRunning else { return }
            task.terminate()
        }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            var data = Data()
            while let chunk = try? pipe.fileHandleForReading.read(upToCount: 8192), !chunk.isEmpty {
                if data.count + chunk.count <= 65536 { data.append(chunk) }
                else { if task.isRunning { task.terminate() }; data.removeAll(); break }
            }
            task.waitUntilExit()
            let result = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            DispatchQueue.main.async {
                guard let self = self, self.connectivityProcess === task else { return }
                self.connectivityProcess = nil
                self.view.externalBusy = false
                if task.terminationStatus == 0,
                   let facts = result?["data"] as? [String: Any],
                   let state = facts["state"] as? String, ["ON", "OFF"].contains(state),
                   let target = facts["target"] as? String, ["test", "production"].contains(target) {
                    self.view.externalState = state
                    self.connectivityTarget = target
                    self.connectivityReadAt = ProcessInfo.processInfo.systemUptime
                    self.view.externalReadAt = self.connectivityReadAt
                } else {
                    self.view.externalState = result?["message"] as? String == "EXTERNAL_LINK_CURRENT_VEHICLE_REQUIRED" ? "NO_VEHICLE" : "UNKNOWN"
                    self.connectivityTarget = nil
                }
                self.view.needsDisplay = true
                self.completeCloseIfReady()
            }
        }
    }

    func startTelemetry() {
        guard let executable = telemetryCommand.first else { return }
        let task = Process()
        let pipe = Pipe()
        task.executableURL = URL(fileURLWithPath: executable)
        task.arguments = Array(telemetryCommand.dropFirst())
        task.standardInput = FileHandle.nullDevice
        task.standardOutput = pipe
        // Data and unrestricted transport errors must not enter session logs.
        task.standardError = FileHandle.nullDevice
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            DispatchQueue.main.async { self?.consumeTelemetry(data) }
        }
        task.terminationHandler = { [weak self] task in
            DispatchQueue.main.async {
                guard let self = self, self.telemetryProcess === task else { return }
                self.telemetryOutput?.readabilityHandler = nil
                self.telemetryOutput = nil
                self.telemetryProcess = nil
                self.telemetryView?.disconnected = true
                self.telemetryView?.needsDisplay = true
                self.completeCloseIfReady()
            }
        }
        do {
            try task.run()
            telemetryProcess = task
            telemetryOutput = pipe.fileHandleForReading
        } catch {
            telemetryView?.disconnected = true
            jsonLine("telemetry_display_state", fields: ["state": "DISCONNECTED"])
        }
    }

    func consumeTelemetry(_ data: Data) {
        guard !closing else { return }
        telemetryBuffer.append(data)
        guard telemetryBuffer.count <= 131072 else {
            telemetryBuffer.removeAll()
            telemetryView?.disconnected = true
            return
        }
        while let newline = telemetryBuffer.firstIndex(of: 10) {
            let line = Data(telemetryBuffer[..<newline])
            telemetryBuffer.removeSubrange(...newline)
            guard line.count <= 65535,
                  let value = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
            telemetryView?.accept(value)
        }
    }

    func completeCloseIfReady() {
        if closing && process == nil && telemetryProcess == nil && connectivityProcess == nil {
            jsonLine("keyboard_ui_closed")
            NSApp.terminate(nil)
        }
    }

    func startBridge() {
        let task = Process()
        let inputPipe = Pipe()
        let outputPipe = Pipe()
        task.executableURL = URL(fileURLWithPath: "/bin/zsh")
        task.arguments = ["-lc", command]
        task.standardInput = inputPipe
        task.standardOutput = outputPipe
        task.standardError = FileHandle.standardError
        outputPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            DispatchQueue.main.async { self?.consumeBridgeOutput(text) }
        }
        task.terminationHandler = { [weak self] task in
            DispatchQueue.main.async {
                guard let self = self, self.process === task else { return }
                self.output?.readabilityHandler = nil
                self.process = nil
                self.input = nil
                self.output = nil
                if self.closing {
                    self.completeCloseIfReady()
                } else {
                    self.view.connectionLost()
                    jsonLine("keyboard_ui_paused", fields: ["reason": "connection_lost"])
                }
            }
        }
        do {
            try task.run()
            process = task
            input = inputPipe.fileHandleForWriting
            output = outputPipe.fileHandleForReading
        } catch {
            view.connectionLost()
            jsonLine("keyboard_ui_failed", fields: ["error": error.localizedDescription])
        }
    }

    func consumeBridgeOutput(_ text: String) {
        outputBuffer += text
        while let newline = outputBuffer.firstIndex(of: "\n") {
            let line = String(outputBuffer[..<newline])
            outputBuffer.removeSubrange(...newline)
            guard !line.isEmpty else { continue }
            print(line)
            fflush(stdout)
            guard
                let data = line.data(using: .utf8),
                let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                value["source"] as? String == "keyboard_control_bridge",
                let event = value["event"] as? String
            else { continue }
            if event == "bridge_ready" {
                if let modes = value["available_modes"] as? [String] {
                    view.setAvailableModes(modes)
                }
                view.setConnected()
            } else if event == "mode_changed", let mode = value["mode"] as? String {
                view.setMode(mode, reason: value["reason"] as? String ?? "")
            } else if event == "mode_rejected", let mode = value["mode"] as? String {
                view.rejectMode(mode)
            } else if event == "bridge_failed" {
                view.connectionLost()
            }
        }
    }

    @objc func lostFocus() {
        if !closing && view.mode == "manual" && !view.awaitingOperator { selectMode("safe_stop") }
    }

    func writePayload(_ payload: [String: Any]) {
        guard let input = input,
              let data = try? JSONSerialization.data(withJSONObject: payload) else { return }
        do {
            try input.write(contentsOf: data + Data([0x0a]))
        } catch {
            view.connectionLost()
        }
    }

    func selectMode(_ mode: String) {
        guard !closing else { return }
        if !view.connected {
            // Explicit operator recovery only. Reacquire stopped; never replay
            // the requested driving mode or buffered pedal commands.
            if process == nil {
                outputBuffer = ""
                view.statusDetail = "RECONNECTING — THEN SELECT A DRIVING MODE"
                view.needsDisplay = true
                startBridge()
            }
            return
        }
        view.requestingMode(mode)
        writePayload(["action": "set_mode", "mode": mode])
    }

    func send(_ control: Control) {
        guard view.mode == "manual",
              let data = try? JSONEncoder().encode(control),
              let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return }
        writePayload(value)
    }

    func finish() {
        if closing { return }
        closing = true
        view.requestingMode("safe_stop")
        writePayload(["action": "exit"])
        try? input?.close()
        input = nil
        if let telemetry = telemetryProcess, telemetry.isRunning { telemetry.terminate() }
        completeCloseIfReady()
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        finish()
        return false
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

guard (2...4).contains(CommandLine.arguments.count) else {
    fputs("usage: KeyboardControl <bridge-command> [telemetry-argv-json] [connectivity-argv-json]\n", stderr)
    exit(2)
}
var telemetryCommand: [String] = []
if CommandLine.arguments.count >= 3 {
    guard let data = CommandLine.arguments[2].data(using: .utf8),
          let arguments = try? JSONSerialization.jsonObject(with: data) as? [String],
          !arguments.isEmpty else { exit(2) }
    telemetryCommand = arguments
}
var connectivityCommand: [String] = []
if CommandLine.arguments.count == 4 {
    guard let data = CommandLine.arguments[3].data(using: .utf8),
          let arguments = try? JSONSerialization.jsonObject(with: data) as? [String],
          !arguments.isEmpty else { exit(2) }
    connectivityCommand = arguments
}
let application = NSApplication.shared
let delegate = AppDelegate(command: CommandLine.arguments[1], telemetryCommand: telemetryCommand, connectivityCommand: connectivityCommand)
application.delegate = delegate
application.run()
