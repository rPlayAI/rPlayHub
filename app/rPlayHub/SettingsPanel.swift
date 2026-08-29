//
//  SettingsPanel.swift
//  Device Hub's Settings tab: the device's own appearance and accessibility switches.
//
//  These are real settings on the phone, not view options -- flipping Reduce Motion here changes
//  it in iOS Settings. They ride CoreDevice actions on `com.apple.coredevice.configuration`
//  (engine methods `get_settings` / `set_setting`); doc/COREDEVICE-ACTIONS.md has the protocol,
//  the per-row payload shapes and how they were established.
//
//  Layout follows Device Hub's, checked against its live window: rounded grouped sections like
//  iOS Settings, each row an icon + label with its control right-aligned, and Location alone in
//  a second group below.
//

import AppKit

final class SettingsPanel: NSView {
    /// The engine connection. Setting it loads the current values.
    var control: ControlClient? {
        didSet { if control != nil { reload() } }
    }

    /// Row identifiers, matching the engine's `key` exactly -- they are the API, so they are
    /// written once here rather than duplicated per control.
    private enum Key: String {
        case appearance, liquidGlass, colorFilter, textSize
        case reduceMotion, increaseContrast, showBorders, reduceTransparency, voiceOver
    }

    private var switches: [Key: NSSwitch] = [:]
    private var popups: [Key: NSPopUpButton] = [:]
    private var sliders: [Key: NSSlider] = [:]
    private let locationPopup = NSPopUpButton()
    private let status = NSTextField(labelWithString: "")
    private let stack = NSStackView()

