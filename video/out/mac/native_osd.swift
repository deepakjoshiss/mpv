/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

import Cocoa

// --macos-osd: an OSD drawn with Core Animation in a view above mpv's own, for the native video
// path and libplacebo alike. What it shows and where comes from osd_model.swift; this file draws it
// in design pixels (1920x1080), scaled to the window, and turns clicks into mpv commands.
class NativeOSD: NSView, EventSubscriber {
    enum Control: Equatable {
        case bookmark, info, options, chapterPrev, back, playPause, forward, chapterNext
        case subs, audio, seek, row(Int)
    }

    struct Key {
        let control: Control
        let size: CGFloat
        let plate = CALayer()
        let icon = CAShapeLayer()
    }

    struct Pill {
        let control: Control
        let plate = CALayer()
        let icon = CAShapeLayer()
        let label = CATextLayer()
        let chip = CALayer()
        let tag = CATextLayer()
    }

    var input: InputHelper { return AppHub.shared.input }
    var state = OSDState()
    var geo = OSDLayout(viewWidth: 1920, viewHeight: 1080)

    let canvas = CALayer()
    let veil = CAGradientLayer()
    let kicker = CATextLayer(), title = CATextLayer(), meta = CATextLayer(), clock = CATextLayer()
    let elapsed = CATextLayer(), remaining = CATextLayer()
    let track = CALayer(), played = CALayer(), blade = CALayer()
    var notches: [CALayer] = []
    var badges: [(CALayer, CATextLayer)] = []
    var keys: [Key] = []
    var pills: [Pill] = []
    let panel = CALayer()
    var rows: [(plate: CALayer, label: CATextLayer, check: CAShapeLayer, track: OSDTrack?)] = []

    var hitRects: [(Control, CGRect)] = []
    var hovered: Control?
    var panelType: String?      // "sub" or "audio" while the track panel is open
    var panelScroll = 0
    var dragFraction: Double?   // set while the seek bar is dragged
    var shown = false
    var hideTimer: Timer?
    var lastSecond = -1
    var lastBadges: [String] = []

