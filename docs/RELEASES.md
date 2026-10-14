# Versions and publishing

## v0.7.0

Office streaming adds validated desktop move operations, conservative native dirty metadata, unchanged-frame suppression, independent cursor messages and lossless idle repair. Dynamic content can use hardware H.264 through Windows Media Foundation or macOS VideoToolbox, with image fallback and capability negotiation. TLS/TCP scheduling measures acknowledgements, encoding and capture queue delay without a fixed full-desktop bandwidth cap.

A synthetic five-scene benchmark compares legacy image encoding, office optimization and automatic video selection using identical input hashes and real loopback TLS. See [STREAMING.md](STREAMING.md) for scope and metric definitions. Windows build, regression tests and native hardware roundtrip are validated. macOS capture/codec additions require an Apple SDK build and cross-platform device testing before declaring Mac runtime support validated. LAN direct TLS needs an environment where the machine can receive private-address connections; a skipped environmental precondition is not a passing direct TLS result.

HEVC, 4:4:4 and a possible authenticated/encrypted UDP or WebRTC transport remain follow-up work, guided by capability and impaired-network measurements.

## v0.6.0

Incremental desktop updates, static-frame reuse, lossless changed regions, Windows DXGI capture with GDI fallback, Mac encrypted local password storage without Keychain, connection-list groups/thumbnails, single selected control window, and administrator-protected server settings.

The initial public source sync includes the light ivory/sage/apricot application icon. Private operating records, server configs and preconfigured application bundles are excluded.

## Future major feature versions

For versions such as 0.7.0 or 1.0.0, finish validation and privacy review before committing and syncing to the existing GitHub main branch. Patch-only changes need not sync unless requested. See AGENTS.md and run the staged-file publishing guard before every upload.
