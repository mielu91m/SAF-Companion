
# SAF Companion

An add-on for SAF (Starfield Animation Framework) that makes animations easy to use: a selection menu, a mini-player with music, a camera mode and multi-NPC support — no console commands needed.

SAF Companion **does not play animations itself**. It calls SAF's native API (`SAFScript.pex`). Without SAF installed the mod will not work.

## Features

- Animation menu for the player and NPCs (list scanned from `Data\SAF\Animations\Control`).
- Mini-player: one player row + up to 6 NPCs, with prev/stop/next per row.
- Music: separate audio session, looping, with a volume slider (mutes game audio while playing).
- Camera mode: native tfc with movement blocked (looking around stays free) and a passive HUD.
- The mod stays inactive until you enable it with a hotkey — it does not listen for E outside the menus.

## Requirements

- Starfield **1.16.244** + SFSE (native free camera is verified on this version).
- **SAF** with `SAFScript.pex` (FBX animation playback).
- **OSF UI 2.0.0** or newer (menus and settings).
- **Cassiopeia Papyrus Extender**.
- Optional: **Modern Command Framework** — for the `safc` console command.

## Installation

1. Copy the contents of the package's `Data` folder into the game's `Data` folder (preferably via MO2).
2. Enable `SAFControlMenu.esm`.
3. Launch the game through SFSE.

Put animations in `Data\SAF\Animations\Control\*.fbx`. Put music (optional) in `Data\Music` — supported: `.mp3`, `.wav`, `.m4a`, `.wma`, `.aac`.

## Controls

Default keys (all rebindable in OSF UI → settings → **SAF Companion**):

| Key | Action |
| --- | --- |
| NumPad 5 | Player animation menu (enables Companion) |
| NumPad 6 | NPC animation menu |
| NumPad 4 | Stop all animations and disable the mod |
| NumPad 7 | Camera mode |

In the OSF UI settings you can also bind F1–F12.

### Player

1. NumPad 5 — the animation list opens.
2. Pick an animation — it starts on the player and the mini-player shows a player row.

### NPC

1. NumPad 6 — the animation list opens.
2. Pick an animation.
3. Walk up to an NPC and press **E** (as if starting a dialogue). The NPC starts playing.
4. Repeating this on another NPC adds another row (max 6). Choosing on a dancing NPC replaces its animation.

### Mini-player

- Click a row — makes it the active one.
- Per row: `◀` previous, `■` stop, `▶` next animation.
- Top section: previous track, play/pause, next track, volume slider (35% by default).
- Camera icon — camera mode; `X` closes the panel.

### Camera

NumPad 7 or the button in the panel. Works like a free camera: WASD blocked, looking around free, passive HUD on screen. Exit with the same key/button.

## Console command (optional)

Requires Modern Command Framework:

```
safc                     - list animations
safc list                - list animations
safc play <number>       - play on the player
safc play <number> ref   - play on the actor under the crosshair
safc play <number> <hex> - play on the actor with the given FormID (e.g. 14 = player)
```

## Notes

- After loading a save or changing location the mod resets the player state and closes the panels.
- If a panel fails to open on the first try (cold start of the WebView2 host), the mod retries opening it.

## Project layout

```
Plugin/src/          - SFSE plugin (C++): Papyrus API, music, camera, OSF UI bridge
do gry/SafCompanion/Data/
  Scripts/           - Papyrus scripts (SAFControlMenu, SAFCompanion)
  SFSE/Plugins/      - SAFCompanion.dll
  SFSE/Plugins/OSFUI - HTML/JS views (animmenu, mininpc, minihud, miniplayer)
  SAF/Animations/    - sample .fbx animations
```

Building: DLL with `xmake` (`build/windows/x64/releasedbg/SAFCompanion.dll`), scripts with `PapyrusCompiler.exe` using the Starfield flags.