    override var isFlipped: Bool { return true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.opacity = 0
        canvas.anchorPoint = .zero
        layer?.addSublayer(canvas)
        build()

        let ev = AppHub.shared.event
        ev?.subscribe(self, event: .init(name: "media-title", format: MPV_FORMAT_STRING))
        ev?.subscribe(self, event: .init(name: "metadata/by-key/date", format: MPV_FORMAT_STRING))
        ev?.subscribe(self, event: .init(name: "time-pos", format: MPV_FORMAT_DOUBLE))
        ev?.subscribe(self, event: .init(name: "duration", format: MPV_FORMAT_DOUBLE))
        ev?.subscribe(self, event: .init(name: "speed", format: MPV_FORMAT_DOUBLE))
        ev?.subscribe(self, event: .init(name: "pause", format: MPV_FORMAT_FLAG))
        ev?.subscribe(self, event: .init(name: "seeking", format: MPV_FORMAT_FLAG))
        ev?.subscribe(self, event: .init(name: "chapter-list", format: MPV_FORMAT_NODE))
        ev?.subscribe(self, event: .init(name: "track-list", format: MPV_FORMAT_NODE))
        ev?.subscribe(self, event: .init(name: "video-params/gamma", format: MPV_FORMAT_STRING))
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    // MARK: building

    func build() {
        veil.colors = [0, 0.3, 0.55, 0.72, 0.78, 0.8, 0.8].map { OSDStyle.bg.copy(alpha: $0)! }
        veil.locations = [0, 0.1, 0.2, 0.35, 0.5, 0.7, 1]
        veil.startPoint = CGPoint(x: 0.5, y: 0)
        veil.endPoint = CGPoint(x: 0.5, y: 1)
        track.backgroundColor = OSDStyle.fill3
        track.cornerRadius = 3
        track.masksToBounds = true
        played.backgroundColor = OSDStyle.accent
        track.addSublayer(played)
        blade.backgroundColor = OSDStyle.text
        blade.cornerRadius = 2
        [veil, kicker, title, meta, clock, elapsed, remaining, track, blade].forEach { canvas.addSublayer($0) }

        let order: [(Control, OSDIcon)] = [(.bookmark, .bookmark), (.info, .info), (.options, .options),
            (.chapterPrev, .prev), (.back, .back), (.playPause, .pause), (.forward, .forward), (.chapterNext, .next)]
        for (control, icon) in order {
            let key = Key(control: control, size: icon.size)
            key.plate.cornerRadius = 8
            key.icon.path = icon.path(size: icon.size)
            key.plate.addSublayer(key.icon)
            canvas.addSublayer(key.plate)
            keys.append(key)
        }
        for (control, icon) in [(Control.subs, OSDIcon.subs), (.audio, .audio)] {
            let pill = Pill(control: control)
            pill.plate.cornerRadius = 8
            pill.plate.borderWidth = 1
            pill.plate.borderColor = OSDStyle.line
            pill.icon.path = icon.path(size: 16)
            pill.icon.fillColor = OSDStyle.text62
            pill.chip.cornerRadius = 6
            pill.chip.backgroundColor = OSDStyle.fill2
            [pill.icon, pill.label, pill.chip, pill.tag].forEach { pill.plate.addSublayer($0) }
            canvas.addSublayer(pill.plate)
            pills.append(pill)
        }

        panel.backgroundColor = OSDStyle.panel
        panel.cornerRadius = 14
        panel.borderWidth = 1
        panel.borderColor = OSDStyle.line
        panel.isHidden = true
        panel.zPosition = 10   // above badges, which are re-added as they change
        canvas.addSublayer(panel)
    }

    // MARK: mpv state

    func handle(event: EventHelper.Event) {
        switch event.name {
        case "media-title": state.title = event.string ?? ""; poke()
        case "metadata/by-key/date": state.date = event.string
        case "time-pos": state.position = event.double ?? 0
        case "duration": state.duration = event.double ?? 0
        case "speed": state.speed = event.double ?? 1
        case "pause": state.paused = event.bool ?? false; poke()
        case "seeking": if event.bool == true { poke() }
        case "video-params/gamma": state.gamma = event.string
        case "chapter-list":
            state.chapters = event.array.compactMap { ($0 as? [String: Any?])?["time"] as? Double }
        case "track-list":
            state.tracks = event.array.compactMap { ($0 as? [String: Any?]).flatMap { OSDTrack($0) } }
            if panelType != nil { openPanel(panelType) }
        default: return
        }
        if event.name == "time-pos" { updateProgress() } else { relayout() }
    }

    // MARK: layout

    override func layout() {
        super.layout()
        relayout()
    }

    func relayout() {
        geo = OSDLayout(viewWidth: bounds.width, viewHeight: bounds.height)
        let L = geo, s = L.scale
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        canvas.bounds = CGRect(x: 0, y: 0, width: L.width, height: L.height)
        canvas.position = .zero
        canvas.setAffineTransform(CGAffineTransform(scaleX: s, y: s))
        hitRects = []

        veil.frame = CGRect(x: 0, y: L.veilTop, width: L.width, height: L.veil)
        let titleWidth = L.width - 2 * L.margin - (L.compact ? 360 : 560)
        set(kicker, kickerText, OSDStyle.font(.wide, 15), OSDStyle.accent,
            CGRect(x: L.margin, y: L.kickerTop, width: titleWidth, height: 22))
        set(title, state.title.uppercased(), OSDStyle.font(.title, 22), OSDStyle.text,
            CGRect(x: L.margin, y: L.titleTop, width: titleWidth, height: 35))
        meta.isHidden = L.compact
        set(meta, OSDText.meta(state), OSDStyle.font(.body, 20), OSDStyle.text62,
            CGRect(x: L.margin, y: L.metaTop, width: titleWidth, height: 22))
        set(clock, OSDText.clock(state), OSDStyle.font(.body, 20), OSDStyle.text62,
            CGRect(x: L.width - L.margin - 560, y: L.clockTop, width: 560, height: 22), align: .right)
        layoutBadges()

        set(elapsed, OSDText.time(shownPosition), OSDStyle.font(.body, 18), OSDStyle.text,
            CGRect(x: L.margin, y: L.timesTop, width: 120, height: 24))
        set(remaining, OSDText.remaining(state), OSDStyle.font(.body, 18), OSDStyle.text62,
            CGRect(x: L.width - L.margin - 160, y: L.timesTop, width: 160, height: 24), align: .right)
        track.frame = CGRect(x: L.trackLeft, y: L.trackTop, width: L.trackWidth, height: 6)
        layoutNotches()
        hitRects.append((.seek, CGRect(x: L.trackLeft, y: L.bladeTop - 1, width: L.trackWidth, height: 24)))

        layoutKeys()
        layoutPills()
        layoutPanel()
        updateProgress(force: true)
        applyHover(animated: false)
        CATransaction.commit()
        setScale(canvas, (window?.backingScaleFactor ?? 2) * s)
    }

    var kickerText: String {
        if let f = dragFraction {
            let delta = f * state.duration - state.position
            let m = Int(abs(delta)) / 60, sec = Int(abs(delta)) % 60
            return String(format: "SEEKING %@%d:%02d", delta < 0 ? "\u{2212}" : "+", m, sec)
        }
        return state.paused ? "PAUSED" : "NOW PLAYING"
    }

    var shownPosition: Double { return dragFraction.map { $0 * state.duration } ?? state.position }

    func layoutBadges() {
        let L = geo
        let labels = L.compact ? [] : OSDText.badges(state)
        if labels != lastBadges {
            badges.forEach { $0.0.removeFromSuperlayer() }
            badges = labels.map { _ in
                let plate = CALayer(), text = CATextLayer()
                plate.borderWidth = 1
                plate.borderColor = OSDStyle.lineBadge
                plate.cornerRadius = 8
                plate.addSublayer(text)
                canvas.addSublayer(plate)
                return (plate, text)
            }
            lastBadges = labels
        }
        var right = L.width - L.margin
        for (i, label) in labels.enumerated().reversed() {
            let font = OSDStyle.font(.wide, 15)
            let w = OSDStyle.width(label, font) + 32
            right -= w
            badges[i].0.frame = CGRect(x: right, y: L.badgesTop, width: w, height: 40)
            set(badges[i].1, label, font, OSDStyle.textDim, CGRect(x: 0, y: 10, width: w, height: 22), align: .center)
            right -= 8
        }
    }

    func layoutNotches() {
        let fractions = state.chapterFractions
        while notches.count < fractions.count {
            let n = CALayer()
            n.backgroundColor = OSDStyle.notch
            track.addSublayer(n)
            notches.append(n)
        }
        for (i, n) in notches.enumerated() {
            n.isHidden = i >= fractions.count
            if i < fractions.count {
                n.frame = CGRect(x: track.bounds.width * fractions[i] - 1.5, y: 0, width: 3, height: 6)
            }
        }
    }

    func layoutKeys() {
        let L = geo
        let left = L.compact ? [Control.options] : [.bookmark, .info, .options]
        let centre: [Control] = [.chapterPrev, .back, .playPause, .forward, .chapterNext]
        let centreX = (L.width - (5 * 86 + 4 * 16)) / 2
        for key in keys {
            var x: CGFloat?
            if let i = left.firstIndex(of: key.control) { x = L.margin + CGFloat(i) * 102 }
            if let i = centre.firstIndex(of: key.control) { x = centreX + CGFloat(i) * 102 }
            key.plate.isHidden = x == nil
            guard let kx = x else { continue }
            let frame = CGRect(x: kx, y: L.keysTop, width: 86, height: 48)
            key.plate.transform = CATransform3DIdentity   // frame is undefined under the hover zoom
            key.plate.frame = frame
            let isPlay = key.control == .playPause
            if isPlay { key.icon.path = (state.paused ? OSDIcon.play : OSDIcon.pause).path(size: 18) }
            key.icon.fillColor = isPlay ? OSDStyle.onAccent : OSDStyle.text
            let sz = key.size
            key.icon.frame = CGRect(x: (86 - sz) / 2, y: (48 - sz) / 2, width: sz, height: sz)
            hitRects.append((key.control, frame))
        }
    }

    func layoutPills() {
        let L = geo
        var right = L.width - L.margin
        for pill in pills.reversed() {
            let type = pill.control == .subs ? "sub" : "audio"
            let word = L.compact ? "" : (type == "sub" ? "SUBS" : "AUD")
            let wide = OSDStyle.font(.wide, 15)
            let tag = OSDText.tag(state, type)
            let wordW = word.isEmpty ? 0 : OSDStyle.width(word, wide) + 10
            let tagW = tag.map { OSDStyle.width($0, wide) + 18 } ?? 0
            let leftPad: CGFloat = L.compact ? 16 : 20
            let w = leftPad + 16 + 10 + wordW + (tag != nil ? 4 + tagW + 10 : 10)
            right -= w
            let frame = CGRect(x: right, y: L.keysTop, width: w, height: 48)
            pill.plate.transform = CATransform3DIdentity
            pill.plate.frame = frame
            pill.icon.frame = CGRect(x: leftPad, y: 16, width: 16, height: 16)
            set(pill.label, word, wide, OSDStyle.text62,
                CGRect(x: leftPad + 26, y: 13, width: wordW, height: 22))
            pill.chip.isHidden = tag == nil
            if let tag = tag {
                let cx = leftPad + 26 + wordW + 4
                pill.chip.frame = CGRect(x: cx, y: 10, width: tagW, height: 28)
                set(pill.tag, tag, wide, OSDStyle.text, CGRect(x: cx, y: 13, width: tagW, height: 22), align: .center)
            }
            hitRects.append((pill.control, frame))
            right -= 16
        }
    }

    func layoutPanel() {
        guard let type = panelType else {
            panel.isHidden = true
            return
        }
        let L = geo
        let options: [OSDTrack?] = (type == "sub" ? [nil] : []) + state.tracks(type).map { Optional($0) }
        let visible = min(options.count, 5)
        panelScroll = min(max(panelScroll, 0), max(options.count - visible, 0))
        let h = CGFloat(69 + visible * 56 + max(visible - 1, 0) * 2 + 10)
        let pillFrame = pills.first { $0.control == (type == "sub" ? .subs : .audio) }?.plate.frame ?? .zero
        let x = min(max(pillFrame.maxX + 14 - 340, L.margin), L.width - L.margin - 340)
        let frame = CGRect(x: x, y: L.keysTop - 20 - h, width: 340, height: h)
        panel.isHidden = false
        panel.frame = frame

        panel.sublayers?.forEach { $0.removeFromSuperlayer() }
        let icon = CAShapeLayer()
        icon.path = (type == "sub" ? OSDIcon.subs : OSDIcon.audio).path(size: 15)
        icon.fillColor = OSDStyle.text
        icon.frame = CGRect(x: 21, y: 17, width: 15, height: 15)
        let head = CATextLayer()
        set(head, type == "sub" ? "SUBTITLES" : "AUDIO", OSDStyle.font(.wide, 16), OSDStyle.text,
            CGRect(x: 49, y: 15, width: 278, height: 22))
        let rule = CALayer()
        rule.backgroundColor = OSDStyle.line
        rule.frame = CGRect(x: 11, y: 51, width: 316, height: 1)
        [icon, head, rule].forEach { panel.addSublayer($0) }

        rows = []
        for i in 0..<visible {
            let t = options[i + panelScroll]
            let selected = t?.selected ?? (state.selected(type) == nil)
            let plate = CALayer(), label = CATextLayer(), check = CAShapeLayer()
            let rowFrame = CGRect(x: 9, y: 68 + CGFloat(i) * 58, width: 320, height: 56)
            plate.frame = rowFrame
            plate.cornerRadius = 8
            plate.borderColor = OSDStyle.accentRing
            set(label, t.map { OSDText.label($0) } ?? "Off", OSDStyle.font(.body, 24),
                selected ? OSDStyle.accent : OSDStyle.text, CGRect(x: 14, y: 12, width: 254, height: 32))
            plate.addSublayer(label)
            if selected {
                check.path = OSDIcon.check.path(size: 18)
                check.fillColor = OSDStyle.accent
                check.frame = CGRect(x: 288, y: 19, width: 18, height: 18)
                plate.addSublayer(check)
            }
            panel.addSublayer(plate)
            rows.append((plate, label, check, t))
            hitRects.append((.row(i), rowFrame.offsetBy(dx: frame.minX, dy: frame.minY)))
        }
        setScale(panel, (window?.backingScaleFactor ?? 2) * L.scale)
    }

    func updateProgress(force: Bool = false) {
        let f = dragFraction ?? state.fraction
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        played.frame = CGRect(x: 0, y: 0, width: track.bounds.width * f, height: 6)
        blade.frame = CGRect(x: geo.trackLeft + geo.trackWidth * f - 2.5, y: geo.bladeTop, width: 5, height: 22)
        let second = Int(shownPosition)
        if force || second != lastSecond {
            lastSecond = second
            elapsed.string = text(OSDText.time(shownPosition), OSDStyle.font(.body, 18), OSDStyle.text)
            remaining.string = text(OSDText.remaining(state), OSDStyle.font(.body, 18), OSDStyle.text62)
            clock.string = text(OSDText.clock(state), OSDStyle.font(.body, 20), OSDStyle.text62)
        }
        CATransaction.commit()
    }

    // MARK: hover

    // Hovered keys and pills brighten, take an accent ring and Jellyvin's 18 px knob glow; hovered panel
    // rows take an accent tint and a 1 px accent line instead. All zoom like Jellyvin's cards: 103.5%
    // (rows 102%) over 260 ms cubic ease-out, in and out.
    // Instant with Reduce Motion.
    func applyHover(animated: Bool) {
        CATransaction.begin()
        if animated && !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            CATransaction.setAnimationDuration(0.26)
            CATransaction.setAnimationTimingFunction(CAMediaTimingFunction(controlPoints: 0.215, 0.61, 0.355, 1))
        } else {
            CATransaction.setDisableActions(true)
        }
        for key in keys {
            let on = hovered == key.control, isPlay = key.control == .playPause
            key.plate.backgroundColor = isPlay ? OSDStyle.accent : (on ? OSDStyle.fill3 : OSDStyle.fill1)
            key.plate.borderColor = OSDStyle.accentRing
            key.plate.borderWidth = on && !isPlay ? 1.5 : 0
            zoom(key.plate, on ? 1.035 : 1)
            glow(key.plate, on)
        }
        for pill in pills {
            let on = hovered == pill.control
            let open = panelType == (pill.control == .subs ? "sub" : "audio")
            pill.plate.backgroundColor = on || open ? OSDStyle.fill3 : OSDStyle.clear
            pill.plate.borderColor = on || open ? OSDStyle.accentRing : OSDStyle.line
            pill.plate.borderWidth = on ? 1.5 : 1
            zoom(pill.plate, on ? 1.035 : 1)
            glow(pill.plate, on)
        }
        for (i, row) in rows.enumerated() {
            let on = hovered == .row(i)
            row.plate.backgroundColor = on ? OSDStyle.accentTint : OSDStyle.fill2
            row.plate.borderWidth = on ? 1 : 0
            zoom(row.plate, on ? 1.02 : 1)
        }
        CATransaction.commit()
    }

