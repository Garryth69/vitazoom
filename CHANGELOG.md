# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

## [1.0.0-beta.2] - 2026-11-24

### Changed
- Gestures: one finger only pans; double tap zooms in on the tapped point at normal size and goes back to normal while zoomed. The long-press zoom was removed.
- Zoom moves smoothly: the view glides to each new position in sub-pixel steps instead of jumping (smaller horizontal steps, especially in Adrenaline).
- "Block rear touch" became "Rear touch": to the app, blocked, or zoom gestures.

### Added
- Rear panel gestures (drag, pinch, double tap), with their own gesture state.
- The view stays inside the picture, so black bars around a 4:3 picture no longer fill the screen at the edges.

### Fixed
- Adrenaline: touch now works on the whole panel (all touch regions are read and merged); a pinch across two regions no longer turns into a drag.
- The menu could stay on screen after closing it on a static screen (Adrenaline with "Original" filtering): the plugin now checks which frame the display shows and presents the app's frame again until it is back.
- The shortcut capture could trap the menu: CIRCLE alone cancels it, and it gives up after 8 s.

## [1.0.0-beta.1] - 2026-11-24
First public release (beta).

### Features
- Zoom through the display controller viewport, on any app and in Adrenaline, with no FPS cost, and framebuffer cropping as a fallback.
- Gestures: pinch (adjustable strength), long press that zooms in step by step, one-finger drag, and double tap to reset.
- Panning with the right stick (always or while holding L).
- Overlay menu (DOWN + SELECT by default), quick zoom shortcut and per-app profiles.
- "To the app" mode: one finger goes to the app, with its position corrected for the zoom.
- The menu's own frame on screens the app does not present again (Adrenaline with "Original" filtering).
- Log at `ux0:data/vitazoom/log.txt`, with the previous session kept as `log.1.txt`.
