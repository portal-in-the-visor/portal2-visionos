# Portal 2 VR for Apple Vision Pro (nxnative branch)

An experimental native VR port of Portal 2 (Nintendo Switch, v1.0.0) for Apple Vision Pro, built on [nxnative](https://github.com/seedds/nxnative). It runs as a native visionOS app with stereo rendering, head tracking, hand-tracked portal-gun aim and Joy-Con controls.

**No game files are included or provided.** This branch contains source code only, and works only with your own legally owned copy of Portal 2. Please don't post or ask for game files, keys or firmware.

Made by a hobbyist with AI coding agents (Claude and ChatGPT).

## Status

**This is not maintained.** I'm a hobbyist, not a developer, and I won't be updating it, fixing bugs, answering build questions or reviewing pull requests. It's shared as-is so others can learn from it or take it further. Feel free to fork it.

## Building

Build Portal 2 for iOS with nxnative from your own game, following nxnative's own instructions. This branch adds no steps of its own for that part. Then:

1. Generate the visionOS project from the build folder nxnative made:
   ```
   python3 dm/visionos/tools/gen_visionos.py <build folder>
   ```
   (needs `xcodegen`)
2. Open `<build folder>/xcode-visionos/DmVision.xcodeproj` in Xcode, choose your own signing team and bundle identifier, select the scheme "Portal 2 visionOS" and your Vision Pro, and press Run.
3. Put the game data into the app's Documents folder, as nxnative describes for iOS.
4. Launch the app, pair a controller, and press Enter VR. Allow hand tracking and world sensing when asked.

Only tested on one M2 Vision Pro.

## Controls

- Head: look around (the view follows your head; walking goes where you look)
- Right hand: aims the portal gun (point with your hand; the ring shows where portals go)
- Controller: stick to walk, triggers to fire, buttons as in the Switch game

## Tuning

An `env.txt` file in the app's Documents folder (one `KEY=VALUE` per line) changes settings at start:

- `DM_VR_FOV_H` / `DM_VR_FOV_V`: how much of the headset's field of view the game renders (default 0.65 / 0.85; lower is sharper but narrower)
- `DM_VR_AIM_DIST`: distance in metres where the hand's aim meets the crosshair (default 4)
- `DM_VR_HAND_AIM=0`: aim with your head instead of your hand
- `DM_VR_GUN=0`: keep the gun at the eye instead of in your hand
- `DM_VR_GUN_OFS=<forward>,<right>,<up>`: where your hand holds the gun (game units, default 16,8,-5)
- `DM_VR_CROSSHAIR=0`: hide the crosshair

## Known issues

- Heavier areas can drop below 60 fps, which shows as judder
- The glow ("electricity") on the portal gun is missing in VR
- Objects carried close to you, like Wheatley, can look squashed
- Menus and loading screens are shown on a flat screen in front of you
- The pause menu is shown flat

## How it works

The full technical log is in [`dm/games/0100ABD01785C000/README.md`](dm/games/0100ABD01785C000/README.md). In short: hooks in the game's `CViewRender::RenderView` and `C_BaseViewModel::CalcViewModelView` (`dm/host/hle_vrview.c`, registered in `dm/games/0100ABD01785C000/patches.py`) render each eye and place the gun. A CompositorServices immersive space (`dm/visionos/Sources`) shows the frames with head-pose reprojection and hand tracking.

## Credits and references

- [nxnative](https://github.com/seedds/nxnative) by seedds (MIT): the Switch-to-Apple native framework this is built on
- [portal2vr](https://github.com/Gistix/portal2vr): the PC Portal 2 VR mod; its approach to per-eye rendering and view angles was followed
- [source-sdk-2013](https://github.com/ValveSoftware/source-sdk-2013): Valve's engine source, used as a reference
- [alvr-visionos](https://github.com/alvr-org/alvr-visionos), [metal-spatial-rendering](https://github.com/metal-by-example/metal-spatial-rendering) and [Klepton](https://github.com/NSCabezon/Klepton): references for Vision Pro immersive rendering
- [vrperfkit](https://github.com/fholger/vrperfkit): reference for VR performance ideas

## Taking it further

This project won't be developed further here. If you want to carry it on, give your AI agent this branch plus the references above. Open areas: the gun glow, performance in heavy scenes, a floating panel for menus and the HUD, and foveated rendering.

Portal 2 is a trademark of Valve Corporation. This project is not affiliated with Valve, Nintendo or Apple.