    func zoom(_ layer: CALayer, _ s: CGFloat) {
        layer.transform = CATransform3DMakeScale(s, s, 1)
    }

    // The glow sits on its own sublayer, masked to outside the plate: a layer's own shadow shows
    // through a translucent plate and tints it cyan.
    func glow(_ layer: CALayer, _ on: Bool) {
        let g = layer.sublayers?.first { $0.name == "glow" } ?? {
            let g = CALayer()
            g.name = "glow"
            g.shadowColor = OSDStyle.accent
            g.shadowOffset = .zero
            g.shadowRadius = 18
            g.mask = CAShapeLayer()
            layer.insertSublayer(g, at: 0)
            return g
        }()
        let r = layer.cornerRadius, b = layer.bounds
        let plate = CGPath(roundedRect: b, cornerWidth: r, cornerHeight: r, transform: nil)
        let outside = CGMutablePath()
        outside.addRect(b.insetBy(dx: -60, dy: -60))
        outside.addPath(plate)
        if let mask = g.mask as? CAShapeLayer, g.bounds != b {
            g.frame = b
            g.shadowPath = plate
            mask.frame = b
            mask.fillRule = .evenOdd
            mask.path = outside
        }
        g.shadowOpacity = on ? 0.65 : 0
    }

    // MARK: text helpers

