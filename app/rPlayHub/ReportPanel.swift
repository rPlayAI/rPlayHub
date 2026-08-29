//
//  ReportPanel.swift
//  Device Hub's Report tab: the crash reports on the device.
//
//  Checked against Device Hub's own Report page: a flat list, each row an icon, the process name
//  in bold, and "Today at 10:53:21 AM • 7 KB" beneath it — a relative date and a file size. A
//  Filter field and a category popup sit in a bar along the bottom, the same shape the Apps tab
//  uses. No column headers.
//
//  The engine already had everything: the crash reports come over AFC on
//  crashreportcopymobile (`list_dir` with service "crash"), which pokes crashreportmover first so
//  the device moves pending reports into place. Nothing new was needed on that side.
//

import AppKit

final class ReportPanel: NSView {
    var control: ControlClient? {
        didSet { if control != nil, !hasLoaded { reload() } }
    }

    /// One crash report, as the list needs it.
    private struct Report {
        let file: String        // the on-device filename
        let process: String     // what Device Hub shows in bold
        let size: Int
        let date: Date
    }

    private var all: [Report] = []
    private var shown: [Report] = []
    private var hasLoaded = false

    private let table = NSTableView()
    private let filter = NSSearchField()
    private let category = NSPopUpButton()
    private let status = NSTextField(labelWithString: "")

    /// Device Hub's own bottom-bar categories. "Crashes" is its default and the only one we can
    /// fill honestly today: the device's crash directory also holds tailspins and log archives,
    /// which are a different kind of thing and get their own entry rather than being mixed in.
    private enum Category: String, CaseIterable {
        case crashes = "Crashes"
        case logs = "Logs"
        case all = "All"

        func matches(_ file: String) -> Bool {
            let crash = file.hasSuffix(".ips") || file.hasSuffix(".crash") || file.hasSuffix(".panic")
            switch self {
            case .crashes: return crash
            case .logs:    return !crash
            case .all:     return true
            }
        }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        let col = NSTableColumn(identifier: .init("report"))
        table.addTableColumn(col)
        table.headerView = nil
        table.rowHeight = 48                 // Device Hub's rows: two lines plus padding
        table.dataSource = self
        table.delegate = self
        table.usesAlternatingRowBackgroundColors = false
        table.style = .plain
        // #F3F3F2, as everywhere else: a touch darker than the pane behind it.
        let rowBackground = NSColor(srgbRed: 0xF3 / 255, green: 0xF3 / 255, blue: 0xF2 / 255, alpha: 1)
        table.backgroundColor = rowBackground

        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.drawsBackground = true
        scroll.backgroundColor = rowBackground
        scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false

        filter.placeholderString = "Filter"
        filter.controlSize = .small
        filter.sendsSearchStringImmediately = true
        filter.target = self
        filter.action = #selector(refilter)

        category.addItems(withTitles: Category.allCases.map(\.rawValue))
        category.controlSize = .small
        category.bezelStyle = .accessoryBarAction
        category.target = self
        category.action = #selector(refilter)

        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.preferredMaxLayoutWidth = 224    // a wrapping label needs this, not a width
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3

