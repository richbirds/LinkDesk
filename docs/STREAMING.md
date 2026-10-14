# Streaming and synthetic benchmark

## Capture and office updates

Windows uses DXGI dirty rectangles and move metadata, falling back to GDI when duplication is unavailable. Metadata is used only when its previous frame matches the last committed reference. Dropped or resized captures trigger full pixel comparison. Every move is checked against current pixels before transmission; the receiver copies from an immutable previous image and then applies PNG patches. Generic scroll detection also handles platforms without move metadata.

macOS uses ScreenCaptureKit with a two-frame capture queue and cursor excluded from screen images. Dirty hints are used only when their coordinate scale is unambiguous; Retina or resized images use full comparison. Permission/startup failures are reported, and stopping sharing releases the native stream. The new Apple implementation still requires macOS build and device validation.

Unchanged full desktops send no picture packets. Cursor shape, hotspot and position have a separate negotiated channel. After 600 ms of idle time, a lossy reference is repaired with an exact PNG. Thumbnail quality settings deliberately avoid full-desktop cursor and repair traffic.

## Video and compatibility

The default automatic mode keeps office content on image deltas and selects hardware H.264 after sustained large changes. Users can select desktop or H.264 mode; lossless mode selects image encoding. Windows uses hardware Media Foundation transforms, NV12, no B frames and low-latency attributes. macOS uses hardware VideoToolbox, real-time encoding and no frame reordering. These settings are measured rather than treated as a 30 FPS guarantee.

Idle video switches back to a lossless image. Encoder failure falls back to images for the session generation; receiver decode failure disables video for that connection. Discarded video output resets the encoder so the next accepted stream begins independently. Optional Windows media components are loaded dynamically. `LINKDESK_DISABLE_H264=1` exercises fallback on both platforms.

Authentication negotiates `officeV2`, `cursorChannel`, `videoH264` and `frameMetrics`. Older clients keep the previous desktop packet format. Video uses a bounded `LDV1` envelope with resolution, sequence and Annex-B H.264 payload. Copy operations and PNG rectangles are bounds checked. All messages remain inside the existing authenticated end-to-end TLS connection.

HEVC and 4:4:4 are not advertised. They require separate capability negotiation, decoder support and tests before activation.

## Feedback and scheduling

Frame IDs match receiver ACKs. The sender pauses new capture when outstanding frames or socket queues become stale, and uses the latest source after recovery. Queue limits bound memory and delay; they are not a full-desktop transfer-rate cap. ACK delivery rate affects video bitrate only when delay and outstanding frames indicate congestion, because small typing updates do not measure available network capacity.

Metrics include transmitted frame bytes, actual encrypted socket writes, sent/decoded frames, encoding time, capture queue time, ACK RTT and receiver decode time. ACK RTT includes receiver processing, so it is not a pure network ping. Capture queue time runs from capture request until send, excluding measured encoding; it includes native acquisition work but not downstream kernel/router/display queues. Both smoothed live values and arithmetic means are available.

The current transport is TLS/TCP. This release does not establish weak-network head-of-line behavior or introduce UDP. Any later WebRTC/UDP work must preserve authentication and encryption and compare under controlled loss, delay and bandwidth.

## Reproduce the benchmark

Build with `BUILD_TESTING=ON`, then run:

```sh
desktop_benchmark --frames 90 --output benchmark.json
ctest --test-dir build --output-on-failure
```

On Windows the executable is `desktop_benchmark.exe`. In headless use set `QT_QPA_PLATFORM=offscreen`; make sure system fonts are available. Keep generated reports outside the public source tree or in an ignored build directory.

The five generated scenes are static desktop, typing, scrolling, window dragging and simulated video motion. Each mode uses identical precomputed 1280×720 pixels, quality 88, a requested 30 FPS source timer and loopback TLS/TCP. `inputDigest` verifies matching source sequences. Modes are `baseline` (legacy image codec), `office` (office changes) and `auto` (office plus automatic hardware video). The baseline uses the new shared transport scheduler; it is not an unmodified v0.6.0 application comparison.

Report `transportBytes`, `decodedFrames`, `meanEncodeMs`, `meanAckRttMs`, `meanCaptureQueueMs`, elapsed time and video/fallback counters together. Normalize bytes per decoded frame when frame counts differ. The initial image and 700 ms drain/idle repair are included. Static content intentionally produces one image rather than continuous presentation FPS.

This benchmark bypasses native screen capture and runs sender and receiver on one machine. It does not compare ToDesk, real desktops, native capture cost, physical display latency, macOS runtime or impaired networks. Device tests must use equal resolution, input scenes and network conditions and record the same metrics before claiming cross-device improvements.