    func text(_ s: String, _ font: NSFont, _ color: CGColor) -> NSAttributedString {
        return NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: NSColor(cgColor: color) ?? .white])
    }

    func set(_ layer: CATextLayer, _ s: String, _ font: NSFont, _ color: CGColor, _ frame: CGRect,
             align: CATextLayerAlignmentMode = .left) {
        layer.string = text(s, font, color)
        layer.alignmentMode = align
        layer.truncationMode = .end
        layer.frame = frame
    }

    func setScale(_ layer: CALayer, _ scale: CGFloat) {
        layer.contentsScale = scale
        layer.sublayers?.forEach { setScale($0, scale) }
    }

    // MARK: showing and hiding

    func poke() {
        if !shown {
            shown = true
            fade(to: 1)
        }
        hideTimer?.invalidate()
        hideTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: false) { [weak self] _ in self?.hideIfIdle() }
    }

    func hideIfIdle() {
        let mouse = window.map { convert($0.mouseLocationOutsideOfEventStream, from: nil) } ?? .zero
        let overBand = bounds.contains(mouse) && mouse.y / geo.scale >= geo.veilTop
        if state.paused || overBand || panelType != nil || dragFraction != nil {
            poke()
            return
        }
        shown = false
        hovered = nil
        fade(to: 0)
    }

    func fade(to opacity: Float) {
        CATransaction.begin()
        CATransaction.setAnimationDuration(0.2)
        layer?.opacity = opacity
        CATransaction.commit()
    }

    // MARK: input

    override func updateTrackingAreas() {
        trackingAreas.forEach { removeTrackingArea($0) }
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect],
                                       owner: self, userInfo: nil))
        super.updateTrackingAreas()
    }

    func designPoint(_ event: NSEvent) -> CGPoint {
        let p = convert(event.locationInWindow, from: nil)
        return CGPoint(x: p.x / geo.scale, y: p.y / geo.scale)
    }

    func control(at p: CGPoint) -> Control? {
        return hitRects.last { $0.1.contains(p) }?.0
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        guard shown, let superview = superview else { return nil }
        let p = convert(point, from: superview)
        if panelType != nil && bounds.contains(p) { return self }
        return control(at: CGPoint(x: p.x / geo.scale, y: p.y / geo.scale)) != nil ? self : nil
    }

    override func mouseMoved(with event: NSEvent) {
        poke()
        let now = control(at: designPoint(event))
        if now != hovered {
            hovered = now
            applyHover(animated: true)
        }
    }

    override func mouseDown(with event: NSEvent) {
        let p = designPoint(event)
        guard let c = control(at: p) else {
            openPanel(nil)
            return
        }
        poke()
        switch c {
        case .bookmark: input.command("script-binding select/select-chapter")
        case .info: input.command("script-binding stats/display-stats-toggle")
        case .options: input.command("script-binding select/menu")
        case .chapterPrev: input.command("no-osd add chapter -1")
        case .chapterNext: input.command("no-osd add chapter 1")
        case .back: input.command("no-osd seek -5")
        case .forward: input.command("no-osd seek 5")
        case .playPause: input.command("cycle pause")
        case .subs: openPanel(panelType == "sub" ? nil : "sub")
        case .audio: openPanel(panelType == "audio" ? nil : "audio")
        case .seek: drag(to: p, exact: false)
        case .row(let i):
            guard i < rows.count, let type = panelType else { return }
            let prop = type == "sub" ? "sid" : "aid"
            input.command("set \(prop) \(rows[i].track.map { String($0.id) } ?? "no")")
            openPanel(nil)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        if dragFraction != nil { drag(to: designPoint(event), exact: false) }
    }

    override func mouseUp(with event: NSEvent) {
        if dragFraction != nil {
            drag(to: designPoint(event), exact: true)
            dragFraction = nil
            relayout()
        }
    }

    override func scrollWheel(with event: NSEvent) {
        guard panelType != nil else { return }
        panelScroll += event.scrollingDeltaY < 0 ? 1 : (event.scrollingDeltaY > 0 ? -1 : 0)
        relayout()
    }

    func drag(to p: CGPoint, exact: Bool) {
        guard state.duration > 0 else { return }
        let f = min(max(Double((p.x - geo.trackLeft) / geo.trackWidth), 0), 1)
        dragFraction = f
        input.command(String(format: "no-osd seek %.3f absolute+%@", f * state.duration, exact ? "exact" : "keyframes"))
        set(kicker, kickerText, OSDStyle.font(.wide, 15), OSDStyle.accent, kicker.frame)
        updateProgress(force: true)
    }

    func openPanel(_ type: String?) {
        if type != panelType { panelScroll = 0 }
        panelType = type
        relayout()
    }
}

