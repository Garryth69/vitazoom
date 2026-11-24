# VitaZoom

> **Beta.** It works, but it is still being tested. Use it at your own risk and report problems in *Issues*.

A taiHEN kernel plugin for PS Vita that lets you zoom into the screen **while playing**, with pinch, double tap or the right stick, while the buttons keep working normally.

## Features
- Zoom on top of any app or game, including Adrenaline (with any "Graphics Filtering" setting).
- **No FPS cost:** it uses the display controller *viewport*, the same mechanism as the system Accessibility zoom. The hardware scales the image.
- Smooth zoom on both axes: the view glides to every new position in sub-pixel steps, in fine zoom steps (about 4.6% each), up to 3x the screen.
- Pinch, drag and double tap on the front panel, optionally on the rear panel too, and right-stick panning.
- The view stays inside the picture: black bars around a 4:3 picture (PS1 games, emulators) never fill the screen.
- Overlay menu with global settings and **per-app profiles**.

## Compatibility
- Tested on a **PS Vita 1000 (OLED)** with HENkaku/Ensō and taiHEN.
- Adrenaline: tested on hardware with an Adrenaline 8.x build. The source of Adrenaline 7.0 and 8.0.2 was reviewed and uses the same video, controller and touch paths.
- PS Vita 2000 and PS TV: not tested.

## Installation
1. Download `vitazoom.skprx` from the [Releases](../../releases) page.
2. Copy it to `ur0:tai/` (with VitaShell, over FTP or USB).
3. Edit `ur0:tai/config.txt` and add the line under `*KERNEL`:
   ```
   *KERNEL
   ur0:tai/vitazoom.skprx
   ```
4. Restart the console (power off and on; resuming from sleep is not enough).

**Recovery:** if the console does not boot, hold **L** while it starts. HENkaku/Ensō then skips the plugins, and you can remove the line from `config.txt` with VitaShell.

## Usage
- **DOWN + SELECT** opens the menu, and the same shortcut (or **CIRCLE**) closes it. The shortcut can be changed. While the menu is open, the game gets no buttons or touch.
- In the menu, set **Zoom: ON** and close it.
- While zoom is on, front touch no longer reaches the game and is used for zooming:
  - **Two-finger pinch:** zooms in or out. The point between the fingers stays fixed, and moving both fingers also pans.
  - **One-finger drag:** pans the zoomed area.
  - **Double tap:** at normal size, zooms in on the tapped point (2x by default); while zoomed, goes back to the normal screen.
  - **Rear panel** (optional, see "Rear touch"): the same drag, pinch and double tap from the back of the console, so your fingers do not cover the screen.
- To give touch back to the game, turn zoom off in the menu or with the quick shortcut.

### Menu items
| Item | What it does |
|---|---|
| Zoom | Turns zoom mode on or off |
| Turn on when this app opens | Zoom is turned on when the app opens, at the zoom saved for it *(per app)* |
| One-finger touch | **zoom only:** touch does not reach the app. **to the app:** one finger goes to the app, with its position corrected for the zoom, and zooming uses two fingers *(per app)* |
| Move with analog stick | Pans with the right stick, always or only while holding L. While it pans, the game does not get the right stick *(per app)* |
| Pinch strength | How much zoom each pinch makes: soft (half), normal or strong (double) *(per app)* |
| Double tap zoom | Enables the double tap: zoom in at normal size, back to normal when zoomed *(per app)* |
| Double-tap zoom level | How much the double tap zooms in: 1.5x, 2x, 2.5x or 3x *(per app)* |
| Rear touch | While zoom is on: **to the app** (unchanged), **blocked** (hidden from the app) or **zoom gestures** (drag, pinch and double tap on the rear panel, hidden from the app) *(per app)* |
| Menu shortcut | Press X, then hold the new combination for 1 s (at least 2 buttons). CIRCLE alone cancels, and so does waiting 8 s |
| Quick zoom shortcut | Turns zoom on and off without the menu. TRIANGLE removes it |
| Save for this app | Saves the app profile, including the current zoom as its starting zoom |
| Save as default (all apps) | Saves the settings used by apps without a profile |
| Use default for this app | Deletes the app profile |

With "One-finger touch: to the app", the front panel zooms with two fingers only (no drag or double tap there); the rear panel is not affected by that setting.

While pinching or using the stick, an indicator shows the current zoom for 1.5 s. In apps that do not present frames by themselves (static screens, Adrenaline with "Original" filtering) the indicator is not shown, since it would stay burned into the picture or flicker.

### Per-app profiles
Each app (identified by its Title ID, shown at the top of the menu) can have its own profile. When you leave an app and come back, the plugin restores the zoom and position you left it at (up to 8 apps, until the console is turned off). Shortcuts are always global. In Adrenaline, all PSP games share the Adrenaline profile.

