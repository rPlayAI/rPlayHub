//
//  ConsolePanel.swift
//  The inspector's Console tab: the device's live syslog.
//
//  The engine's `syslog` method turns one control connection into a stream of
//  {"event":"syslog","line":...} objects, so this panel opens a connection of its own rather
//  than borrowing the app's: the shared client serializes calls on one socket, and a stream
//  that never ends would stall every tap behind it.
//

import AppKit

final class ConsolePanel: NSView {
    private let text = NSTextView()
    private let filter = NSSearchField()
    private let toggle = NSButton()
    private let clearButton = NSButton()
    private let status = NSTextField(labelWithString: "")
    private var client: ControlClient?
    private var running = false
    private var lines: [String] = []
    private var pending: [String] = []
    private var flushScheduled = false
    private let maxLines = 5000

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        text.isEditable = false
        text.isRichText = false
        text.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        text.textContainerInset = NSSize(width: 4, height: 4)
        text.isHorizontallyResizable = false
        text.autoresizingMask = [.width]
        text.textContainer?.widthTracksTextView = true
        let scroll = NSScrollView()
        scroll.documentView = text
        scroll.hasVerticalScroller = true
        scroll.borderType = .bezelBorder
        scroll.translatesAutoresizingMaskIntoConstraints = false

        toggle.bezelStyle = .rounded
        toggle.controlSize = .small
        toggle.target = self
        toggle.action = #selector(toggleStream)
        setToggle(running: false)

        clearButton.title = "Clear"
        clearButton.bezelStyle = .rounded
        clearButton.controlSize = .small
        clearButton.target = self
        clearButton.action = #selector(clear)

        filter.placeholderString = "Filter"
        filter.controlSize = .small
        filter.target = self
        filter.action = #selector(filterChanged)
        filter.sendsSearchStringImmediately = true

        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail

        let buttons = NSStackView(views: [toggle, clearButton, filter])
        buttons.orientation = .horizontal
        buttons.spacing = 6
        let stack = NSStackView(views: [buttons, scroll, status])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 10, right: 10)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            scroll.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -20),
            buttons.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            status.widthAnchor.constraint(equalTo: scroll.widthAnchor),
        ])
        // The inspector holds its 260-point width at priority 700; anything in here that resists
        // compression at the default 750 would win and grow the pane across the window, pushing
        // the screen out. Everything yields instead and truncates or scrolls.
        for v in [self, stack, buttons, status, scroll] + buttons.arrangedSubviews {
            v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            v.setContentHuggingPriority(.init(100), for: .horizontal)
        }
        status.stringValue = "Press Start to stream the device log."
    }

    private func setToggle(running: Bool) {
        toggle.title = running ? " Stop" : " Start"
        toggle.image = NSImage(systemSymbolName: running ? "stop.fill" : "play.fill",
                               accessibilityDescription: toggle.title)
        toggle.imagePosition = .imageLeading
    }

    @objc private func toggleStream() {
        running ? stop() : start()
    }

    func start() {
        guard !running else { return }
        let c = ControlClient()
        do { try c.connect() } catch {
            status.stringValue = "Engine not reachable: \(error)"
            return
        }
        client = c
        running = true
        setToggle(running: true)
        status.stringValue = "Streaming…"
        c.stream("syslog", onEvent: { [weak self] ev in
            guard let line = ev["line"] as? String else { return }
            self?.enqueue(line)
        }, onEnd: { [weak self] error in
            guard let self else { return }
            self.running = false
            self.setToggle(running: false)
            self.client = nil
            self.status.stringValue = error.map { "Stream ended: \($0)" } ?? "Stopped."
        })
    }

    func stop() {
        guard running else { return }
        client?.close()        // the engine ends the relay when our side goes away
        client = nil
        running = false
        setToggle(running: false)
        status.stringValue = "Stopped."
    }

    /// Lines arrive in bursts of hundreds a second; append once per run-loop turn, not per line.
    private func enqueue(_ line: String) {
        pending.append(line)
        if !flushScheduled {
            flushScheduled = true
            DispatchQueue.main.async { self.flush() }
        }
    }

    private func flush() {
        flushScheduled = false
        let batch = pending
        pending.removeAll()
        lines.append(contentsOf: batch)
        if lines.count > maxLines { lines.removeFirst(lines.count - maxLines) }
        let needle = filter.stringValue
        let shown = needle.isEmpty ? batch
                  : batch.filter { $0.localizedCaseInsensitiveContains(needle) }
        guard !shown.isEmpty else { return }
        let atBottom = text.visibleRect.maxY >= text.bounds.maxY - 20
        text.textStorage?.append(NSAttributedString(
            string: shown.joined(separator: "\n") + "\n",
            attributes: [.font: text.font!, .foregroundColor: NSColor.textColor]))
        if let s = text.textStorage, s.length > maxLines * 200 {
            s.deleteCharacters(in: NSRange(location: 0, length: s.length - maxLines * 150))
        }
        if atBottom { text.scrollToEndOfDocument(nil) }
        status.stringValue = "\(lines.count) lines" + (running ? " · streaming" : "")
    }

    @objc private func filterChanged() {
        let needle = filter.stringValue
        let shown = needle.isEmpty ? lines : lines.filter { $0.localizedCaseInsensitiveContains(needle) }
        text.string = shown.joined(separator: "\n") + (shown.isEmpty ? "" : "\n")
        text.scrollToEndOfDocument(nil)
    }

    @objc private func clear() {
        lines.removeAll()
        text.string = ""
        status.stringValue = running ? "Streaming…" : "Cleared."
    }
}
