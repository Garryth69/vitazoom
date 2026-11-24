# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

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