enum OSDStyle {
    static func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
        return CGColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: a)
    }
    // Jellyvin's colours/defaults.xml
    static let bg = rgb(7, 8, 12)
    static let text = rgb(243, 244, 247)
    static let text62 = rgb(243, 244, 247, 0.62)
    static let textDim = rgb(137, 138, 141)
    static let accent = rgb(77, 234, 255)
    static let onAccent = rgb(11, 13, 19)
    static let fill1 = rgb(255, 255, 255, 0.06)
    static let fill2 = rgb(255, 255, 255, 0.09)
    static let fill3 = rgb(255, 255, 255, 0.14)
    static let line = rgb(255, 255, 255, 0.09)
    static let lineBadge = rgb(243, 244, 247, 0.18)
    static let notch = rgb(7, 8, 12, 0.6)
    static let panel = rgb(11, 13, 19)            // jv_panel, solid: no blur behind it
    static let accentRing = rgb(77, 234, 255, 0.6)
    static let accentTint = rgb(77, 234, 255, 0.22)   // hovered panel row
    static let clear = rgb(255, 255, 255, 0)

    // Jellyvin's Archivo cuts (tracking baked into the t140 ones), shipped in the app bundle
    enum Face: String {
        case body = "Archivo-SemiBold-t000"
        case wide = "ArchivoExp120-Bold-t140"
        case title = "ArchivoSemiExp110-SemiBold-t140"
    }

    // the three cuts share one PostScript name, so each font is made from its own file, not by name
    static var descriptors: [Face: CTFontDescriptor] = {
        var out: [Face: CTFontDescriptor] = [:]
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("fonts") else { return out }
        for face in [Face.body, .wide, .title] {
            let url = dir.appendingPathComponent(face.rawValue + ".ttf") as CFURL
            out[face] = (CTFontManagerCreateFontDescriptorsFromURL(url) as? [CTFontDescriptor])?.first
        }
        return out
    }()
    static var cache: [String: NSFont] = [:]

    static func font(_ face: Face, _ size: CGFloat) -> NSFont {
        let key = "\(face.rawValue)-\(size)"
        if let f = cache[key] { return f }
        let f = descriptors[face].map { CTFontCreateWithFontDescriptor($0, size, nil) as NSFont }
            ?? NSFont.systemFont(ofSize: size, weight: face == .body ? .semibold : .bold)
        cache[key] = f
        return f
    }

    static func width(_ s: String, _ font: NSFont) -> CGFloat {
        return ceil(NSAttributedString(string: s, attributes: [.font: font]).size().width)
    }
}