### Adrenaline
- Zoom works with any "Graphics Filtering" setting, including Original.
- With Original, the screen is the PSP video memory itself (480x272), which the emulator rewrites every frame. While the menu is open, VitaZoom shows its own frame (a dimmed still of the game with the full-size menu), and goes back to the game when it closes.
- The emulator reads touch through touch regions; VitaZoom reads all of them and merges the fingers, so gestures work on the whole panel.
- Inside the emulator, VitaZoom does not re-present frames during gameplay (only while its menu is open) and does not change the emulator's touch panel state.
- Avoid shortcuts that PSP games use (L+R, for example).

## Files
| File | Contents |
|---|---|
| `ur0:data/vitazoom/config.txt` | Global settings |
| `ur0:data/vitazoom/apps/<TITLEID>.txt` | Per-app profiles |
| `ux0:data/vitazoom/log.txt` | Log of the current session (the previous one is kept as `log.1.txt`) |

When reporting a problem, attach `log.txt` (or `log.1.txt` if the console froze and was restarted).

## How it works
- **Zoom:** the plugin sets the display viewport (`ksceDisplaySetViewportConf`), which magnifies part of whatever is on screen. The viewport glides to each new position (half of the remaining distance per frame, in 1/16 pixel steps). If the display refuses the viewport, it falls back to handing the display a cropped part of the app's framebuffer (in that mode horizontal panning moves in 64 px steps).
- **Touch:** Vita touch is only delivered to the focused process, so the plugin reads the panels inside the app's context, in the display hook. Each panel has its own gesture state, and only samples from the focused app that are newer than the previous one are used, which avoids fake touches.
- **Picture area:** twice a second while zoom is on, every 8th row of the frame is checked for black borders. A bigger picture area is taken at once; a smaller one only after it was seen four times in a row, so a dark scene does not restrict panning by accident.
- **Static screens:** apps that rarely present a frame are handled from the vblank wait: when something changed, the last frame is presented again.
- **Menu and indicator:** they use no image buffers; they are lists of rectangles and text (~9 KB), rasterised line by line while being copied to the screen. The exception is the menu's own frame on static screens (2 MB, in main RAM), which only exists while the menu is open.
- **Stability:** zoom only applies to the app it belongs to, is re-sent every second (so it comes back by itself after sleep), is never applied to the menu, and the menu closes by itself when PS is pressed. When the menu closes on a static screen, the plugin asks the display which frame it shows and presents the app's frame again until it is back. Nothing is written to the memory card from inside the video hooks.

## Known limitations
- Maximum zoom is 3x the screen (a viewport limit). In Adrenaline with Original filtering, up to 2x on top of the 2x the console already applies.
- If there is not enough memory for the menu's own frame, the menu is drawn over the app's screen instead and may flicker.
- The shortcut that opens the menu reaches the game for an instant before the block starts.

## Building
Requires [VitaSDK](https://vitasdk.org) with taiHEN installed.
```
cmake -B build && cmake --build build
# output: build/vitazoom.skprx
```
Host tests (gestures on both panels, zoom levels, picture area, settings, menu layout and rasterising) run with a regular C compiler, with AddressSanitizer and UBSan:
```
make -C tests
```
The embedded fonts are generated by scripts:
```
python3 tools/gen_font.py /usr/lib/kbd/consolefonts/default8x16.psfu.gz src/font.c
python3 tools/gen_bigfont.py NotoSansMono-SemiBold.ttf src/font_big.c 19
```

## Versioning
The project follows [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.
- Test builds carry a `-beta.N` suffix (for example `1.0.0-beta.1`, `1.0.0-beta.2`) and are published as pre-releases.
- After `1.0.0`: fixes bump PATCH (`1.0.1`), new features bump MINOR (`1.1.0`), and changes that require redoing the settings bump MAJOR (`2.0.0`).
- The version is shown at the top of the menu and on the first line of the log. Changes are listed in the [CHANGELOG](CHANGELOG.md).

## License and credits
- Code: [GPL-3.0-or-later](LICENSE).
- Large menu font: glyphs rendered from [Noto Sans Mono](https://github.com/notofonts/latin-greek-cyrillic), © The Noto Project Authors, [SIL OFL 1.1](LICENSE-OFL-1.1.txt).
- Small font: `default8x16`, distributed with [kbd](https://kbd-project.org/), GPL-2.0-or-later.
- Inspired by the overlay menu style of PSVshell and by hooking techniques used by community plugins (ds4vita, VitaControl). Built with [taiHEN](https://github.com/yifanlu/taiHEN) and [VitaSDK](https://vitasdk.org).

This project is not affiliated with Sony Interactive Entertainment. "PlayStation" and "PS Vita" are trademarks of their respective owners.
