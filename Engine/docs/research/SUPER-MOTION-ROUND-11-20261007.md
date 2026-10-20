# DLSS SUPER MOTION, round 11: five surfaces, the window's own frames

7 October 2026. RTX 3060 (SM86), 2560x1440 at 200 Hz, SDR. Measured on `tests\game.exe` and `tests\scene.exe`; nothing was run in THE FINALS.

## The report

THE FINALS: "very, very bad lag" some thirty seconds in; a hundred frames a second short of Lossless Scaling; the read-outs still not good. And a doubt: does the engine work on things that are not the game? It should take one window, alone, as its own. Windows' notices "ruin everything"; could the window be made full screen to be rid of them?

## What the session's log says

- The engine takes the desktop there, not the window: Desktop Duplication, and - unlike GTA V - through the compositor (the output's frames come back as desktop frames; the first second is twenty lines of "nothing new on screen" / "moved again").
- 76 to 100 pairs a second, three frames in between each (the count cut from five because they did not reach the screen): 320 shown.
- Once, for 2.9 seconds, the presenter did not come round at all, while 397 frames were taken; then "the pairs waiting to be shown left no room". Nothing in the log said where it stood.

## Three surfaces were two frames a refresh

The output's swap chain had three surfaces: one on the screen, one waiting for it, one to draw into. So the screen took two frames a refresh and no more - 400 a second at 200 Hz (round 8 measured the ceiling and took it for the path's own). Past it a drawing waits for a surface: frames come late and are passed over, the count a pair is cut to three and tried at four again every few seconds, the list of drawings on the card grows, and now and then the presenter stands behind it ("the presenter stood 806 ms in letting a pair's pictures go", caught once in a test).

A light game (185 FPS alone, the card three quarters used), x6:

| Surfaces | Pairs a second | Frames in between a pair | Shown | Present to screen |
| ---: | ---: | ---: | ---: | ---: |
| 3 | 86-97 | 3 (4 tried, 3 again) | about 350 | 7.6 ms |
| 5 | 81-84 | 5 | 487-498 | 1.5-1.8 ms |
| 6 | 83 | 5 | 495 | 1.5 ms |

Five it is (`lf_show.inl`; `SM_BUFFERS=<n>` to measure another number). A heavy game is as it was (x2 69/138, x4 58/230, x6 54/326), and its frames come more evenly: step error 0.31 ms at x2 (0.36), 0.21 at x6 (0.48), the 95th of 100 at 0.7 ms where it was 1.6 to 1.9.

Whether the 2.9 seconds of the report were this is not proven - it was not reproduced at that length. What goes with it is gone in the measurements, and the presenter now says where it stands: a wait of a fifth of a second is a `[stall]` line with its place, and a frame whose moment lies more than a quarter of a second ahead is shown at once instead of waited for.

## The window's own frames

Where the compositor draws the output (another program's window over it - an overlay), the desktop is the wrong place to take the game from: it is everything on the screen, a notice of Windows included, and every frame the engine shows comes back as one more desktop frame to copy, look at and throw away. So when that is seen (`lf_own_motion.inl`, SourceSeen) the source is opened again as the game's window by itself - Windows Graphics Capture, the "WGC" of the app's switch - once in a session (`SM_KEEP_DESKTOP=1` leaves it). Measured as fast as the desktop's frames (light game x6 83 pairs with five each, heavy x6 53-54), and the game need not be the window in front for it.

Not tested as a whole: the switching itself in a game the compositor draws over. `tests\overlay.ps1` does not make that state here (the engine hides its output under a window that covers the game); the reopening is the same one the app's switch does while running (the suite's `api-live`).

## Not done

- Notices of Windows are not switched off: an application has no documented way to, and a system setting is the user's to change (Settings, System, Notifications, "Turn on do not disturb automatically": when playing a game, when using an app in full-screen mode). With the window's own frames they are no longer in the picture the frames are made from.
- The output is not made an exclusive full screen: that takes the keyboard and the focus from the game.
- The read-outs: nothing new in this round.
