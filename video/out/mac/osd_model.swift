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

import Foundation

// --macos-osd, the portable part: what the OSD shows and where, with no UI framework in it, so a
// renderer for another platform can reuse it (native_osd.swift is the macOS one). Positions are in
// design pixels of a 1920x1080 frame, the size the OSD was designed at; the renderer scales them.

struct OSDTrack {
    var id: Int
    var type: String
    var title: String?
    var lang: String?
    var selected: Bool
    var forced: Bool
    var hearingImpaired: Bool
    var codec: String?
    var codecProfile: String?
    var channels: Int?
    var width: Int?
    var height: Int?
    var dvProfile: Int?

    // one entry of mpv's track-list property
    init?(_ node: [String: Any?]) {
        guard let id = node["id"] as? Int64, let type = node["type"] as? String else { return nil }
        self.id = Int(id)
        self.type = type
        title = node["title"] as? String
        lang = node["lang"] as? String
        selected = node["selected"] as? Bool ?? false
        forced = node["forced"] as? Bool ?? false
        hearingImpaired = node["hearing-impaired"] as? Bool ?? false
        codec = node["codec"] as? String
        codecProfile = node["codec-profile"] as? String
        channels = (node["demux-channel-count"] as? Int64).map { Int($0) }
        width = (node["demux-w"] as? Int64).map { Int($0) }
        height = (node["demux-h"] as? Int64).map { Int($0) }
        dvProfile = (node["dolby-vision-profile"] as? Int64).map { Int($0) }
    }
}

struct OSDState {
    var title = ""
    var date: String?           // metadata date or year, if the file has one
    var paused = false
    var position = 0.0
    var duration = 0.0
    var speed = 1.0
    var chapters: [Double] = [] // chapter start times in seconds
    var tracks: [OSDTrack] = []
    var gamma: String?          // video-params/gamma: "pq", "hlg", ...

    func tracks(_ type: String) -> [OSDTrack] { return tracks.filter { $0.type == type } }
    func selected(_ type: String) -> OSDTrack? { return tracks.first { $0.type == type && $0.selected } }
    var fraction: Double { return duration > 0 ? min(max(position / duration, 0), 1) : 0 }
    var chapterFractions: [Double] {
        return duration > 0 ? chapters.filter { $0 > 0 && $0 < duration }.map { $0 / duration } : []
    }
}

