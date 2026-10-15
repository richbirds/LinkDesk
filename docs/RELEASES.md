# Versions and publishing

## v0.9.0

Closing the main window now keeps connections running in the macOS menu-bar tray or Windows notification area. Tray Show restores the previous page, window state and owned windows. Tray Exit, the sidebar Complete Exit button and application quit requests use the same default-cancel confirmation. A second launch restores a hidden instance.

File cards display acknowledged transfer throughput in B/s, KB/s or MB/s, sampled once per second; reconnects reset the sample baseline. Hash workers now check cancellation between bounded reads. Confirmed process exit cancels work, flushes partial files, stops transports and removes the tray icon. A process-wide eight-second fallback terminates only an already-confirmed exit if native capture/codec cleanup stalls; normal exit and the fallback have distinct exit codes. No individual Qt thread is forcibly terminated.

Subprocess regression covers live TLS sessions, tray hide/restore, cancelled confirmation, explicit exit with an open viewer and file hashing, transfer-rate display, and simulated blocked cleanup. Actual user desktop sessions and file contents are excluded from public tests.

## v0.8.0

Compact navigation and device cards for smaller displays. Controlled desktops use the responsive local cursor without a delayed duplicate; viewing mode still shows the remote cursor. Device context menus add file selection and confirmed restart/shutdown requests, requiring host control permission for power actions.

Authenticated TLS file transfer supports drag and drop, per-card progress, SHA-256 verification, bounded chunk queues and resuming partial files after reconnect. Receivers use their configured folder (Downloads by default), preserve existing files, and offer a nonmodal folder-open prompt on completion. Re-selecting the same unchanged file can resume after restarting the application. Folder transfer is not supported. Both peers need v0.8.0 for these additions; the relay continues forwarding opaque TLS bytes.

General settings add per-user login startup and the receive folder. Startup registers the installed executable path; moving the application requires disabling and re-enabling this option. Login startup does not bypass OS login, screen recording, accessibility or remote control authorization. Windows power requests use a 30-second system countdown; macOS uses System Events and may require OS permission.


## v0.7.5

Fix Windows self-control freezing when a remote user presses LinkDesk's own minimize/maximize/close caption buttons. Native caption tracking can wait for a mouse release while preventing the GUI thread from processing the socket that carries that release. The input controller handles caption commands using the requested remote coordinates; a Windows native event filter also handles the application's caption presses and posts the matching system command on release, cancelling if released elsewhere. This covers remote, VM and local input paths. Button hit testing uses Windows title-bar accessibility rectangles, including Qt windows that report HTCAPTION for caption buttons. Other applications retain native input injection. A Windows integration regression sends separate press/release packets over TLS and checks continued image updates and metrics across repeated maximize/minimize cycles.

## v0.7.4

Only one interactive client may own a local device profile across installation/build folders. Reopening a running client activates its main window; a crashed owner releases its lock on the next launch. Packaging screenshot smoke runs use isolated settings and do not register a real device or reconnect saved desktops. A subprocess regression covers activation, duplicate startup and recovery after termination. A relay regression repeatedly expands incompressible lossless desktops through the forwarding window and verifies continued acknowledgements and telemetry. These changes address confirmed duplicate-process interference; the reported intermittent full-screen stall is not yet conclusively attributed.

## v0.7.3

Native unchanged-frame fast paths now return a reference repeat, so the three-second lightweight heartbeat also works with DXGI and ScreenCaptureKit. Capture metrics remain available while an encoder is busy and include encoder state and capture age. Optional LINKDESK_STREAM_TRACE logs numerical stream metrics locally for diagnosing stalls; this does not establish the cause of every reported disconnect.

## v0.7.2

Closing the main window now explicitly quits the application after stopping all connections and sharing. Remote windows can no longer keep an invisible process alive and lock the Windows installation folder. A subprocess regression checks actual exit with another viewer open and with Qt automatic quit disabled.

## v0.7.1

Fix idle desktop thumbnails incorrectly turning red after 15 seconds and blocking expansion. Hosts send a 19-byte reference repeat at most every three seconds; viewers also recognize live capture metrics with no unacknowledged frames from v0.7.0 hosts. Regression covers ten idle thumbnails, subsequent expansion and changed pixels.

## v0.7.0

Office streaming adds validated desktop move operations, conservative native dirty metadata, unchanged-frame suppression, independent cursor messages and lossless idle repair. Dynamic content can use hardware H.264 through Windows Media Foundation or macOS VideoToolbox, with image fallback and capability negotiation. TLS/TCP scheduling measures acknowledgements, encoding and capture queue delay without a fixed full-desktop bandwidth cap.

A synthetic five-scene benchmark compares legacy image encoding, office optimization and automatic video selection using identical input hashes and real loopback TLS. See [STREAMING.md](STREAMING.md) for scope and metric definitions. Windows build, regression tests and native hardware roundtrip are validated. macOS capture/codec additions require an Apple SDK build and cross-platform device testing before declaring Mac runtime support validated. LAN direct TLS needs an environment where the machine can receive private-address connections; a skipped environmental precondition is not a passing direct TLS result.

HEVC, 4:4:4 and a possible authenticated/encrypted UDP or WebRTC transport remain follow-up work, guided by capability and impaired-network measurements.

## v0.6.0

Incremental desktop updates, static-frame reuse, lossless changed regions, Windows DXGI capture with GDI fallback, Mac encrypted local password storage without Keychain, connection-list groups/thumbnails, single selected control window, and administrator-protected server settings.

The initial public source sync includes the light ivory/sage/apricot application icon. Private operating records, server configs and preconfigured application bundles are excluded.

## Future major feature versions

For versions such as 0.7.0 or 1.0.0, finish validation and privacy review before committing and syncing to the existing GitHub main branch. Patch-only changes need not sync unless requested. See AGENTS.md and run the staged-file publishing guard before every upload.