        // Filter and category along the bottom, as Device Hub has them.
        let bar = NSStackView(views: [filter, category])
        bar.orientation = .horizontal
        bar.spacing = 6
        let stack = NSStackView(views: [scroll, bar, status])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
            scroll.widthAnchor.constraint(equalTo: stack.widthAnchor),
            bar.widthAnchor.constraint(equalTo: stack.widthAnchor),
        ])
    }

    /// Called when the tab is first revealed, so a pane nobody is looking at costs nothing --
    /// listing crash reports opens an AFC session and pokes crashreportmover.
    func revealed() {
        if !hasLoaded { reload() }
    }

    func reload() {
        guard let control else { return }
        hasLoaded = true
        status.stringValue = "Reading crash reports…"
        control.send("list_dir", ["service": "crash", "path": "/"]) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let info):
                let entries = info["entries"] as? [[String: Any]] ?? []
                self.all = entries.compactMap { e in
                    guard let name = e["name"] as? String, (e["is_dir"] as? Bool) != true
                    else { return nil }
                    let mtime = (e["mtime"] as? Double) ?? 0
                    return Report(file: name,
                                  process: Self.processName(from: name),
                                  size: (e["size"] as? Int) ?? 0,
                                  date: Date(timeIntervalSince1970: mtime))
                }.sorted { $0.date > $1.date }        // newest first, as Device Hub lists them
                self.applyFilter()
            case .failure(let e):
                self.status.stringValue = "Could not read crash reports: \(e)"
            }
        }
    }

    /// Device Hub shows just the process, so the timestamp the device appends has to come off.
    ///
    /// Two shapes on the device, both seen in one listing:
    ///   fusiond-2026-08-29-095321.ips                  process-date-time
    ///   airplayd_blockage_1_2026.08.09_12-09-51.tailspin   process_..._date_time
    /// Rather than pattern-match either, drop the extension and then trim trailing components
    /// that start with a digit -- the name itself never does, and every trailing date component
    /// does.
    static func processName(from file: String) -> String {
        var name = file
        if let dot = name.lastIndex(of: ".") { name = String(name[name.startIndex..<dot]) }
        while true {
            guard let cut = name.lastIndex(where: { $0 == "-" || $0 == "_" }) else { break }
            let tail = name[name.index(after: cut)...]
            guard let first = tail.first, first.isNumber else { break }
            name = String(name[name.startIndex..<cut])
        }
        return name.isEmpty ? file : name
    }

    @objc private func refilter() { applyFilter() }

    private func applyFilter() {
        let cat = Category(rawValue: category.titleOfSelectedItem ?? "") ?? .crashes
        let needle = filter.stringValue.lowercased()
        shown = all.filter { r in
            cat.matches(r.file) &&
            (needle.isEmpty || r.process.lowercased().contains(needle)
                            || r.file.lowercased().contains(needle))
        }
        table.reloadData()
        status.stringValue = shown.isEmpty
            ? (all.isEmpty ? "No crash reports on this device." : "Nothing matches that filter.")
            : ""
    }

    private static let dateText: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .medium
        f.doesRelativeDateFormatting = true      // gives Device Hub's "Today at 10:53:21 AM"
        return f
    }()
}

extension ReportPanel: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int { shown.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?,
                   row: Int) -> NSView? {
        let r = shown[row]
        let cell = NSTableCellView()

        let icon = NSImageView()
        icon.image = NSImage(systemSymbolName: "doc.text", accessibilityDescription: "Report")
        icon.contentTintColor = .secondaryLabelColor
        icon.translatesAutoresizingMaskIntoConstraints = false

        let title = NSTextField(labelWithString: r.process)
        title.font = .systemFont(ofSize: 12, weight: .semibold)
        title.lineBreakMode = .byTruncatingTail
        title.translatesAutoresizingMaskIntoConstraints = false

        let size = ByteCountFormatter.string(fromByteCount: Int64(r.size), countStyle: .file)
        let sub = NSTextField(labelWithString: "\(Self.dateText.string(from: r.date)) • \(size)")
        sub.font = .systemFont(ofSize: 10)
        sub.textColor = .secondaryLabelColor
        sub.lineBreakMode = .byTruncatingTail
        sub.translatesAutoresizingMaskIntoConstraints = false

        for v in [icon, title, sub] as [NSView] { cell.addSubview(v) }
        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 6),
            icon.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            icon.widthAnchor.constraint(equalToConstant: 18),
            title.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 8),
            title.trailingAnchor.constraint(lessThanOrEqualTo: cell.trailingAnchor, constant: -6),
            title.topAnchor.constraint(equalTo: cell.topAnchor, constant: 7),
            sub.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            sub.trailingAnchor.constraint(lessThanOrEqualTo: cell.trailingAnchor, constant: -6),
            sub.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 2),
        ])

        // A per-cell hairline, inset past the icon like Device Hub's. Not gridStyleMask: that
        // paints across the table's whole visible height regardless of how many rows exist, which
        // is what put "row shadows" under an empty list before.
        let line = NSBox()
        line.boxType = .separator
        line.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(line)
        NSLayoutConstraint.activate([
            line.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 32),
            line.trailingAnchor.constraint(equalTo: cell.trailingAnchor),
            line.bottomAnchor.constraint(equalTo: cell.bottomAnchor),
        ])
        return cell
    }
}
