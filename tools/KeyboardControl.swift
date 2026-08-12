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
    var active = false
    var reason = "PRESS ENTER TO ARM"
    var throttle = 0.0
    var brake = 1.0
    var steering = 0.0
    var pressed = Set<UInt16>()
    var onControl: ((Control) -> Void)?
    var onArm: (() -> Void)?
    var onPause: ((String) -> Void)?
    var onExit: (() -> Void)?
    private var lastUpdate = ProcessInfo.processInfo.systemUptime
    private let armButtonRect = NSRect(x: 24, y: 22, width: 210, height: 48)
    private let stopButtonRect = NSRect(x: 246, y: 22, width: 210, height: 48)
    private let throttleKeyRect = NSRect(x: 170, y: 356, width: 140, height: 44)
    private let steerLeftKeyRect = NSRect(x: 20, y: 306, width: 140, height: 44)
    private let steerRightKeyRect = NSRect(x: 320, y: 306, width: 140, height: 44)
    private let brakeKeyRect = NSRect(x: 170, y: 256, width: 140, height: 44)

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
        case 36, 76: onArm?()
        case 49: onPause?("emergency_stop")
        case 53: onExit?()
        case 123, 124, 125, 126: pressed.insert(event.keyCode)
        default: super.keyDown(with: event)
        }
    }

    override func keyUp(with event: NSEvent) {
        pressed.remove(event.keyCode)
    }

    func arm() {
        active = true
        reason = "DRIVING — CONTROL ACTIVE"
        throttle = 0
        brake = 0
        steering = 0
        pressed.removeAll()
        needsDisplay = true
    }

    func pause(_ why: String) {
        active = false
        reason = why.replacingOccurrences(of: "_", with: " ").uppercased()
        throttle = 0
        brake = 1
        steering = 0
        pressed.removeAll()
        onControl?(Control(throttle: 0, brake: 1, steering: 0))
        needsDisplay = true
    }

    private func approach(_ current: Double, _ target: Double, _ delta: Double) -> Double {
        target > current ? min(target, current + delta) : max(target, current - delta)
    }

    private func tick() {
        let now = ProcessInfo.processInfo.systemUptime
        let elapsed = max(0, min(now - lastUpdate, 0.25))
        lastUpdate = now
        if active && !(window?.isKeyWindow ?? false) {
            onPause?("focus_lost")
            return
        }
        if active {
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
            steering = approach(steering, steeringTarget, (steeringTarget == 0 ? 2.8 : 1.6) * elapsed)
            onControl?(Control(throttle: throttle, brake: brake, steering: steering))
        }
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if armButtonRect.contains(point) {
            onArm?()
        } else if stopButtonRect.contains(point) {
            onPause?("operator_stop")
        }
    }

    private func text(_ value: String, x: CGFloat, y: CGFloat, size: CGFloat,
                      color: NSColor, bold: Bool = false,
                      alignment: NSTextAlignment = .left, width: CGFloat = 396) {
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = alignment
        let font = bold ? NSFont.boldSystemFont(ofSize: size) : NSFont.systemFont(ofSize: size)
        value.draw(in: NSRect(x: x, y: y, width: width, height: 50), withAttributes: [
            .font: font, .foregroundColor: color, .paragraphStyle: paragraph
        ])
    }

    private func centeredText(_ value: String, in rect: NSRect, size: CGFloat,
                              color: NSColor, bold: Bool = false) {
        let font = bold ? NSFont.boldSystemFont(ofSize: size) : NSFont.systemFont(ofSize: size)
        let attributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: color,
        ]
        let measured = value.size(withAttributes: attributes)
        value.draw(at: NSPoint(x: rect.midX - measured.width / 2,
                               y: rect.midY - measured.height / 2),
                   withAttributes: attributes)
    }

    private func roundedCard(_ rect: NSRect, fill: NSColor, border: NSColor,
                             lineWidth: CGFloat = 1) {
        let path = NSBezierPath(roundedRect: rect, xRadius: 8, yRadius: 8)
        fill.setFill()
        path.fill()
        border.setStroke()
        path.lineWidth = lineWidth
        path.stroke()
    }

    private func keycap(_ title: String, rect: NSRect) {
        roundedCard(rect,
                    fill: NSColor(calibratedWhite: 0.995, alpha: 1),
                    border: NSColor(calibratedWhite: 0.74, alpha: 1),
                    lineWidth: 1.25)
        centeredText(title, in: rect, size: 13, color: .labelColor, bold: true)
    }

    private func actionButton(_ title: String, rect: NSRect, fill: NSColor,
                              border: NSColor, textColor: NSColor) {
        roundedCard(rect, fill: fill, border: border, lineWidth: 1.5)
        centeredText(title, in: rect, size: 13, color: textColor, bold: true)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(calibratedWhite: 0.96, alpha: 1).setFill()
        bounds.fill()
        let statusRect = NSRect(x: 24, y: 426, width: 432, height: 52)
        let statusFill = active
            ? NSColor(calibratedRed: 0.82, green: 0.93, blue: 0.85, alpha: 1)
            : NSColor(calibratedRed: 0.96, green: 0.84, blue: 0.84, alpha: 1)
        let statusBorder = active
            ? NSColor(calibratedRed: 0.30, green: 0.65, blue: 0.38, alpha: 1)
            : NSColor(calibratedRed: 0.82, green: 0.34, blue: 0.34, alpha: 1)
        roundedCard(statusRect, fill: statusFill, border: statusBorder, lineWidth: 1.5)
        centeredText(active ? reason : "SAFE STOP — \(reason)", in: statusRect, size: 16,
                     color: statusBorder, bold: true)

        keycap("↑  THROTTLE", rect: throttleKeyRect)
        keycap("←  STEER LEFT", rect: steerLeftKeyRect)
        keycap("STEER RIGHT  →", rect: steerRightKeyRect)
        keycap("↓  BRAKE", rect: brakeKeyRect)

        text("THROTTLE", x: 46, y: 202, size: 12, color: .secondaryLabelColor)
        text(String(format: "%.2f", throttle), x: 44, y: 202, size: 13, color: .labelColor,
             bold: true, alignment: .right)
        text("BRAKE", x: 46, y: 174, size: 12, color: .secondaryLabelColor)
        text(String(format: "%.2f", brake), x: 44, y: 174, size: 13, color: .labelColor,
             bold: true, alignment: .right)
        text("STEERING", x: 46, y: 146, size: 12, color: .secondaryLabelColor)
        text(String(format: "%+.2f", steering), x: 44, y: 146, size: 13, color: .labelColor,
             bold: true, alignment: .right)
        text("ENTER: ARM / RESUME    SPACE: SAFE STOP    ESC: EXIT", x: 42, y: 105,
             size: 10, color: .secondaryLabelColor, alignment: .center)
        text("Losing focus always releases control and brakes.", x: 42, y: 87,
             size: 10, color: .secondaryLabelColor, alignment: .center)
        actionButton("ARM / RESUME", rect: armButtonRect,
                     fill: NSColor(calibratedRed: 0.78, green: 0.91, blue: 0.81, alpha: 1),
                     border: NSColor(calibratedRed: 0.26, green: 0.62, blue: 0.34, alpha: 1),
                     textColor: NSColor(calibratedRed: 0.10, green: 0.42, blue: 0.18, alpha: 1))
        actionButton("SAFE STOP", rect: stopButtonRect,
                     fill: NSColor(calibratedRed: 0.95, green: 0.80, blue: 0.80, alpha: 1),
                     border: NSColor(calibratedRed: 0.78, green: 0.27, blue: 0.27, alpha: 1),
                     textColor: NSColor(calibratedRed: 0.65, green: 0.10, blue: 0.10, alpha: 1))
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let command: String
    var process: Process?
    var input: FileHandle?
    var view: ControlView!
    var window: NSWindow!
    var closing = false
    var signalSources: [DispatchSourceSignal] = []

    init(command: String) { self.command = command }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        view = ControlView(frame: NSRect(x: 0, y: 0, width: 480, height: 500))
        window = NSWindow(contentRect: view.bounds,
                          styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "CARLA M6.1 — Keyboard Control"
        window.contentView = view
        window.delegate = self
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            window.setFrameOrigin(NSPoint(x: frame.maxX - 510, y: frame.maxY - 540))
        }
        view.onControl = { [weak self] control in self?.send(control) }
        view.onArm = { [weak self] in self?.arm() }
        view.onPause = { [weak self] reason in self?.pause(reason) }
        view.onExit = { [weak self] in self?.finish() }
        NotificationCenter.default.addObserver(self, selector: #selector(lostFocus),
                                               name: NSWindow.didResignKeyNotification,
                                               object: window)
        for number in [SIGINT, SIGTERM] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { [weak self] in self?.finish() }
            source.resume()
            signalSources.append(source)
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        jsonLine("keyboard_ui_ready", fields: ["state": "safe_stop"])
    }

    @objc func lostFocus() {
        if !closing { pause("focus_lost") }
    }

    func arm() {
        guard process == nil else { return }
        let task = Process()
        let pipe = Pipe()
        task.executableURL = URL(fileURLWithPath: "/bin/zsh")
        task.arguments = ["-lc", command]
        task.standardInput = pipe
        task.standardOutput = FileHandle.standardOutput
        task.standardError = FileHandle.standardError
        task.terminationHandler = { [weak self] task in
            DispatchQueue.main.async {
                guard let self = self, self.process === task else { return }
                self.process = nil
                self.input = nil
                if !self.closing && self.view.active {
                    self.view.pause("connection_lost")
                }
            }
        }
        do {
            try task.run()
            process = task
            input = pipe.fileHandleForWriting
            view.arm()
            jsonLine("keyboard_ui_armed")
        } catch {
            view.pause("connection_failed")
            jsonLine("keyboard_ui_failed", fields: ["error": error.localizedDescription])
        }
    }

    func send(_ control: Control) {
        guard let input = input,
              let data = try? JSONEncoder().encode(control) else { return }
        do {
            try input.write(contentsOf: data + Data([0x0a]))
        } catch {
            pause("connection_lost")
        }
    }

    func pause(_ reason: String) {
        view.pause(reason)
        if let input = input {
            try? input.write(contentsOf: Data("{\"action\":\"stop\"}\n".utf8))
            try? input.close()
        }
        input = nil
        jsonLine("keyboard_ui_paused", fields: ["reason": reason])
    }

    func finish() {
        if closing { return }
        closing = true
        pause("window_closed")
        jsonLine("keyboard_ui_closed")
        NSApp.terminate(nil)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool { finish(); return false }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

guard CommandLine.arguments.count == 2 else {
    fputs("usage: KeyboardControl <bridge-command>\n", stderr)
    exit(2)
}
let application = NSApplication.shared
let delegate = AppDelegate(command: CommandLine.arguments[1])
application.delegate = delegate
application.run()