enum OSDText {
    static func time(_ t: Double) -> String {
        let s = Int(max(t, 0).rounded(.down))
        return String(format: "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
    }

    static func remaining(_ s: OSDState) -> String {
        return "\u{2013}" + time(s.duration - s.position)
    }

    static func runtime(_ d: Double) -> String {
        let m = Int((d / 60).rounded())
        return m >= 60 ? String(format: "%dh %02dm", m / 60, m % 60) : "\(m)m"
    }

    // year · runtime, whichever is known
    static func meta(_ s: OSDState) -> String {
        var parts: [String] = []
        if let d = s.date, d.count >= 4, Int(d.prefix(4)) != nil { parts.append(String(d.prefix(4))) }
        if s.duration > 0 { parts.append(runtime(s.duration)) }
        return parts.joined(separator: " · ")
    }

    // "11:37 PM · ENDS AT 12:43 AM", in the system's 12/24-hour style
    static func clock(_ s: OSDState, now: Date = Date()) -> String {
        let f = DateFormatter()
        f.timeStyle = .short
        f.dateStyle = .none
        var text = f.string(from: now)
        if s.duration > 0 {
            let left = max(s.duration - s.position, 0) / max(s.speed, 0.01)
            text += " · ENDS AT " + f.string(from: now.addingTimeInterval(left))
        }
        return text.uppercased()
    }

    static func badges(_ s: OSDState) -> [String] {
        var out: [String] = []
        if let v = s.selected("video") {
            let w = v.width ?? 0, h = v.height ?? 0
            if h >= 1800 || w >= 3200 { out.append("4K") }
            else if h >= 1000 || w >= 1800 { out.append("1080P") }
            else if h >= 700 || w >= 1200 { out.append("720P") }
            else if h > 0 { out.append("SD") }
            if v.dvProfile != nil { out.append("DV") }
            else if s.gamma == "pq" { out.append("HDR10") }
            else if s.gamma == "hlg" { out.append("HLG") }
            let codecs = ["hevc": "H.265", "h264": "H.264", "av1": "AV1", "vp9": "VP9", "mpeg2video": "MPEG-2"]
            if let c = v.codec { out.append(codecs[c] ?? c.uppercased()) }
        }
        if let a = s.selected("audio") {
            if a.codecProfile?.localizedCaseInsensitiveContains("atmos") == true { out.append("ATMOS") }
            else if let c = a.codec {
                let codecs = ["truehd": "TRUEHD", "eac3": "DD+", "ac3": "DD", "dts": "DTS", "aac": "AAC",
                              "flac": "FLAC", "opus": "OPUS", "mp3": "MP3"]
                out.append(codecs[c] ?? c.uppercased())
            }
            if let n = a.channels { out.append(channels(n)) }
        }
        return out
    }

    static func channels(_ n: Int) -> String {
        switch n {
        case 1: return "MONO"
        case 2: return "2.0"
        case 6: return "5.1"
        case 7: return "6.1"
        case 8: return "7.1"
        default: return "\(n)CH"
        }
    }

    // mpv's OSC style: selected track id over track count, "-" when none is selected
    static func tag(_ s: OSDState, _ type: String) -> String? {
        let count = s.tracks(type).count
        guard count > 0 else { return nil }
        return "\(s.selected(type).map { String($0.id) } ?? "-")/\(count)"
    }

    static func language(_ code: String?) -> String? {
        guard let code = code, !code.isEmpty, code != "und" else { return nil }
        return Locale(identifier: "en").localizedString(forLanguageCode: code) ?? code.uppercased()
    }

    // a track panel row: its own title, else its language, plus what the title doesn't say already
    static func label(_ t: OSDTrack) -> String {
        let title = t.title?.trimmingCharacters(in: .whitespaces) ?? ""
        var parts = [title.isEmpty ? (language(t.lang) ?? "Track \(t.id)") : title]
        let said = parts[0].lowercased()
        if t.type == "sub" {
            if t.forced && !said.contains("forced") { parts.append("Forced") }
            if t.hearingImpaired && !said.contains("sdh") && !said.contains("hearing") { parts.append("SDH") }
        } else if t.type == "audio", let n = t.channels, !said.contains(channels(n).lowercased()) {
            parts.append(channels(n))
        }
        return parts.joined(separator: " · ")
    }
}

// Where things go. Down to a 0.6 scale the OSD shrinks with the window; below that it keeps 0.6
// and lays out on a wider design area, and under 1280 design px it drops the least needed items.
struct OSDLayout {
    let scale: CGFloat
    let width: CGFloat          // design size of the window
    let height: CGFloat
    let compact: Bool           // no bookmark/info keys, badges, meta line or SUBS/AUD words
    let margin: CGFloat
    let veil: CGFloat

    init(viewWidth: CGFloat, viewHeight: CGFloat) {
        let w = max(viewWidth, 1), h = max(viewHeight, 1)
        scale = max(0.6, min(w / 1920, h / 1080))
        width = w / scale
        height = h / scale
        compact = width < 1280
        margin = compact ? 48 : 90
        veil = compact ? 270 : 300
    }

    // tops of each row, from Jellyvin's VideoOSD.xml, kept at their distance from the bottom edge
    private func top(_ bottom: CGFloat, _ h: CGFloat) -> CGFloat { return height - bottom - h }
    var kickerTop: CGFloat { return top(compact ? 221 : 245, 22) }
    var titleTop: CGFloat { return top(compact ? 179 : 203, 35) }
    var metaTop: CGFloat { return top(179, 22) }
    var clockTop: CGFloat { return top(compact ? 186 : 230, 22) }
    var badgesTop: CGFloat { return top(168, 40) }
    var timesTop: CGFloat { return top(120, 24) }
    var trackTop: CGFloat { return top(129, 6) }
    var bladeTop: CGFloat { return top(121, 22) }
    var readoutTop: CGFloat { return trackTop + 17 }   // SEEKING ±m:ss: cap top 8 px under the grown track
    var nowLineTop: CGFloat { return top(48, 35) }     // the seek-only bar's NOW PLAYING line
    var keysTop: CGFloat { return top(32, 48) }
    var veilTop: CGFloat { return height - veil }
    var trackLeft: CGFloat { return margin + (compact ? 102 : 110) }
    var trackWidth: CGFloat { return width - 2 * trackLeft }
}
