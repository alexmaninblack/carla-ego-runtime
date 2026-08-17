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
    var mode = "safe_stop"
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
            if mode == "manual" { pressed.insert(event.keyCode) }
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
        pressed.removeAll()
        if selected == "manual" {
            statusDetail = "MANUAL CONTROL — ARROWS ACTIVE"
            throttle = 0
            brake = 0
            steering = 0
        } else if selected == "autopilot" {
            statusDetail = "AUTOPILOT — VEHICLE DRIVING"
            throttle = 0
            brake = 0
            steering = 0
        } else if selected == "scenario" {
            statusDetail = "SCRIPTED BRAKE SCENARIO — RUNNING"
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
        if mode == "manual" && !(window?.isKeyWindow ?? false) {
            onMode?("safe_stop")
            return
        }
        if mode == "manual" {
            let braking = pressed.contains(125)
            let throttleTarget = pressed.contains(126) && !braking ? 0.55 : 0.0
            let brakeTarget = braking ? 0.75 : 0.0
            throttle = braking ? 0 : approach(throttle, throttleTarget, 1.25 * elapsed)
            brake = approach(brake, brakeTarget, 3.0 * elapsed)
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
                ? NSColor(calibratedWhite: 0.995, alpha: 1)
                : NSColor(calibratedWhite: 0.92, alpha: 1),
            border: enabled
                ? NSColor(calibratedWhite: 0.62, alpha: 1)
                : NSColor(calibratedWhite: 0.82, alpha: 1),
            lineWidth: enabled ? 1.5 : 1
        )
        centeredText(
            title,
            in: rect,
            size: 13,
            color: enabled ? .labelColor : .tertiaryLabelColor,
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
        roundedCard(rect, fill: fill, border: border, lineWidth: selected ? 3 : 1.5)
        centeredText(title, in: rect, size: 12, color: textColor, bold: true)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(calibratedWhite: 0.96, alpha: 1).setFill()
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
        roundedCard(statusRect, fill: statusFill, border: statusBorder, lineWidth: 1.5)
        centeredText(statusDetail, in: statusRect, size: 15, color: statusBorder, bold: true)

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
                color: .secondaryLabelColor,
                bold: true,
                alignment: .center
            )
        } else if mode == "scenario" {
            text(
                "VEHICLE CONTROLLED BY THE BRAKE-EVENT STATE MACHINE",
                x: 44,
                y: 270,
                size: 12,
                color: .secondaryLabelColor,
                bold: true,
                alignment: .center
            )
        } else {
            text("THROTTLE", x: 46, y: 302, size: 12, color: .secondaryLabelColor)
            text(
                String(format: "%.2f", throttle),
                x: 44,
                y: 302,
                size: 13,
                color: .labelColor,
                bold: true,
                alignment: .right
            )
            text("BRAKE", x: 46, y: 274, size: 12, color: .secondaryLabelColor)
            text(
                String(format: "%.2f", brake),
                x: 44,
                y: 274,
                size: 13,
                color: .labelColor,
                bold: true,
                alignment: .right
            )
            text("STEERING", x: 46, y: 246, size: 12, color: .secondaryLabelColor)
            text(
                String(format: "%+.2f", steering),
                x: 44,
                y: 246,
                size: 13,
                color: .labelColor,
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
            color: .secondaryLabelColor,
            alignment: .center
        )
        text(
            "Focus loss stops manual control; scripted and autopilot modes continue.",
            x: 44,
            y: 187,
            size: 10,
            color: .secondaryLabelColor,
            alignment: .center
        )

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

    init(command: String) { self.command = command }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        view = ControlView(frame: NSRect(x: 0, y: 0, width: 520, height: 600))
        window = NSWindow(
            contentRect: view.bounds,
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "CARLA — Live Driving Control"
        window.contentView = view
        window.delegate = self
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            window.setFrameOrigin(NSPoint(x: frame.maxX - 550, y: frame.maxY - 640))
        }
        view.onControl = { [weak self] control in self?.send(control) }
        view.onMode = { [weak self] mode in self?.selectMode(mode) }
        view.onExit = { [weak self] in self?.finish() }
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
        jsonLine("keyboard_ui_ready", fields: ["state": "safe_stop"])
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
                    jsonLine("keyboard_ui_closed")
                    NSApp.terminate(nil)
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
        if !closing && view.mode == "manual" { selectMode("safe_stop") }
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
        guard !closing, view.connected else { return }
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
        if process == nil {
            jsonLine("keyboard_ui_closed")
            NSApp.terminate(nil)
        }
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        finish()
        return false
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

guard CommandLine.arguments.count == 2 else {
    fputs("usage: KeyboardControl <bridge-command>\n", stderr)
    exit(2)
}
let application = NSApplication.shared
let delegate = AppDelegate(command: CommandLine.arguments[1])
application.delegate = delegate
application.run()
