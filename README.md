# Dishonored 2 Head Tracking

![Dishonored 2 running with this mod](https://raw.githubusercontent.com/itsloopyo/dishonored-2-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for Dishonored 2 that moves the view with your
head while your mouse or controller keeps aiming, driven by a webcam, phone, or
any OpenTrack compatible tracker, with no VR headset required.

## Features

- **Decoupled look and aim** - your head moves the view; mouse and controller still control aim
- **6DOF positional tracking** - lean and peek with head position on top of yaw, pitch, and roll
- **Works with any OpenTrack compatible tracker** - free options available for PC, iOS and Android

## Requirements

- [Dishonored 2 on Steam](https://store.steampowered.com/app/403640/Dishonored_2/), legitimately owned, on the build of `Dishonored2.exe` timestamped 2025-02-06. On any other build the mod loads, writes a line into `HeadTracking.log` saying it is staying dormant and why, installs no hooks and touches nothing else, so the game runs vanilla. A build it does recognise logs `Build profile matched: steam-win64-20250206`.
- A tracking source that outputs the OpenTrack UDP protocol, such as [OpenTrack](https://github.com/opentrack/opentrack).
- Windows 10 or 11, 64-bit.

## Installation

### Lopari

Download [Lopari](https://lopari.app), choose **Dishonored 2**, and click
**Play with head tracking**.

### Standalone Installer

1. Download the latest `Dishonored2HeadTracking-v*-installer.zip` from the [Releases](https://github.com/itsloopyo/dishonored-2-headtracking/releases) page.
2. Extract the ZIP anywhere.
3. Double-click `install.cmd`.
4. Configure OpenTrack to output UDP to `127.0.0.1:4242`.
5. Launch the game.

If the installer cannot find your game, point it at the install folder
directly, either with a positional argument:

```powershell
install.cmd "D:\Games\Steam\steamapps\common\Dishonored2"
```

or with an environment variable:

```powershell
$env:DISHONORED_2_PATH = "D:\Games\Steam\steamapps\common\Dishonored2"
install.cmd
```

### Manual Installation

If you would rather place files by hand (or use the Nexus "extract to game
folder" ZIP):

1. Download `Ultimate-ASI-Loader_x64.zip` from [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases), take `dinput8.dll` out of it, rename that file to `winmm.dll`, and put it next to `Dishonored2.exe`. Do not leave it named `dinput8.dll`: `winmm.dll` is the name this mod's installer and its launcher manifest use, so keeping it matches the docs and the automated install. Skip this step if a loader proxy is already present.
2. Copy `Dishonored2HeadTracking.asi` and `HeadTracking.ini` into the same folder as `Dishonored2.exe`.

The Nexus ZIP contains only those mod files (no loader); the GitHub installer
ZIP bundles the loader for you.

## Setting Up OpenTrack

The mod listens for OpenTrack pose data on UDP port `4242`, on every network
interface. One datagram is six little-endian 64-bit floats in the order
`x, y, z, yaw, pitch, roll`: position in centimeters, rotation in degrees, 48
bytes in total. Anything that sends that to that port drives the view.
OpenTrack's **UDP over network** output sends exactly this, and the steps below
set it up.

1. Install [OpenTrack](https://github.com/opentrack/opentrack/releases).
2. Pick a tracker under **Input**, using the notes below.
3. Set **Output** to **UDP over network**, host `127.0.0.1`, port `4242`.
4. Press **Start**. Tracking and the game can start in either order.

### VR Headset Setup

1. Get the headset connected to the PC however it normally connects - a link cable, or whatever wireless streaming your headset uses. What matters is that SteamVR sees it.
2. Start SteamVR.
3. Set OpenTrack's **Input** to the SteamVR tracker.
4. Leave **Output** on **UDP over network**, host `127.0.0.1`, port `4242`.

### Webcam Setup

OpenTrack ships a `neuralnet tracker` input that reads a plain webcam. Select it
under **Input**, pick your camera in its settings, and use the output settings
above. How well it tracks depends on your camera and your lighting, so try it
before buying anything.

### Phone App Setup

A phone app can reach the mod directly, with no OpenTrack on the PC, if it sends
the datagram described above. Point it at this PC's IP address (run `ipconfig`
to find it) on port `4242`. Not every phone tracker speaks this protocol, so
check yours for an OpenTrack or UDP output option first. [Headcam](https://headcam.app)
sends it and filters on-device, so it can go direct; I made it so decent
tracking is free for anybody with a phone already in their pocket. Any app that
filters as much works the same way.

On-device filtering is what decides this. The mod's smoothing is sized to take
the edge off a clean signal rather than to rescue a noisy one, so a raw or
lightly filtered feed sent direct will jitter. The test is quick: try direct,
hold your head still, and if the view drifts or shakes, route it through
OpenTrack instead. Point the app at OpenTrack's **UDP over network** *input* on
some other port, say 5252, and let OpenTrack's filters and curves clean the feed
up before its output forwards to `127.0.0.1:4242`.

Anything arriving from outside `127.0.0.0/8` counts as a remote connection and
is smoothed with `RemoteSmoothing` rather than `LocalSmoothing`. That includes a
tracker on this very PC that sends to the machine's own LAN address, because the
mod reads the source address and not the machine.

### Centering

Centering is done in your tracker: OpenTrack's **Center** bind, the CENTER
button in a phone app, or SteamVR's reset view. Sit the way you play, look at
the monitor, and press it there.

## Controls

Two equivalent binding sets - use whichever your keyboard has:

| Action              | Nav-cluster | Chord           |
|---------------------|-------------|-----------------|
| Toggle tracking     | `End`       | `Ctrl+Shift+Y`  |
| Cycle tracking mode | `Page Up`   | `Ctrl+Shift+G`  |
| Toggle yaw mode     | `Page Down` | `Ctrl+Shift+H`  |

`Page Up` / `Ctrl+Shift+G` cycles tracking mode:

1. Normal head-tracked gameplay
2. Positional tracking disabled, rotational tracking enabled
3. Rotational tracking disabled, positional tracking enabled
4. Back to normal

## Configuration

Edit `HeadTracking.ini`, located next to `Dishonored2.exe`. It is read once, at
startup, so restart the game after editing it. Delete it and the mod writes a
fresh default copy on the next launch.

A trailing `; note` is fine on any line: everything from the first `;` or `#`
onwards is stripped before the value is read.

A number has to be written whole, with a dot for the decimal point.
`RemoteSmoothing=0,15` is refused and the default is kept, with a line in
`HeadTracking.log` naming the key and the text. So is a hotkey written as a key
name rather than a code: `Toggle=End` is refused, `Toggle=0x23` is not, and so
is a yes/no value that is not one of `1`/`0`/`true`/`false`/`yes`/`no`/`on`/`off`.

There is no field of view setting here, because the game has one. Dishonored 2
carries it on its own settings screen as `ark_fieldOfView`, 80 degrees by
default and adjustable from 65 to 110. The mod reads the field of view each
frame is actually drawn with and scales the head pose against the one you have
set, so the same head movement shifts the picture by the same amount whether the
game is zoomed or not. Move that slider mid-session and the mod follows it
without a restart.

This is the file the mod writes for itself, so what you get on disk matches it:

```ini
; Dishonored 2 Head Tracking config.
; Read once, at startup: restart the game after editing it. Delete this file
; and a fresh default copy is written on the next launch.
; A trailing comment is fine on any line: everything from the first ; or #
; onwards is stripped before the value is read.
; A number has to be written whole, with a dot for the decimal point, and a
; hotkey as a code rather than a key name. Anything else keeps the default
; and says so in HeadTracking.log, naming the key and what was written.

[Network]
; The UDP port the tracker sends to. OpenTrack's default is 4242.
UdpPort=4242

[General]
; Start with tracking switched on. End (or Ctrl+Shift+Y) toggles it in game.
EnableOnStartup=1

[Rotation]
; Per-axis multipliers on the pose the tracker sends. 1.0 is 1:1.
YawSensitivity=1
PitchSensitivity=1
RollSensitivity=1
; Flip an axis that tracks backwards.
InvertYaw=0
InvertPitch=0
InvertRoll=0
; Smoothing is chosen per connection and covers rotation and position. Which
; of the two applies is decided by the address the packets arrive from, so a
; tracker on this PC sending to a LAN address instead of 127.0.0.1 counts as
; remote. 0 = none, 1 = heavy.
; LocalSmoothing: packets from 127.0.0.1 on this machine.
LocalSmoothing=0
; RemoteSmoothing: packets from any other address.
RemoteSmoothing=0.15
; 1 = yaw turns about the world's up axis, 0 = about the camera's own.
; Page Down (or Ctrl+Shift+H) switches it in game.
WorldLockedYaw=1

[Position]
; Which mode the mod STARTS in: 1 is rotation and position, 0 is rotation
; only. Page Up (or Ctrl+Shift+G) cycles all three modes whatever this says.
Enabled=1
; Per-axis multipliers on the head position the tracker sends. 1.0 is 1:1.
SensitivityX=1
SensitivityY=1
SensitivityZ=1
; How far the eye may travel, in metres. LimitX and LimitY are symmetric.
LimitX=0.3
LimitY=0.2
; LimitZ is the forward lean and LimitZBack the backward one. They differ so
; that leaning back does not pull the eye into the player's own body.
LimitZ=0.4
LimitZBack=0.1
; Flip an axis that leans the wrong way.
InvertX=0
InvertY=0
; InvertZ is for a tracker that sends depth backwards, not for a lean that
; feels reversed. It is applied before the LimitZ / LimitZBack clamp, so
; turning it on also swaps the travel budgets to 0.10m forward and 0.40m back.
InvertZ=0
; Camera collision: stop a lean putting the view inside a wall. It runs the
; engine's own line check from the un-leaned eye toward where the head wants
; to go and cuts the lean to whatever the room leaves.
CollisionEnabled=1
; How far off a surface the eye is held, in metres. This has to be larger
; than the camera's near clip distance or the wall is not drawn anyway.
CollisionMargin=0.15
; Which bodies the check collides with, as the hex flag word the engine's
; own camera collision uses. 0 collides with everything the query allows.
CollisionChannel=0x800
; How quickly the lean reopens once the obstruction clears. Tightening is
; never smoothed. 0 = instant, 1 = very slow.
CollisionReleaseSmoothing=0.9

[Reticle]
; Look along the aim line for what the shot will hit, and draw the reticle
; on that point. Turn this off and the reticle marks the aim DIRECTION,
; which is exact when you turn your head and drifts when you lean.
AimTraceEnabled=1
; Which bodies the aim line stops on, as a hex flag word.
AimTraceChannel=0x800
; How far along the aim line to look, in metres.
AimTraceRange=200

[Diagnostics]
; Write a camera line to HeadTracking.log this often, in milliseconds, saying
; what the view was before and after the head pose, how far the aim line
; reached and what the lean clamp did. 0 turns it off. Use 250 while
; checking an axis or the camera collision, then set it back to 0.
CameraTraceMs=0

[Hotkeys]
; Virtual-key codes in hex, as in 0x23: the code itself, never the name of
; the key. Defaults are End=Toggle, PgUp=TrackingMode, PgDn=YawMode. The
; Ctrl+Shift+Y/G/H chord alternatives are always active whatever these say.
Toggle=0x23
TrackingMode=0x21
YawMode=0x22
```

## Troubleshooting

- **Mod not loading:** confirm `winmm.dll`, `Dishonored2HeadTracking.asi`, and `HeadTracking.ini` are all next to `Dishonored2.exe`, and check `HeadTracking.log` in the same folder. That log is rewritten from scratch on every launch; the previous launch is kept as `HeadTracking.prev.log`, which is the one to send if the game crashed and you have relaunched since.
- **No tracking response:** confirm your tracker is sending OpenTrack UDP to `127.0.0.1:4242`. Some trackers default to a different port; change it on either end.
- **Jittery or unstable tracking:** raise `LocalSmoothing` or `RemoteSmoothing` in `[Rotation]`. Which of the two is in force is decided by the address the packets arrive from, not by which machine they came from: only `127.0.0.1` counts as local, so a tracker running on this PC but sending to the PC's own LAN address gets `RemoteSmoothing`. `HeadTracking.log` names the one it picked. Wireless and webcam trackers especially benefit from a higher value.
- **View sits off-center:** center it in your tracker app. OpenTrack's Center bind, or the CENTER button in your phone app, zeroes the pose the mod receives.
- **Wrong rotation axis:** set the matching `Invert*` flag in `[Rotation]` if an axis tracks in the opposite direction.
- **Yaw feels wrong when looking steeply up or down:** press `Page Down` (or `Ctrl+Shift+H`) to switch yaw mode. World-locked yaw, the default, turns about the world up axis and keeps a sideways glance level at any pitch. Camera-local yaw turns the view about the camera's own up axis instead, so at a steep pitch a glance sideways spins the world rather than sweeping it. Set `WorldLockedYaw=0` in `[Rotation]` to start in camera-local mode.
- **Tracking feels stronger or weaker after a zoom:** it should not, and `HeadTracking.log` says why. The `[camera] zoom compensation basis:` line prints the field of view the frame was drawn with, the field of view set in the game's video settings, and the factor between them; the per-frame `[camera]` lines carry `zoom=` too. In ordinary gameplay that factor reads `1.0000`. A line saying `no zoom compensation from this frame` means the field of view could not be read, in which case the pose is applied at 1:1 and nothing else changes.
- **The view stops short when I lean into a wall:** that is deliberate. The mod
  runs the engine's own line check from your un-leaned eye toward where your head
  wants to go and cuts the lean to whatever the room leaves, holding the view
  0.15m off the surface so the wall is still drawn. It tightens instantly and
  reopens smoothly as you step clear. Raise `CollisionMargin` in `[Position]` to
  be held further off, or set `CollisionEnabled=0` to lean through geometry as
  before.
- **The crosshair does not sit where my shots land:** set `CameraTraceMs=250` in
  `[Diagnostics]`, play for a few seconds, then read `HeadTracking.log`. Each
  `[reticle]` line names what the crosshair was placed on - `impact` is the
  traced point the shot stops at, and anything else means the aim line reached
  nothing that frame and the crosshair is marking the aim direction instead. The
  `[trace]` lines next to them carry the same distance under `aim=`. Set it back
  to `0` when you are done; it writes several lines a second.
- **The crosshair sits slightly off the edge of a railing or a grille:** it marks
  the point the aim line stops at, which is the surface a bolt or a bullet stops
  at too. Where a piece of geometry is drawn in more detail than it collides,
  those are a few centimetres apart, and the crosshair is on the thing you would
  hit rather than on the drawn edge.
- **The crosshair marks a pane of glass in the way:** the aim line stops at the
  first surface it collides with, so at very close range it marks that rather
  than what is behind it.
- **The crosshair sits on the wall behind someone rather than on them:**
  `AimTraceChannel` decides which bodies the aim line stops on, and its default
  `0x800` is the flag the game's own camera collision uses against level
  geometry. Whether characters carry that flag too has not been confirmed in a
  running game. If a shot at a person marks the wall behind them, that is this,
  and the channel is the setting to change.
- **The game window moved when I launched:** in windowed mode the mod centers the window on the monitor it was already on, once, shortly after startup. Move it wherever you like afterwards and it stays put for the rest of the session. Borderless and fullscreen are left alone.

## Updating

Download the new release and run `install.cmd` again. Your config is preserved.

## Uninstalling

Run `uninstall.cmd`. This removes `Dishonored2HeadTracking.asi`, your
`HeadTracking.ini` and both log files. The Ultimate ASI Loader is only removed
if the installer put it there; use `uninstall.cmd /force` to remove it anyway.

## Building from Source

```powershell
git clone --recursive https://github.com/itsloopyo/dishonored-2-headtracking
cd dishonored-2-headtracking
pixi run build         # builds bin/Release/Dishonored2HeadTracking.asi
pixi run package       # produces installer + nexus ZIPs under release/
```

Requires Visual Studio 2022 (C++ workload) and CMake 3.20 or newer.

## Community & Support

- [Discord](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch of head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your phone into a head tracker

## License

MIT License - see [LICENSE](LICENSE) for details. Bundled third-party
components retain their original licenses, listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Credits

- [Arkane Studios](https://www.arkane-studios.com/) for Dishonored 2.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG.
- [OpenTrack](https://github.com/opentrack/opentrack) for the tracking protocol.
- [MinHook](https://github.com/TsudaKageyu/minhook).
- The [CameraUnlock](https://github.com/itsloopyo/cameraunlock-core) shared library.

## Disclaimer

This mod is not affiliated with, endorsed by, or supported by Arkane Studios or
Bethesda Softworks. Use at your own risk.