    /// True while reload() is populating controls, so programmatic changes don't echo back to
    /// the device as writes. Without it, loading the current values immediately re-sends every
    /// one of them.
    private var loading = false

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 14
        stack.edgeInsets = NSEdgeInsets(top: 10, left: 10, bottom: 10, right: 10)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
        ])

        // Group one: appearance and accessibility, in Device Hub's own order.
        //
        // The icons were originally my own plausible-looking guesses and several were wrong.
        // These are identified against Device Hub's: each row's glyph cropped out of its live
        // window at 5x and compared with rendered SF Symbol candidates -- the same method that
        // settled the View Screen icon. Confirmed unchanged: Appearance, Text Size, Increase
        // Contrast, Location. Corrected: Liquid Glass, Reduce Motion (a dotted circle overlapping
        // a solid one, which is a symbol in its own right), Show Borders (a dashed square BEHIND
        // a solid one), VoiceOver (the dedicated `voiceover` symbol, not a speaker).
        let main = GroupBox()
        main.addRow(icon: "circle.lefthalf.filled", title: "Appearance",
                    control: popup(.appearance, ["Light", "Dark"]))
        main.addRow(icon: "square.on.square", title: "Liquid Glass",
                    control: slider(.liquidGlass, min: 0, max: 1))
        main.addRow(icon: "camera.filters", title: "Color Filter",
                    control: popup(.colorFilter, ["None", "On"]))
        main.addRow(icon: "textformat.size", title: "Text Size",
                    control: slider(.textSize, min: 0, max: Double(Self.textSizes.count - 1)))
        main.addRow(icon: "circle.dotted.and.circle", title: "Reduce Motion",
                    control: toggle(.reduceMotion))
        main.addRow(icon: "circle.righthalf.filled", title: "Increase Contrast",
                    control: toggle(.increaseContrast))
        main.addRow(icon: "square.on.square.dashed", title: "Show Borders",
                    control: toggle(.showBorders))
        main.addRow(icon: "square.on.square.squareshape.controlhandles", title: "Reduce Transparency",
                    control: toggle(.reduceTransparency))
        main.addRow(icon: "voiceover", title: "VoiceOver",
                    control: toggle(.voiceOver))
        stack.addArrangedSubview(main)
        main.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -20).isActive = true

        // Location sits in its own group in Device Hub, with a dropdown. The menu below is its
        // real one, read off the live app by opening it and walking to the bottom: None, a
        // separator, fourteen cities, a "Trips" section header, four trips, then Custom
        // Coordinates. Same order, same wording.
        let locationGroup = GroupBox()
        buildLocationMenu()
        locationPopup.bezelStyle = .accessoryBarAction
        locationPopup.controlSize = .small
        locationPopup.target = self
        locationPopup.action = #selector(locationChanged(_:))
        // An NSPopUpButton sizes itself to its widest menu item, so "Johannesburg, South Africa"
        // stretched this one across the row. Device Hub's stays compact and lets the title
        // truncate, so pin the width and do the same.
        locationPopup.widthAnchor.constraint(equalToConstant: 96).isActive = true
        locationPopup.cell?.lineBreakMode = .byTruncatingTail
        locationGroup.addRow(icon: "location.circle", title: "Location", control: locationPopup)
        stack.addArrangedSubview(locationGroup)
        locationGroup.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -20).isActive = true

        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.preferredMaxLayoutWidth = 224      // a wrapping label needs this, not a width
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3
        stack.addArrangedSubview(status)
    }

    // MARK: - location

    /// Device Hub's own Location menu, in its order and wording.
    ///
    /// The cities are plain coordinate pairs: the capture caught Device Hub sending
    /// `setsimulatedlocation {latitude: 37.3348, longitude: -122.009}` — Apple Park, which is its
    /// own Cupertino entry, so that one is confirmed on the wire and the rest follow the same
    /// shape. Only Cupertino's numbers come from Device Hub itself; the others are the standard
    /// coordinates for those cities and may differ in the last decimal from whatever it sends.
    private static let cities: [(String, Double, Double)] = [
        ("Berlin, Germany", 52.5200, 13.4050),
        ("Cupertino, CA, USA", 37.3348, -122.0090),
        ("Hong Kong, China", 22.3193, 114.1694),
        ("Johannesburg, South Africa", -26.2041, 28.0473),
        ("London, England", 51.5074, -0.1278),
        ("Mexico City, Mexico", 19.4326, -99.1332),
        ("Mumbai, India", 19.0760, 72.8777),
        ("New York, NY, USA", 40.7128, -74.0060),
        ("Paris, France", 48.8566, 2.3522),
        ("Rio de Janeiro, Brazil", -22.9068, -43.1729),
        ("San Francisco, CA, USA", 37.7749, -122.4194),
        ("Sydney, Australia", -33.8688, 151.2093),
        ("Tokyo, Japan", 35.6762, 139.6503),
        ("Warsaw, Poland", 52.2297, 21.0122),
    ]

    /// Trips are routes, not points, so they need `setlocationscenario` with an identifier the
    /// device supplies through `availablelocationscenarios` — neither of which is wired yet.
    /// They are shown, to match Device Hub's menu, but disabled: offering them as if they worked
    /// would be worse than showing they are not ready.
    private static let trips = ["City Run", "City Bicycle Ride", "Apple", "Freeway Drive"]

    private func buildLocationMenu() {
        let menu = NSMenu()
        // Items carry no action of their own (the popup's own action fires), and with
        // autoenablesItems on AppKit greys out anything actionless -- which greyed Custom
        // Coordinates. Turning it off makes the isEnabled set below the only thing that decides.
        menu.autoenablesItems = false
        menu.addItem(withTitle: "None", action: nil, keyEquivalent: "")
        menu.addItem(.separator())
        for (name, _, _) in Self.cities {
            menu.addItem(withTitle: name, action: nil, keyEquivalent: "")
        }
        let header = NSMenuItem(title: "Trips", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)
        for t in Self.trips {
            let item = NSMenuItem(title: t, action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
        }
        menu.addItem(.separator())
        menu.addItem(withTitle: "Custom Coordinates…", action: nil, keyEquivalent: "")
        locationPopup.menu = menu
        locationPopup.selectItem(at: 0)
    }

    @objc private func locationChanged(_ sender: NSPopUpButton) {
        guard !loading, let control else { return }
        let title = sender.titleOfSelectedItem ?? "None"
        if title == "None" {
            control.send("clear_location") { [weak self] r in self?.report(r, "Location") }
        } else if title == "Custom Coordinates…" {
            askForCoordinates()
        } else if let city = Self.cities.first(where: { $0.0 == title }) {
            send(latitude: city.1, longitude: city.2)
        }
    }

    private func send(latitude: Double, longitude: Double) {
        control?.send("set_location", ["latitude": latitude, "longitude": longitude]) {
            [weak self] r in self?.report(r, "Location")
        }
    }

    /// Device Hub's Custom Coordinates sheet, checked by opening its own: titled "Custom
    /// Coordinates", two EMPTY fields labelled Latitude and Longitude, and Cancel / Apply with
    /// Apply disabled until both are filled. It does not pre-fill the current location — an
    /// earlier version of this filled in Cupertino, which matched neither Device Hub nor any
    /// sensible default.
    private func askForCoordinates() {
        let alert = NSAlert()
        alert.messageText = "Custom Coordinates"
        let sheet = CoordinateFields()
        alert.accessoryView = sheet.view
        alert.addButton(withTitle: "Apply")       // first added is the rightmost, default button
        alert.addButton(withTitle: "Cancel")
        sheet.apply = alert.buttons.first
        sheet.apply?.isEnabled = false            // nothing typed yet, as in Device Hub
        let choice = alert.runModal()
        guard choice == .alertFirstButtonReturn,
              let la = Double(sheet.lat.stringValue), let lo = Double(sheet.lon.stringValue) else {
            // Cancelled, so the menu must go back to what the device is actually on rather than
            // sitting on "Custom Coordinates…" as though something had been applied.
            locationPopup.selectItem(at: 0)
            return
        }
        send(latitude: la, longitude: lo)
    }

    private func report(_ result: Result<[String: Any], Error>, _ what: String) {
        if case .failure(let e) = result {
            status.stringValue = "\(what) was not accepted: \(e)"
        } else {
            status.stringValue = ""
        }
    }

    // MARK: - control factories

    private func toggle(_ key: Key) -> NSSwitch {
        let s = NSSwitch()
        // Device Hub's switches measure 36x18pt; an unmodified NSSwitch is 54x21, half again as
        // wide, and stood out badly next to the rest of the row. .mini is the closest stock size.
        s.controlSize = .mini
        s.target = self
        s.action = #selector(switchChanged(_:))
        s.identifier = NSUserInterfaceItemIdentifier(key.rawValue)
        switches[key] = s
        return s
    }

    private func popup(_ key: Key, _ titles: [String]) -> NSPopUpButton {
        let p = NSPopUpButton()
        p.addItems(withTitles: titles)
        p.bezelStyle = .accessoryBarAction
        p.controlSize = .small
        p.target = self
        p.action = #selector(popupChanged(_:))
        p.identifier = NSUserInterfaceItemIdentifier(key.rawValue)
        popups[key] = p
        return p
    }

    private func slider(_ key: Key, min lo: Double, max hi: Double) -> NSSlider {
        let s = NSSlider(value: lo, minValue: lo, maxValue: hi,
                         target: self, action: #selector(sliderChanged(_:)))
        s.identifier = NSUserInterfaceItemIdentifier(key.rawValue)
        s.isContinuous = false          // one write per gesture, not one per pixel
        if key == .textSize {
            s.numberOfTickMarks = Self.textSizes.count
            s.allowsTickMarkValuesOnly = true
        }
        s.widthAnchor.constraint(equalToConstant: 110).isActive = true
        sliders[key] = s
        return s
    }

    /// The device reports Text Size as an enum case name, not a number (see
    /// doc/COREDEVICE-ACTIONS.md), so the slider maps position <-> case. This is the order iOS
    /// presents them in; only `large` has been seen on the wire, so an unknown name is shown
    /// rather than guessed at.
    private static let textSizes = ["extraSmall", "small", "medium", "large",
                                    "extraLarge", "extraExtraLarge", "extraExtraExtraLarge"]

    // MARK: - reading

    func reload() {
        guard let control else { return }
        control.send("get_settings") { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let info):
                self.loading = true
                self.apply(info)
                self.loading = false
                self.status.stringValue = ""
            case .failure(let e):
                self.status.stringValue = "Could not read the device's settings: \(e)"
            }
        }
    }

    /// The three nesting conventions are the device's, not ours -- most rows wrap their value in
    /// a named key, appearance is flat, and textSize is an enum encoded as a single-key
    /// dictionary. Reading them apart here keeps that oddity in one place.
    private func apply(_ info: [String: Any]) {
        func enabled(_ key: String, _ wrap: String) -> Bool? {
            guard let outer = info[key] as? [String: Any],
                  let inner = outer[wrap] as? [String: Any] else { return nil }
            return inner["enabled"] as? Bool
        }
        let bools: [(Key, String)] = [
            (.reduceMotion, "reduceMotion"), (.increaseContrast, "increaseContrast"),
            (.showBorders, "showBorders"), (.reduceTransparency, "reduceTransparency"),
            (.voiceOver, "voiceOverConfiguration"), (.colorFilter, "colorFilter"),
        ]
        for (key, wrap) in bools {
            guard let on = enabled(key.rawValue, wrap) else { continue }
            if let sw = switches[key] { sw.state = on ? .on : .off }
            if let pop = popups[key] { pop.selectItem(at: on ? 1 : 0) }
        }
        if let outer = info["appearance"] as? [String: Any],
           let style = outer["style"] as? String {
            popups[.appearance]?.selectItem(at: style.lowercased() == "dark" ? 1 : 0)
        }
        if let outer = info["liquidGlass"] as? [String: Any],
           let cfg = outer["configuration"] as? [String: Any],
           let opacity = cfg["opacity"] as? Double {
            sliders[.liquidGlass]?.doubleValue = opacity
        }
        if let outer = info["textSize"] as? [String: Any],
           let ts = outer["textSize"] as? [String: Any],
           let size = ts["size"] as? [String: Any],
           let name = size.keys.first {
            if let idx = Self.textSizes.firstIndex(of: name) {
                sliders[.textSize]?.doubleValue = Double(idx)
            } else {
                status.stringValue = "Unknown text size “\(name)”"
            }
        }
    }

    // MARK: - writing

    @objc private func switchChanged(_ sender: NSSwitch) {
        guard let key = key(of: sender) else { return }
        write(key, value: sender.state == .on)
    }

    @objc private func popupChanged(_ sender: NSPopUpButton) {
        guard let key = key(of: sender) else { return }
        if key == .appearance {
            write(key, value: sender.indexOfSelectedItem == 1 ? "dark" : "light")
        } else {
            write(key, value: sender.indexOfSelectedItem == 1)
        }
    }

    @objc private func sliderChanged(_ sender: NSSlider) {
        guard let key = key(of: sender) else { return }
        if key == .textSize {
            let idx = min(max(Int(sender.doubleValue.rounded()), 0), Self.textSizes.count - 1)
            write(key, value: Self.textSizes[idx])
        } else {
            write(key, value: sender.doubleValue)
        }
    }

    private func key(of view: NSView) -> Key? {
        guard let raw = view.identifier?.rawValue else { return nil }
        return Key(rawValue: raw)
    }

    private func write(_ key: Key, value: Any) {
        guard !loading, let control else { return }
        status.stringValue = ""
        control.send("set_setting", ["key": key.rawValue, "value": value]) { [weak self] result in
            guard let self, case .failure(let e) = result else { return }
            // Say so and re-read: the control is now showing something the device did not
            // accept, and silently leaving it there would misreport the phone's actual state.
            self.status.stringValue = "\(key.rawValue) was not accepted: \(e)"
            self.reload()
        }
    }
}

