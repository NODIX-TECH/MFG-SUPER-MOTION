# Round 17 - OSM FG 1.1, Adaptive Motion Learning, the lists that go by each other

2026-10-11, evening. RTX 3060 + Intel UHD 730, 2560x1440 at 200 Hz.

## The report

1. Switching the generator: sometimes the name beside the switch changes and the box under it does not.
2. OSM FG: not good enough, not near Lossless Scaling in quality or in what it costs, and "its pixels come apart" - perhaps because it is not tied to HDR, VRR and the still pixels.
3. "Motion estimation" disappears with XeSS MFG and OSM FG.
4. A technique of the user's own to be added: Adaptive Motion Learning (the paper beside the app's sources). A switch under "Game type", a place in Settings that lists what is kept for each window and deletes it. Sentences on the screen asking the player to turn, if that helps; otherwise just learning for a minute. Not to be added if it is of no use. Nothing that works now to be harmed.

## 1 and 3 - one cause

Reproduced with `tests\dump\ui-switch.ps1` (the app's boxes picked through UI Automation): after "XeSS MFG" was picked the switch said XeSS MFG and the box still showed "DLSS MFG". The view model handed the "Generator" box a new list of the same three names on every change - also on the change the box itself was just making. The box took the new list in, dropped its pick, and showed the entry before.

So the two lists that go by each other ("Generator", "Preferred GPU") are now handed over only when their entries change, and their boxes are told their place once more after taking a list in (`MainViewModel.fs`, `refreshLists`, `notifyLists`).

That is also where "Motion estimation" went: the row was there for DLSS MFG and XeSS MFG, none for OSM FG - and with the box saying "XeSS MFG" while OSM FG was the generator, it looked as if XeSS MFG had lost it. OSM FG has its row now ("OSM FG", "OSM FG · Fast": its search in full or the lighter one, `osm_search`).

## 2 - OSM FG

**The user's sessions** (`tests\dump\user-logs-r17`): SDR, VRR and HDR off, x6, the engine on the RTX 3060 beside the game ("card 0 does both the generator and the source"), 25 to 38 real frames a second; a pair's work 2.1 ms on the card (OSM FG) against 2.0 (DLSS MFG). HDR and VRR are not what it was missing.

**What Lossless Scaling is set to on this PC** (`%LOCALAPPDATA%\Lossless Scaling\Settings.xml`, read, not changed): LSFG 3, fixed x6, flow scale 35, size "PERFORMANCE", capture DXGI, `PreferredGpuId` 1 - the Intel UHD 730. Its frames are made on the second card, so the game keeps the RTX 3060 to itself; ours were made on the game's card. That is the "far in performance" in the first place, and it is a setting: "Preferred GPU" in the app. What the UHD 730 then manages with OSM FG is below.

**"Its pixels come apart."** Not seen on the drawn street. Seen in numbers on a photograph moved a known way (`osmtruth photo`, new: a real picture's detail and grain): 400 to 650 pixels in every frame of 1.0 wrong by themselves, the four beside each right. Two rules of the last pass decided a pixel at a time; both are a place's or a mark's now (`docs\OSM-FG.md`, "What 1.1 changed"). 1.1: 22 to 70 such pixels.

**Quality on the street scenes**: 1.1 is 1.0's equal or better in most kinds of thing (read-outs 0.7% against 2.4% on drive 60, lamp posts 9.6 against 12.5, the dots of the bare wall 20 against 26), a little worse where a thing of its own crosses a fast camera turn (the car at tilt 32). The whole table is in `docs\OSM-FG.md`.

**Cost** by itself (`osmtruth bench=`): a pair of 1.1 with five frames is a quarter dearer than 1.0's - 2.8 ms against 2.2 on the RTX 3060, 26 against 21 on the UHD 730 (the marks, a pass over the whole picture a pair; and every place asked for the other word in the frames' colours, a frame). "Fast" on the UHD 730: 17.5 ms.

**In the engine** (the figures are in `docs\OSM-FG.md`): on the game's card 1.1 costs the game what 1.0 did (x6: the game at 67 or 68 of 119 frames a second either way). On the UHD 730 as the second card the game keeps 92 to 98 of 119 - and the chip makes 20 to 27 pairs a second at x6, 38 to 42 at x2, with what is shown 110 to 170 ms behind the game. Twice what the chip takes alone: Windows copies every frame of the game onto it and every shown frame back to the card the screen hangs on. So the two ways to run it, as things are:

| | the game keeps | pairs a second | shown | behind the game |
| --- | --- | --- | --- | --- |
| OSM FG x6 on the game's card (RTX 3060) | 57% | all the game draws (67) | 388 to 396 | 45 ms |
| OSM FG x6 on the UHD 730 | 77 to 80% | 20 to 27 | 101 to 110 | 140 to 170 ms |

What Lossless Scaling's LSFG 3 "performance" does on that chip at this size was not measured (it shows its rates only on the screen); the user's eye says it does better there than these figures. A generator made for a weak second card - less work a frame than a whole pass over the picture for each - is its own piece of work, not a setting.


## 4 - Adaptive Motion Learning

`docs\ADAPTIVE-MOTION-LEARNING.md` has all of it. In short:

- Added: the learning (the game's gain and delay from the mouse's counts or a pad's right stick against the picture's movement the engine already counts), what is kept per game, the switch and the list in the app, and one use - OSM FG is told where a fast turn took the picture. Measured on a photograph: at 260 pixels a frame 8.7% of the pixels off without it, 1.7% with; at 400, 19.8% and 5.1%.
- Not added: sentences on the screen (ordinary play teaches it in a quarter of a minute of looking around; the app says how far along it is), and moving the shown frames by what the hand did since they were drawn (the paper's "late warp": an edge with nothing to show, the read-outs to be kept, a tremble where the expectation is a little off - and nothing here to judge it with but a stand-in). It is the next step, as a switch of its own.
- DLSS MFG and XeSS MFG take no such word: with them the switch only learns.

## Left open

- OSM FG in a real game against Lossless Scaling by eye: the user's.
- A late warp from what Adaptive Motion Learning knows.
- OBS itself with the Spout sender (round 16) is still untried.
- A generator made for a weak second card (the user's Lossless Scaling runs on the UHD 730), and what LSFG does there, measured.

## Found on the way

- **The tests waited for a person who was not there.** `idle.ps1` reads Windows' "last input" clock; that evening it was moved four times a second by something that is neither the mouse nor the keyboard (no raw event of either in the same seconds) and never came to rest. `tests\quiet.exe <seconds>` listens for the two themselves - whether anything came, never what - and is the gate now (`tests\dump\r19-batch.sh`).
- **The same clock keeps a test window from taking the front**, and what is taken off the desktop itself (Desktop Duplication) then shows the window that started the tests: "DXGI capture paused: source is not foreground". The stand-ins borrow the front window's input for a moment (`tests\front.h`).
- **Round 16's one bad pair of the pan scene** was the scene, not the generator: two of its steps between two real frames (`tests\dump\panshift.ps1`).
- **Asked of brightness, the other word missed half its work** (tiles of different colour and the same brightness): it is asked of the frames' colours.
- **Offering a block its word of the pair before** made things worse and is not in.

## Tested

`tests\dump\r19-batch.sh`, `r19-final.sh`, on the engine built beside the real one: Adaptive Motion Learning in the engine on three scenes; the app's lists after nine picks with the advanced rows open (the switch's name, "Generator", "Motion estimation" and "Preferred GPU" agree after every one); `xess-smoke.ps1` 41 of 41 (five new: `osm-fast`, `osm-still-off`, `osm-aml`, `dlss-aml`, `xess-aml`); `suite.ps1` clean with each of the three generators; then the last changes (the other word in colours, "Fast" without it) on ten of the smoke cases and the suite with OSM FG once more.
