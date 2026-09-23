# Changelog

## [0.0.0] - 2026-09-06

### Added
- Added head tracking that moves the view. Your head turns, tilts and leans the
  camera; the mouse or controller still aims, and what the game shoots, traces
  and sees is exactly what it would be with tracking switched off.
- Added a crosshair that follows the aim rather than the view, so it stays on the
  thing the shot is lined up with when you look away from it. It is drawn on the
  point the aim line actually stops at, so it stays on that point when you lean
  as well as when you turn, at any range.
- Added camera collision, so leaning no longer puts the view inside a wall. The
  lean is cut to whatever the room leaves, the eye is held clear of the surface
  so the wall is still drawn, and the lean reopens smoothly as you step away.
- Added a diagnostics interval that writes what the camera, the aim line and the
  collision check each did on a frame, for working out why a crosshair or a lean
  is not behaving. It is off unless you turn it on.
- Added scaling of head tracking to the field of view the frame is actually drawn
  with, so a head turn is worth the same amount of screen while the game has the
  view narrowed, for a cinematic or the spyglass, as it is walking around. The
  reference is the field of view set in the game's own video settings, read live,
  so moving that slider mid-session lands on the next frame. Yaw, pitch and
  leaning are scaled; head roll is not.
- Added suppression of tracking outside gameplay: the shell, a level load, the
  pause menu, cutscenes, conversations, the journal, the power wheel, notes, the
  black market and the results and death screens all hold the view still.
- Added a build check that recognises the shipped executable and leaves the mod
  completely dormant on any build it does not know, so a game patch cannot break
  a running install.
- Added centring of the game window in windowed mode, once, shortly after launch,
  on the monitor it started on. Borderless and fullscreen are left alone, and
  moving the window yourself afterwards sticks.
- Added `LocalSmoothing` (default `0.0`) and `RemoteSmoothing` (default `0.15`)
  under `[Rotation]`. Which one applies is decided by the address the packets
  arrive from and re-evaluated every frame, so switching trackers needs no
  restart. Only `127.0.0.1` counts as local, so a tracker on this PC sending to
  the PC's own LAN address gets `RemoteSmoothing`. Both cover rotation and
  position.
- Added log rotation. Each launch renames the existing `HeadTracking.log` to
  `HeadTracking.prev.log` before opening a fresh one, so a crash report written
  on the way down survives the relaunch that follows it.
- Added the pose as the tracker sends it, with no centre of the mod's own, so
  the view is centred in your tracker app: OpenTrack's Center bind, a phone
  app's CENTER button, or the headset's reset view. There is no recentring
  hotkey.
- Added a settings file that is read once, at startup. A number has to be
  written whole and with a dot for the decimal point, and a hotkey as a code
  rather than a key name; anything else keeps the default and says so in
  `HeadTracking.log`.
- Added world-locked yaw as the starting yaw mode (`[Rotation]
  WorldLockedYaw=1`), matching the other head-tracking mods. `Page Down` (or
  `Ctrl+Shift+H`) switches to camera-local yaw in game.
