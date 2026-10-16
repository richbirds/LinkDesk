# LinkDesk icon

The application icon uses two linked desktops, pale sage and apricot screen fills, charcoal contours and a transparent alpha background. The mark occupies most of the canvas, without an opaque square tile. Master: `linkdesk.png` (1024 × 1024); native packages: `LinkDesk.icns` and `LinkDesk.ico`.

The master was edited with the built-in image generation tool and packaged with `scripts/generate_icons.py`, which preserves alpha and strips metadata.

Final editing prompt:

> Preserve the two linked computer monitors identity, pale sage green and apricot screen fills, thick crisp charcoal outlines, simple modern flat style, no text and no blue. Replace the entire ivory background with genuine alpha transparency. Enlarge and center the symbol to fill roughly 90% of a square canvas with a narrow safety margin. Simplify small details; no shadow, fake checkerboard or background tile.

Tray icons are rendered separately by `src/trayicon.h` at native sizes. They use enlarged, simplified monitor contours without the small connecting link or background tile. macOS receives a monochrome template mask for automatic light/dark menu-bar rendering. Windows receives a transparent sage/apricot mark.