/// One rounded section, as Device Hub groups these rows. Rows are separated by a hairline that
/// starts after the icon, matching the inset separators in the real app.
private final class GroupBox: NSView {
    private let rows = NSStackView()

    init() {
        super.init(frame: .zero)
        wantsLayer = true
        // #F3F3F2 -- sampled off Device Hub's own grouped rows. Slightly DARKER than the pane
        // behind it (#FAFAFA), which is the iOS-settings look; a lighter-than-the-pane box read
        // as a floating card instead of a grouped list.
        layer?.backgroundColor = NSColor(srgbRed: 0xF3 / 255, green: 0xF3 / 255,
                                         blue: 0xF2 / 255, alpha: 1).cgColor
        layer?.cornerRadius = 8
        rows.orientation = .vertical
        rows.alignment = .leading
        rows.spacing = 0
        rows.translatesAutoresizingMaskIntoConstraints = false
        addSubview(rows)
        NSLayoutConstraint.activate([
            rows.topAnchor.constraint(equalTo: topAnchor),
            rows.bottomAnchor.constraint(equalTo: bottomAnchor),
            rows.leadingAnchor.constraint(equalTo: leadingAnchor),
            rows.trailingAnchor.constraint(equalTo: trailingAnchor),
        ])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func addRow(icon: String, title: String, control: NSView) {
        if !rows.arrangedSubviews.isEmpty {
            let line = NSBox()
            line.boxType = .separator
            line.translatesAutoresizingMaskIntoConstraints = false
            rows.addArrangedSubview(line)
            line.leadingAnchor.constraint(equalTo: rows.leadingAnchor, constant: 32).isActive = true
            line.trailingAnchor.constraint(equalTo: rows.trailingAnchor).isActive = true
        }
        let row = Row(icon: icon, title: title, control: control)
        rows.addArrangedSubview(row)
        row.leadingAnchor.constraint(equalTo: rows.leadingAnchor).isActive = true
        row.trailingAnchor.constraint(equalTo: rows.trailingAnchor).isActive = true
    }
}

private final class Row: NSView {
    init(icon: String, title: String, control: NSView) {
        super.init(frame: .zero)
        let image = NSImageView()
        image.image = NSImage(systemSymbolName: icon, accessibilityDescription: title)
        image.contentTintColor = .secondaryLabelColor
        image.translatesAutoresizingMaskIntoConstraints = false

        let label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 12)
        label.translatesAutoresizingMaskIntoConstraints = false
        label.setContentCompressionResistancePriority(.init(200), for: .horizontal)