// Icons drawn on a 24x24 grid, y down, as filled outlines.
enum OSDIcon {
    case bookmark, info, options, prev, back, pause, play, forward, next, subs, audio, check

    var size: CGFloat {
        switch self {
        case .info, .options: return 20
        case .back, .forward: return 22
        default: return 18
        }
    }

    func path(size: CGFloat) -> CGPath {
        let p = CGMutablePath()
        func stroke(_ sub: CGPath, _ w: CGFloat) { p.addPath(sub.copy(strokingWithWidth: w, lineCap: .round, lineJoin: .round, miterLimit: 4)) }
        func poly(_ pts: [(CGFloat, CGFloat)], close: Bool = true) -> CGPath {
            let s = CGMutablePath()
            s.addLines(between: pts.map { CGPoint(x: $0.0, y: $0.1) })
            if close { s.closeSubpath() }
            return s
        }
        func arc(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat, from a0: CGFloat, to a1: CGFloat) -> CGPath {
            let n = 48
            return poly((0...n).map { i in
                let a = (a0 + (a1 - a0) * CGFloat(i) / CGFloat(n)) * .pi / 180
                return (cx + r * cos(a), cy + r * sin(a))
            }, close: false)
        }
        func digit(_ ch: String, x: CGFloat, baseline: CGFloat, size: CGFloat) {
            let font = CTFontCreateWithName("Helvetica-Bold" as CFString, size, nil)
            var glyph = CGGlyph()
            var u = Array(ch.utf16)
            guard CTFontGetGlyphsForCharacters(font, &u, &glyph, 1), let g = CTFontCreatePathForGlyph(font, glyph, nil) else { return }
            let w = g.boundingBox.width
            var t = CGAffineTransform(translationX: x - w / 2 - g.boundingBox.minX, y: baseline).scaledBy(x: 1, y: -1)
            if let moved = g.copy(using: &t) { p.addPath(moved) }
        }
        switch self {
        case .bookmark: p.addPath(poly([(6, 3), (18, 3), (18, 21), (12, 16.5), (6, 21)]))
        case .info:
            stroke(CGPath(ellipseIn: CGRect(x: 3, y: 3, width: 18, height: 18), transform: nil), 2.2)
            stroke(poly([(12, 11), (12, 17)], close: false), 2.2)
            p.addEllipse(in: CGRect(x: 10.8, y: 6.3, width: 2.4, height: 2.4))
        case .options:
            stroke(poly([(3, 8), (21, 8)], close: false), 2)
            stroke(poly([(3, 16), (21, 16)], close: false), 2)
            p.addRoundedRect(in: CGRect(x: 13.5, y: 4.5, width: 2.6, height: 7), cornerWidth: 1, cornerHeight: 1)
            p.addRoundedRect(in: CGRect(x: 7.5, y: 12.5, width: 2.6, height: 7), cornerWidth: 1, cornerHeight: 1)
        case .prev:
            p.addRoundedRect(in: CGRect(x: 5, y: 5, width: 2.6, height: 14), cornerWidth: 1, cornerHeight: 1)
            p.addPath(poly([(19, 5), (19, 19), (8.5, 12)]))
        case .next:
            p.addPath(poly([(5, 5), (5, 19), (15.5, 12)]))
            p.addRoundedRect(in: CGRect(x: 16.4, y: 5, width: 2.6, height: 14), cornerWidth: 1, cornerHeight: 1)
        case .pause:
            p.addRoundedRect(in: CGRect(x: 6, y: 4, width: 4, height: 16), cornerWidth: 1, cornerHeight: 1)
            p.addRoundedRect(in: CGRect(x: 14, y: 4, width: 4, height: 16), cornerWidth: 1, cornerHeight: 1)
        case .play: p.addPath(poly([(7, 4.5), (7, 19.5), (19.5, 12)]))
        case .back:
            stroke(arc(12, 12, 8, from: 180, to: -134.5), 2)
            stroke(poly([(4, 3.5), (4, 8.3), (8.8, 8.3)], close: false), 2)
            digit("5", x: 12.5, baseline: 15.6, size: 8.5)
        case .forward:
            stroke(arc(12, 12, 8, from: 0, to: 314.5), 2)
            stroke(poly([(20, 3.5), (20, 8.3), (15.2, 8.3)], close: false), 2)
            digit("5", x: 11.5, baseline: 15.6, size: 8.5)
        case .subs:
            stroke(CGPath(roundedRect: CGRect(x: 3, y: 5, width: 18, height: 14), cornerWidth: 2, cornerHeight: 2, transform: nil), 2)
            stroke(poly([(7, 15), (12, 15)], close: false), 2)
            stroke(poly([(14, 15), (17, 15)], close: false), 2)
        case .audio:
            p.addPath(poly([(3, 9), (7, 9), (12, 5), (12, 19), (7, 15), (3, 15)]))
            stroke(arc(14.6, 12, 3.4, from: -62, to: 62), 2)
        case .check: stroke(poly([(5, 12.5), (9.5, 17), (19, 7.5)], close: false), 3)
        }
        var t = CGAffineTransform(scaleX: size / 24, y: size / 24)
        return p.copy(using: &t) ?? p
    }
}
