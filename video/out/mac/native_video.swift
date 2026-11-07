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
import AVFoundation

// Keeps the Vulkan (OSD) layer filling the view, on top of the native video layer, and places the
// video layer at `videoRect` (normalized, top-left origin) on every resize.
class NativeContainerLayer: CALayer {
    var videoRect = CGRect(x: 0, y: 0, width: 1, height: 1)
    var log: LogHelper?

    // Sublayers don't inherit the backing scale; MoltenVK sizes the swapchain from bounds * scale.
    override var contentsScale: CGFloat {
        didSet { sublayers?.forEach { $0.contentsScale = contentsScale } }
    }

    override func layoutSublayers() {
        super.layoutSublayers()
        let b = bounds, r = videoRect
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sublayers?.forEach {
            $0.contentsScale = contentsScale
            if $0 is MetalLayer {
                $0.frame = b
            } else {
                $0.frame = CGRect(x: b.width * r.minX, y: b.height * (1 - r.maxY),
                                  width: b.width * r.width, height: b.height * r.height)
            }
        }
        CATransaction.commit()
        log?.send(message: "Native video layout: bounds \(b) scale \(contentsScale) " +
                           (sublayers ?? []).map { "\(type(of: $0)) \($0.frame) hidden=\($0.isHidden)" }
                                            .joined(separator: ", "),
                  type: MSGL_DEBUG)
    }
}

// --macos-native-video: decoded VideoToolbox frames are shown by Apple's own video layer, so macOS
// does the Dolby Vision and HDR processing (as QuickTime and Infuse do). mpv's Vulkan layer above it
// then only draws the OSD and subtitles on a transparent background.
class NativeVideo: NSObject {
    unowned var common: MacCommon
    var log: LogHelper { return common.log }

    let container = NativeContainerLayer()
    let videoLayer = AVSampleBufferDisplayLayer()
    var visible = false
    var rect = CGRect.null

    init(common com: MacCommon, metalLayer: CALayer) {
        common = com
        super.init()

        let still: [String: CAAction] = ["bounds": NSNull(), "position": NSNull(), "hidden": NSNull()]
        videoLayer.videoGravity = .resize    // mpv computes aspect, crop and zoom itself
        videoLayer.isHidden = true
        videoLayer.actions = still
        container.log = log
        container.backgroundColor = NSColor.black.cgColor
        container.masksToBounds = true    // zoom/pan: the video layer may extend past the view
        container.actions = still
        container.addSublayer(videoLayer)
        container.addSublayer(metalLayer)

        // mpv does the timing: each frame is stamped "now" on a host-time clock running at 1x.
        // Without a timebase a lone frame (paused start, seek while paused) is never shown.
        var timebase: CMTimebase?
        CMTimebaseCreateWithSourceClock(allocator: nil, sourceClock: CMClockGetHostTimeClock(),
                                        timebaseOut: &timebase)
        if let timebase = timebase {
            CMTimebaseSetTime(timebase, time: CMClockGetTime(CMClockGetHostTimeClock()))
            CMTimebaseSetRate(timebase, rate: 1.0)
            videoLayer.controlTimebase = timebase
        }
    }

    // Called from the VO thread, at presentation time of the matching OSD frame.
    @objc func enqueue(_ pixelBuffer: CVPixelBuffer) {
        var format: CMVideoFormatDescription?
        var sample: CMSampleBuffer?
        var timing = CMSampleTimingInfo(duration: .invalid,
                                        presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
                                        decodeTimeStamp: .invalid)
        guard CMVideoFormatDescriptionCreateForImageBuffer(allocator: nil, imageBuffer: pixelBuffer,
                                                           formatDescriptionOut: &format) == noErr,
              let format = format,
              CMSampleBufferCreateReadyWithImageBuffer(allocator: nil, imageBuffer: pixelBuffer,
                                                       formatDescription: format, sampleTiming: &timing,
                                                       sampleBufferOut: &sample) == noErr,
              let sample = sample else {
            log.error("Native video: failed wrapping a decoded frame")
            return
        }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }

        if videoLayer.status == .failed {
            log.error("Native video: display layer failed (\(videoLayer.error.map { "\($0)" } ?? "unknown")), flushing")
            videoLayer.flush()
        }
        videoLayer.enqueue(sample)

        if !visible {
            visible = true
            DispatchQueue.main.async { self.videoLayer.isHidden = false }
        }
    }

    // Full (uncropped) video rectangle, normalized to the view, top-left origin.
    @objc func setVideoRect(x: Double, y: Double, w: Double, h: Double) {
        let new = CGRect(x: x, y: y, width: w, height: h)
        if new == rect { return }
        rect = new
        log.send(message: "Native video rect (normalized): \(new)", type: MSGL_DEBUG)
        DispatchQueue.main.async {
            self.container.videoRect = new
            self.container.setNeedsLayout()
            self.container.layoutIfNeeded()
        }
    }

    @objc func hide() {
        if !visible { return }
        visible = false
        videoLayer.flushAndRemoveImage()
        DispatchQueue.main.async { self.videoLayer.isHidden = true }
    }
}