        control.translatesAutoresizingMaskIntoConstraints = false
        for v in [image, label, control] { addSubview(v) }

        NSLayoutConstraint.activate([
            heightAnchor.constraint(equalToConstant: 32),
            image.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            image.centerYAnchor.constraint(equalTo: centerYAnchor),
            image.widthAnchor.constraint(equalToConstant: 16),
            label.leadingAnchor.constraint(equalTo: image.trailingAnchor, constant: 8),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
            control.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            control.centerYAnchor.constraint(equalTo: centerYAnchor),
            control.leadingAnchor.constraint(greaterThanOrEqualTo: label.trailingAnchor,
                                             constant: 6),
        ])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
}

/// The two fields inside the Custom Coordinates sheet, with the live validation Device Hub does:
/// Apply stays disabled until both hold a number. Kept as its own object because NSAlert has no
/// place to hang a text-field delegate, and the delegate must outlive the call that builds it.
private final class CoordinateFields: NSObject, NSTextFieldDelegate {
    let lat = NSTextField()
    let lon = NSTextField()
    let view = NSView()
    weak var apply: NSButton?

    override init() {
        super.init()
        view.frame = NSRect(x: 0, y: 0, width: 260, height: 58)
        let rows = NSStackView(views: [row("Latitude", lat), row("Longitude", lon)])
        rows.orientation = .vertical
        rows.alignment = .leading
        rows.spacing = 8
        rows.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(rows)
        NSLayoutConstraint.activate([
            rows.topAnchor.constraint(equalTo: view.topAnchor),
            rows.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            rows.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            rows.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        for f in [lat, lon] { f.delegate = self }
    }

    private func row(_ title: String, _ field: NSTextField) -> NSView {
        let label = NSTextField(labelWithString: title)
        label.alignment = .right
        label.translatesAutoresizingMaskIntoConstraints = false
        label.widthAnchor.constraint(equalToConstant: 70).isActive = true
        field.translatesAutoresizingMaskIntoConstraints = false
        field.widthAnchor.constraint(equalToConstant: 170).isActive = true
        let h = NSStackView(views: [label, field])
        h.orientation = .horizontal
        h.spacing = 8
        return h
    }

    func controlTextDidChange(_ obj: Notification) {
        apply?.isEnabled = Double(lat.stringValue) != nil && Double(lon.stringValue) != nil
    }
}
