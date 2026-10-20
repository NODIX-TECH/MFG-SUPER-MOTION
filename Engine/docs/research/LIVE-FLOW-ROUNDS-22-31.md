# LIVE FLOW rounds 22-31 (from DLSS 5 MANAGER, SCREEN-INTEGRATION.md)

The working notes of the rounds in which the technology in this engine was built and measured: VRR and its self-check (22-23), Low Latency and the scheduling class (24-25), the source timer and capture pacing (27), DLSS MFG pacing and the in-frame FPS counter (28), still pixels (30), the window that fills its screen and repeated frames (31). Parts about DLSS 5 colours, FLOW MFG and Flow Super Resolution describe the other app and were not brought over.

## Round 22 — Denoiser, Enhance always on, FLOW Upscaler, VRR — 2026-09-27

**Processing card** rows: DLSS 5 | DLSS 4.5 FG; FLOW Upscaler | FLOW MFG; VRR |
Frame limit; Multi-pass | Denoiser; Sharpness | Edge smoothing. The Network
resolution slider is always enabled.

**Denoiser** (was Halo reduction; engine key still `halo`, so saved settings and
presets keep their value; `LF_HALO`, reshape flag 0x4 unchanged). One slider,
two halves:
- *Rim removal* (`halo_reduction.inl`) now runs at half the slider
  (`kHaloShare = 0.5`, applied once in `lf_pipeline.inl`), and the extra at a
  moving border is `gain = min(1, H * (1 + border))` instead of the old fixed
  jump to 60%. At full strength the old version flattened the network's local
  contrast along every edge.
- *Noise cleaner* (`denoise.inl`, new, `[denoise]` log tag): one pass on the
  finished composite, before sharpness/edges/colours (so sharpening never digs
  grain back out). Motion-compensated history clipped to the 3x3 mean
  ±1.5 sigma where the history can be trusted (still or slow), an edge-aware
  5x5 filter (colour distance under 0.03..0.085) where it cannot (fast motion,
  just uncovered), mixed by that trust; the shown result is next frame's
  history. 12x12 groupshared tile per 8x8 group. Own tmp + two history
  textures at the output size (closed with the pipeline).
- Cost measured on the RTX 3060 (1280x720 window, 2560x1440 output): network
  63.0 -> 62.6 frames/s with the Denoiser at 0.6.

**Enhance is always on** (`nr_small`): `session.py` starts with it on whatever
config.json says, `relaunch_now` never turns it off, a preset's `nr_small` is
ignored, the `boost` toggle is a no-op. The worker still reads `LF_NR_SMALL`.

**FLOW Upscaler** (`flow_upscaler.inl`, `[upscale]`, `LF_UPSCALER`, reshape flag
0x40000 with 0x20000 = display bits valid). Window capture only. The output
window and its swap chain take the whole screen the captured window is on
(`lfShow.bw/bh`; `lfShow.w/h` stay the picture's size, so the pipeline,
recording, screenshots, Spout and the comparison are unchanged); the client
area (title bar left out) is fitted, aspect kept, black bars. `ShowBlit` is
the last step before every Present - ordinary output, pass-through, FLOW MFG's
presenter, DLSS 4.5 FG's thread, HDR - and records: edge-directed Lanczos-2
(kernel stretched along clean edges, deringing to the 2x2 range; four bilinear
taps if the window is larger than the screen), then contrast-limited
sharpening at the screen size (RCAS shape, 0.4 x (scale-1)/0.5, 8-bit only).
Mouse: a pointer thread (2 ms) moves the game's window so the game pixel under
the pointer is the one shown there (`client origin = pointer - (pointer -
picture corner) / scale`, clamped to the client so a black-bar click still
lands on the game); the window goes home when the picture is hidden (the
game not in front - `UpscaleWantsOverlay`), the switch goes off, the source
changes or the worker exits (`atexit`). While it is on, FLOW MFG does not make
the window borderless, and turning it on returns a window FLOW MFG had taken.
Measured: pointer mapping within 0.25 game pixels at 2x; FLOW MFG x2 57.6 FPS
with it on, 56.6 off (same as the round 21 worker, 57.0) - no cost seen.

**VRR** (`lf_vrr.inl`, `[vrr]`, `LF_VRR`, reshape flag 0x80000): tearing-capable
swap chain (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, checked with
`DXGI_FEATURE_PRESENT_ALLOW_TEARING`), every Present passes
`DXGI_PRESENT_ALLOW_TEARING`, shown frames capped at refresh - refresh^2/3600
(`VrrCap`; the frame limit's `PaceCapture` only when nothing multiplies, FLOW
MFG's presenter caps its own shown rate). Toggling rebuilds the window. The
output window stays layered (click-through needs it - tested: a non-layered
cover swallows the click), so whether the screen really enters VRR is the
driver's call (NVIDIA: G-SYNC for windowed and full screen; AMD/Intel: Windows'
Variable refresh rate). State `vrr_refused` when the worker says tearing is not
possible.

**Engine log tags** added to `_KEEP_TAGS`: [denoise], [upscale], [vrr].
Test: `tests/screen_upscaler_vrr.py` (Denoiser, Enhance locked on, upscaler
geometry + pointer mapping + window home, VRR, FLOW MFG scaled, persistence).
`screen_smoke.py` now checks Enhance stays on. All LIVE FLOW tests pass (smoke,
FLOW, frame limit, borderless, FLOW VIEW, FPS counter, two screens, Radeon,
network+FLOW switch, recording, capture, presets).

## Round 23 — the shimmer, a Denoiser that cannot waver, visible sharpening, the Upscaler on the whole screen, VRR that checks itself, and the first two steps from Python to C++ — 2026-09-27

**The shimmer ("the screen waves like gas rising").** Two causes, both measured
on a still photo in a 1280x720 window (scratchpad r23_shimmer.py: mean change
between consecutive frames that reached the screen, 0..255):
- The round 22 noise cleaner pulled its own last output in through the motion
  vectors: sub-pixel noise in the vectors dragged the history around, and the
  feedback kept it going. Replaced (below).
- NVIDIA Optical Flow's field was never cleaned the way the engine's CPU field
  was: on a still picture it holds small random vectors, and the network follows
  them. With NVOFA, Denoiser off: 0.208; the CPU path: 0.091; round 21: 0.087.
  `MotionSettle` (lf_own_motion.inl) now does the CPU field's still check on the
  GPU after either source - vectors under 0.5 px set to none, and a vector kept
  only where it explains the change between the two frames (9x9 texels, every
  other one) better than standing still by half a level a sample. NVOFA,
  Denoiser off: 0.111; at the new 5% default: 0.080. The FLOW MFG football
  truth test is unchanged (ball 2.77 px).
- The finish pass's stability pull reads the history where it is when the
  vector is under 0.75 px.

**Denoiser v2** (`denoise.inl`, rewritten): no motion vectors and no feedback.
A pixel is averaged with the same pixel of the two frames before it (a ring of
the inputs as they came), weighted by how little its 3x3 changed there against
the noise level the slider sets (0.004 + 0.06 x slider); then an edge-aware 5x5
over pixels within 2.5x that level, half as strong where the time average
already worked. Default 5% (`HALO_START`), slider in 1% steps; saved configs
are moved to 5% once (`denoiser_v2`, `session._denoiser_once`, presets drop
their "halo").

**Sharpness** (post_finish.inl): CAS works on one pixel, too fine to see on a
1440p screen ("no difference at 100%"). A clarity lift one step wider (the
picture against an 8-tap blur of radius 1.5, on the luma, clamped to the
neighbourhood's range +-0.03) follows it; edge smoothing only halves the
sharpening on its edges now. Detail (mean |Laplacian|) on the still photo: 5.1
at 0%, 13.2 at 100%.

**FLOW Upscaler on the whole screen.** The person's log showed the switch
toggled with the whole screen as the source, where the round 22 upscaler (a
window only) had nothing to do. Now a render scale sits beside the switch
(77/67/58/50/33%, `upscaler_input`, reshape bits 20..22, `LF_UPSCALE_INPUT`):
with the whole screen captured, each captured frame is brought back to that
size (four bilinear taps over each texel's footprint) and rebuilt at the
screen's size with the same edge-directed Lanczos-2 and RCAS-shape sharpening
(`UpscaleReconstruct`, right after the capture, before the network, FLOW MFG
and the recording; with DLSS 5 off too). Window mode is unchanged.

**VRR** (lf_vrr.inl, rewritten from measurements, scratchpad vrr_probe*.cpp):
the output window reaches the screen through a hardware overlay plane in every
style, so a tearing present really could tear; and on the person's PC no
windowed program got a variable refresh at all (a plain focused full-screen
window presenting at 137 FPS landed on 5/10 ms steps of the fixed 200 Hz). So:
no ALLOW_TEARING any more; VRR = sync interval 1 + the cap under the refresh
(`ShowPresentNow`, every Present of the output window); toggling it no longer
rebuilds the window; and `VrrObserve` reads the frame statistics - once our
own frames are off the refresh grid, it says whether the screen's refreshes
follow them ("[vrr] the screen follows the frames" / "... stays at its fixed
200 Hz - VRR is not reaching it"), and the app shows what to switch on
(`vrr_state`).

**Python -> C++, steps 1 and 2** (the person asked for it a step at a time):
- *Scene cut*: the worker compares each capture's luma with the one before
  (`SceneCutNow`, the same measure and 0.24 threshold as motion_guides.py).
- *Motion*: frames flagged `kFrameOwnMotion` (0x2000) carry no field; the
  worker uses NVIDIA Optical Flow or, where there is none (AMD, `LF_OFA=0`), its
  own GPU estimate (`NetMotionRun`, flow_estimate.h's kernels in an instance of
  its own), then MotionSettle. The engine no longer takes, reads luma, runs
  OpenCV or sends 230 KB of motion per frame; only the "CPU" motion choice still
  goes through Python.

Log tags [motion] and [history] now reach Screen.log. Tests:
`tests/screen_upscaler_vrr.py` (adds the 5% migration, the still check, a real
scene cut, the whole-screen rebuild at 50/67%, the VRR verdict, and the own
estimate with LF_OFA=0); every LIVE FLOW test passes, FLOW truth unchanged.

## Round 24 — Flow Super Resolution, Low Latency, DLSS 4.5 FG out, FLOW MFG under 20 FPS, a faster start — 2026-09-28

All numbers are measured on the person's PC (RTX 3060, 2560x1440 at 200 Hz,
network share 0.5), by the tests named with them.

**Flow Super Resolution** (the switch was "FLOW Upscaler"; `flow_upscaler.inl`,
engine key still `upscaler`, reshape bit 0x40000, `LF_UPSCALER`). Round 23's
"render scale" (a capture shrunk to a game's render size and rebuilt at the
same size) cost quality and saved nothing - it is gone, with its combo
(`upscaler_input`, `LF_UPSCALE_INPUT`, reshape bits 20..22). Now two sizes are
kept apart: the *internal* size is the resolution slider's (the working size)
and the *output* is always the real display. With the switch on and the slider
under 100%, `PipelineBuild` makes the picture itself the internal size
(`pipe.super_res`, `display_w/h`): the capture is brought down once in the
channel-order pass's own command list (`SuperResDownscale`, a box over each
pixel's footprint - the engine's own pictures too, `FeedFrameSuperRes`), and
the motion, the network (never upscaling: NetworkCreate gets no screen size),
the Denoiser, the finish and FLOW MFG all run there. Only the output is scaled:
every Present (`ShowBlit` scales whenever the picture and the back buffer
differ - edge-directed Lanczos-2 + contrast-limited sharpening) and every
read-back (`SuperResExport`: the engine's screenshots/recording, and FLOW MFG's
recording export from its presenter, each with its own pair of textures) - so
those keep the display's size. A window source still fills its whole screen
(the pointer-following window); its placement is in captured pixels and is
converted to the internal picture. HDR kept as HDR (`LF_HDR`) keeps the switch
off. The app shows "1280x720 → 2560x1440" beside Network resolution.
`tests/screen_super_res.py`, whole screen, 1280x720 -> 2560x1440:

| | off | on |
|---|---|---|
| plain, ms a frame | 39.7-40.5 | 38.2-38.4 |
| FLOW MFG x3, base FPS | 18.3-18.7 | 20.6-20.8 |
| FLOW MFG, draw per shown frame | 6.9-7.1 ms | 4.1 ms |
| FLOW MFG x3, shown FPS | 47-53.5 | 61.2-61.8 |

Screenshot and recording come back at 2560x1440 in both modes. The network's
own time (it runs at the working size either way) is most of a plain frame, so
the gain there is small; FLOW MFG is where it pays.

**DLSS 4.5 FG is out of LIVE FLOW**: `lf_dlssg.inl` deleted, `nvngx_dlssg.dll`
no longer in `mod files/native` (game installs keep theirs, from
`mod files/streamline_dlss/dlss`), frame bits 0x100/0x200/0x800 no longer
sent, `frame_generation`/`frame_multiplier` gone from the engine, the app and
the saved config (`read_config` drops them). The old switch is now an unknown
setting. FLOW MFG is the only frame generator.

**Low Latency** (`lf_latency.inl`, reshape bit 0x100000, `LF_LOW_LATENCY`,
engine key `low_latency`), in DLSS 4.5 FG's old place: two buffers and
`SetMaximumFrameLatency(1)` (one frame queued, never three); the capture waits
on the frame-latency waitable object before it takes the picture
(`LowLatencyWait`, at most once per Present and a refresh and a half at most);
FLOW MFG keeps the median of the recent frame ages as its cushion instead of
the 75th percentile; the loop's thread runs above normal. And it is measured:
each shown frame's age from its source reaching the desktop (DXGI
LastPresentTime / WGC SystemRelativeTime) to the refresh that showed it
(GetFrameStatistics) - `[latency] desktop -> screen: avg, p95 ...` every two
seconds, `latency_ms` in the state, shown beside the switch.
`tests/screen_low_latency.py`: plain 44.2 -> 43.7 ms, VRR 43.6 -> 43.7 ms,
FLOW MFG x2 89.4 -> 84.7-85.3 ms. `LF_PHASE=1` (its `[timing]` lines now reach
Screen.log) says why the first two barely move here: Present -> screen is only
~2.5 ms on this PC (the output window is on a hardware overlay plane, no queue
builds), and ~34 ms of the 44 are the DLSS 5 network on the card; the Python
loop costs ~0.3 ms between frames.

**FLOW MFG under 20 FPS** (`DespeckleMain` in flow_shader.h, frames in between
only): where objects cross each other at a low frame rate their outlines break
into fragments. With the strength in gAccum for that dispatch (0 from 20 FPS
up, 1 at 12 and below, `[flow] ... below 20 the unsure edges ... are steadied`),
an unsure pixel on an edge that its neighbourhood disagrees with moves towards
the middle of the nine; sure pixels, flat areas, the HUD and everything from
20 FPS up are untouched. Football truth test (`--low-fps auto|off`, new metrics
`edges` and `sharp`): 30 FPS identical to round 23; 15 FPS speckle 0.508 ->
0.458, dribbler 14.69 -> 14.62, tag 14.09 -> 14.03, edges 21.42 -> 21.39, ball
2.77 -> 2.80 px; 10 FPS speckle -15%. A same-side average tried first changed
nothing measurable. The GPU probe sets the strength like the presenter
(`FLOW_PROBE_LOW_FPS`).

**A faster first start** (measured, `tests/screen_startup_profile.py`: launch
-> first state, and LIVE FLOW on -> first picture, from the worker's own
timestamps): before, 2.0-2.9 s and 1.70 s; of the 1.70, 0.70 s was the
Denoiser's shader compiling inside the first frame, on every start. Now:
- every compute shader goes through `CachedCompile` (`lf_shader_cache.inl`):
  one compile per process, the code kept in `%TEMP%\DLSS5Manager-shaders`
  (`LF_SHADER_CACHE`) named by a hash of source, entry, profile and flags, each
  file checked by a hash of its bytes;
- `ShaderWarmStart` (`lf_shader_warm.inl`) compiles what the run needs on
  background threads from the worker's first moment, while D3D12, NGX and the
  network come up (FLOW MFG's 17 kernels first when it is asked for) -
  `[shader] 15 shaders ready in 0.75 s (0 from the cache ...)`;
- the engine starts the worker as soon as its environment is complete
  (`processor.spawn_worker`, from `session.prepare` right after the screen is
  known - LF_OUTPUT is the last thing it reads) and does the machine and card
  probes while NGX starts; `launch_worker(..., spawned=...)` sends the hello.
Now 1.6-1.7 s and 0.80 s (FLOW MFG on, later starts: 0.84 s; its very first
start after an update compiles ~16 s of kernels in 3.1 s on six threads).
FLOW MFG's own `%TEMP%\DLSS5Manager-flow` cache stays as it was.

Tests: `tests/worker_hlsl_compile.py` (every worker shader through fxc, with
its compile time), `screen_super_res.py`, `screen_low_latency.py`,
`screen_startup_profile.py`; smoke/flow/upscaler-vrr/radeon updated (the Radeon
test now also runs Flow Super Resolution + Low Latency and checks the low-FPS
step at 15 FPS). Every LIVE FLOW test passes; Release 0 warnings.

## Round 25 — a lighter Super Resolution, Low Latency that measures before it chooses, FLOW MFG without the pulse, third-person heads, a page that stays put — 2026-09-28

Measured on the person's PC (RTX 3060, 2560x1440 at 200 Hz, network share 0.5)
by the tests named with the numbers.

**Flow Super Resolution at a fifth of the way.** Round 24 took the whole
picture down to the resolution slider's size and the person found it hurt the
picture. Now the network still runs at the working size (the Enhance path,
`pipe.net_small`), and the *picture* - motion, Denoiser, finish, FLOW MFG - is
only 20% of the way from the display's size to the slider's
(`kSuperResShare = 0.2`, `SuperResSize`: 2304x1296 on 2560x1440 at 50%; a
1280x720 window source at 640x360 work -> 1152x648). Output and read-backs are
scaled to the display as before (`ShowBlit`, `SuperResExport`). Log:
`[upscale] Flow Super Resolution: network WxH, picture WxH -> output WxH`; the
engine reads it into `super_res_size` and the app shows "work · picture →
screen" beside Network resolution. `tests/screen_super_res.py`: plain 40.8 ->
40.6 ms a frame, FLOW MFG x3 draw 7.1 -> 8.1 ms at 2304x1296, shown 42.9 ->
43.4 FPS; screenshot and recording at 2560x1440. At this share the switch is
gentle and costs nothing measurable - it no longer buys speed; the test checks
that FLOW MFG draws at the picture's size instead of "faster".

**FLOW MFG with NVIDIA Optical Flow.** Nothing was removed: on RTX the motion
is still "GPU · NVIDIA Optical Flow" (the GPU estimate with NVOFA as a
candidate). `tests/screen_flow_ab.py` (two worker builds through the real
engine, the stand-in game on the card), round 23 against this round: parity
within the run-to-run noise (±3 px on the ball).

**The pulse** (FLOW MFG running, then stopping, then running) had two causes,
both measured with the new cadence figures (`[flow] cadence: ... a frame in
between costs X ms, Y% late`; `frame_cost` counts in-between frames only,
`missed_smooth` the late share):
- the under-20-FPS edge step switched on and off every few frames around 20
  real FPS: the rate is now smoothed (`real_fps_smooth`, EMA 0.15) with
  hysteresis - on under 19, off over 21;
- a game on the same card: at Windows' normal GPU scheduling class the frames
  in between cost 8-11 ms and 15-40% came too late to be shown; with Low
  Latency's realtime class 2.2 ms and 4-8%. A planner that cut the steps when
  frames came late was tried and removed (it oscillated without fewer late
  frames).

**Low Latency, round 25** (`lf_latency.inl`):
- each new game frame is processed once: the loop's "nothing new" skip also
  applies under Low Latency (`lf_loop.inl`), and WGC waits up to 8 ms for a
  frame instead of answering at once (the engine would otherwise spin);
- a picture's age is counted once, at its first Present (`last_source`) - a
  repeated picture inflated the figure;
- FLOW MFG's cushion is the median only while under 5% of its frames are late,
  the 75th percentile otherwise;
- **the card's scheduling class is measured, not assumed**
  (`D3DKMTSetProcessSchedulingPriorityClass`; D3D12 queue priorities and the
  classes below realtime change nothing). `LowLatencyChoose`: when the switch
  goes on, one [latency] summary at the normal class, one at realtime (the
  first after each change passed over), realtime kept only when quicker by 3 ms
  and 8% - `[latency] Low Latency measured the card's scheduling classes: normal
  X ms, realtime Y ms - ... kept`. Measured again when the age drifts
  (1.35x + 5 ms, twice, 15 s after the last choice) and every two minutes.
  Why: with nothing else on the card realtime *cost* 19.6 ms (plain) and 22 ms
  (FLOW MFG x2) on the football fixture - found by switching each part off in
  turn: without the class Low Latency was -0.2 / -3.3 ms, without the skip
  still +18 ms. Under a game it is the whole gain.

`tests/screen_low_latency.py` (football fixture, nothing else on the card; the
test now waits for the choice): normal kept every time (realtime measured
62.5 / 64.2 / 119 ms) - plain 45.5 -> 45.0 ms, VRR 45.3 -> 44.7, FLOW MFG x2
90.2 -> 88.8. `tests/screen_latency_load.py --coupled` (the stand-in game
`tests/gpu_load.cpp`, 60 FPS keeping ~10 ms of the card each frame, is the
source): realtime kept (normal 51.5 ms vs 16.2; FLOW x4 136.6 vs 55.3):

| | Low Latency off | on |
|---|---|---|
| plain: latency avg / p95 | 46.1 / 58.3 ms | 15.9 / 20.0 ms |
| plain: processed FPS | 27.5 | 51.6 |
| plain: the game's FPS | 60.7 | 48.5 |
| FLOW MFG x4: latency | 131.8 ms | 55.9 ms |
| FLOW MFG x4: real / shown FPS | 17.7 / 44.8 | 29.0 / 105.2 |
| FLOW MFG x4: the game's FPS | 60.8 | 34.3 |

The price is the game's own frame rate when realtime is kept - the person sees
LIVE FLOW's frames, not the game's. The Radeon build runs the same choice
(`tests/screen_radeon.py`: normal kept, 67-70 ms either way).

**VRR** carries a BETA badge like FLOW MFG's.

**The LIVE FLOW page scrolled itself** ("it goes down on its own when I turn
something on"). Found with a temporary trace of every bring-into-view request
and scroll change: Avalonia 12's `ScrollViewer.OnGotFocus` brings the focused
control into view, and focus comes back on its own whenever the window is in
front again - after the output, the game or FLOW MFG's borderless window had
the front, the last bar or list touched got focus again and the page jumped to
it (24 px in the reproduction; as far as the control is, in general). Round 4's
handler sat on the UserControl, but the page's ScrollViewer is inside it: the
bubbling request reaches the ScrollContentPresenter first, so the handler never
stopped anything. Now `BringIntoViewOnFocusChange="False"` on
`ScreenScrollViewer` and the request is handled on `ScreenRoot`, the scroll
content; the DLSS 5 switch's two buttons are `Focusable="False"` like the
others. Lists open in their own popup and still scroll to their item.

**Third-person games: the head against the world.** `tests/third_person_fixture.py`
is a small world the camera orbits (sky, mountains, trees and lamp posts, a
tiled ground, each with its own parallax) with a character in the middle -
spiky hair against the sky and the trees - and exact masks of the head and the
body at any fractional tick. `tests/test_flow_thirdperson_truth.py` scores
frames in between against the true picture (`head_out` the ring around the
head, `head_in`, `body_edge` the band around the outline, `body`, `scenery`,
`bad`, `sharp`; `--fps`, `--motion nvofa|gpu|gpu-nvofa`, `--baseline`,
`--dump`). Where the scenery streams past a character that stands still, a
pixel whose own candidates find nothing sure (`best.w < .5`), that is not on a
crossing and not itself beside the still layer, takes the scenery's motion
(`MovingScene`: the vector median of moving votes on rings of 24 and 48 px)
when exactly one end of that path lands on the still layer - the picture comes
from the end that is scenery. Nine variants were tried; the ones that decided
earlier (in Follow) painted scenery onto the character. `body_edge` in the
app's RTX mode (`--motion gpu-nvofa`): 20 FPS 8.00 -> 6.57, 15 FPS 8.62 ->
7.23; GPU estimate only (AMD): 8.22 -> 6.88; the head ring moves only a little
(5.16 -> 5.10). Football unchanged.

Tests: new `tests/gpu_load.cpp` (built by `tests/artifacts/tools/build_load.cmd`),
`screen_flow_ab.py`, `screen_latency_load.py`, `third_person_fixture.py`,
`test_flow_thirdperson_truth.py`; updated `screen_bench.py` (`--low-latency`,
`--upscaler`, `--keep-window`), `screen_upscaler_vrr.py`, `screen_super_res.py`,
`screen_low_latency.py`, `screen_radeon.py`. Every LIVE FLOW test passes on the
final build; Release 0 warnings.

## Round 26 — FLOW MFG's performance mode, what it costs measured kernel by kernel, and a lighter full drawing — 2026-09-28

The person: turning FLOW MFG on costs the game a lot of frames and then it
lags; they want a performance mode like Lossless Scaling's ("7 frames spent to
make 60, not 20") and better frames. Measured first, on their PC (RTX 3060,
2560x1440).

**What FLOW MFG costs** (`tests/flow_cost.py`: one pair of the third-person
world at the output size through the GPU probe with `FLOW_PROBE_TIMING`, every
dispatch repeated and timed; the probe now warms the card up first - without
that the first stage took the clock ramp, 6.7 ms for a 4-sample kernel - and
sizes the estimate like the worker, `FLOW_PROBE_EST_CAP`; `FLOW_PROBE_FLAGS=0`
compiles as the worker does, which made no difference). At 1440p, motion
1024x576: per pair 5.3 ms (the estimate 3.0, of which FlowFinal 0.8; tracker +
block search 1.7, skipped from 40 FPS up; HUD + FixMain 0.45), per frame in
between 2.1 ms, of which the crossing search was 1.1. x4 at 60 real FPS: more
than half the card.

**Performance mode** (`FastMain` in flow_shader.h, `g_flow.fast`,
`LF_FLOW_FAST`, reshape bit `kReshapeFlowFast` 0x800000, engine setting
`flow_performance`, **on by default**): a frame in between follows only the two
motions (current -> previous, previous -> current, each to both frames) and
takes the better matched - no neighbour candidates, crossing search or
third-person rule; the HUD, still places and HUD edges as in the full drawing;
DespeckleMain after it as before. Per pair: the estimate stops at 400x225
(`EnsureFlowEstimateTargets`), no tracker or block search at any rate, no
RangeMain/ReachMain. FLOW VIEW always shows the full drawing. Switching it
rebuilds FLOW MFG (`EnsureFlow` compares `fast`).

| 1440p, RTX 3060 (`tests/flow_cost.py`) | full drawing | performance |
|---|---|---|
| per pair, 60 real FPS | 3.2 ms | 2.0 ms |
| per frame in between | 2.0 ms | 0.63 ms |
| x3 at 60 real FPS | 43% of the card | 20% |
| x4 at 60 real FPS | 55% | 23% |
| x4 at 30 real FPS | 41% | 13% (no tracker) |

Through the engine (`tests/screen_flow_performance.py`, football on the
desktop, x4, DLSS 5 off): a frame in between 3.60 -> 1.46 ms, shown 164 ->
179-199 FPS, late 31% -> 0-24%. With the stand-in game and the network on
(`screen_latency_load.py --coupled`, x4): FLOW MFG shown 49.5 -> 76.4 FPS, real
18.7 -> 22.6, latency 126 -> 98 ms. With Flow Super Resolution on
(`screen_super_res.py`, network on): a shown frame drawn in 1.9 ms where round
25's full drawing took 7.2.

What it gives up (truth tests, `--fast`; 15 FPS football / 20 FPS third
person, RTX motion): ball 2.28 -> 8.0 px (the crossing search is what keeps a
small fast object), players 9.1 -> 11.4, name tag 14.1 -> 18.8, HUD 1.19 ->
1.64, speckle .35 -> .41; third person: the band round the character 6.57 ->
7.32, scenery 1.51 -> 2.37, the head's inside better (2.76 -> 2.05). Showing the
last real frame instead (no generation): players 28, scenery 9.3. Tried and
left out, measured: deciding at half size and drawing at full
(DecideMain/ComposeMain, 2x2 or 4x4 cells) kept more of the quality but cost
1.1-1.3 ms a frame - the decision's floor did not shrink with the cells -
and the crossing search capped to 1-2 rings saved little.

**The full drawing, lighter at no cost to the picture** (the truth tests give
exactly round 25's numbers):
- `RangeMain` + `ReachMain` once per pair after FixMain: every motion the
  crossing search reads is a bilinear mix of the repaired field within its
  reach, so where that range is narrower than its threshold (with a quarter
  pixel to spare) nothing can pass it - `SearchMayFind` skips the search there
  (pans, still or slow scenes).
- `HudTileMain` once per pair: a map of where a HUD verdict is (8x8 pixels,
  one pixel around); `Layered` - nine reads for every pixel - runs only there.
- The third-person rule reads its neighbourhood only for a pixel it can take.
- Draw is now `HudLayer` + `Decide` + `Finish` (shared with FastMain);
  `FollowSide` says which end a colour came from.
The drawing 2.08 -> 1.86 ms on the parallax world, more where the search is
skipped.

gTiles now holds, after the tracker's and the block search's records, two per
tile of motion ranges and one per 8x8 pixels of HUD map (`FlowTileRecords`,
`RangeAt`, `HudTileAt`). The probe binds gTiles to CSMain and runs the new
kernels when the shader has them (`FLOW_PROBE_FAST=1` for FastMain,
`FLOW_PROBE_SKIP_SEARCH`, `FLOW_PROBE_NO_SPECKLE`).

The app: a **FLOW MFG mode** row under FLOW MFG - Quality | Performance - as
wide as its switch and multiplier; strings `lf_flow_mode*` (en, ar; the rest in
`languages to add.JSON`).

**Installs, same round:** RenoDX's Multipass is on by default wherever it
applies (one-click sheet: `ApplyMultipassDefault`, the user's own switch
stands until the sheet is opened again, an installed route shows what it
carried; advanced sheet and presets: `RenoDxBuild = "multipass"`). The AMD
route's proxy (version.dll unless taken) can be renamed from the DLL NAME card
with OptiScaler's names (`AmdProxyDll`, `amdProxyNames`; recognised by the
`dlssnr_on_amd` its code names, as the file has no version resource; the
health check and the advanced sheet's parts find it under any name). The
translations the person returned (130 strings, 15 languages) are merged: all
17 languages complete before this round's five new strings.

Tests: new `tests/flow_cost.py`, `tests/screen_flow_performance.py`,
`advanced_install.fsx amdname`; `worker_hlsl_compile.py` (29 entry points),
both truth tests `--fast`, the probe as above. Every LIVE FLOW test passes;
Release 0 warnings.

## Round 27 — the source timer that never took a window, generators that showed less than the game alone, and the counter checked — 2026-10-02

After the selected-window / DLSS MFG work of 2026-10-01/02 (LIVE-FLOW-*.md).
Measured on the person's PC (RTX 3060, 2560x1440 at 200 Hz) with a stand-in
game that reports its own rate (`tests/gpu_load.cpp`).

**"Select active window" never selected anything.** `other_app_in_front()`
(windows_api.py) refused every window *with* a taskbar button - every game and
every ordinary application; the test was the wrong way round (`windows_to_offer`
has it right). The expiry had only ever run against a mocked foreground window,
so nobody saw it. Now a window with a taskbar button is taken (a tool window's
owner in its place). And when the timer, or a pick from the list during it,
switched the waiting generator on, it set the setting directly
(`processor.set_flow`) and never sent it to the worker: the switch showed on,
`running_multiplier` stayed 0 and the log repeated "FLOW MFG starting". Both
places go through `actions.do_action((generator, True))` now. A generator saved
as on no longer starts a countdown by itself when LIVE FLOW comes up (five
seconds later it would have taken whatever was in front); it is remembered
(`_source_pending_generator`, saved as on) and starts when a window is picked.
`tests/source_timer_real.py` does it for real - a click on the fixture, the
countdown runs out: taken, borderless over the monitor, FLOW MFG generating,
frame and place back when it goes off. Also run in the app itself.

**The Source card:** the DLSS MFG resolution list had no `Grid.Column` and sat
on its label.

**The counter is true.** `tests/screen_counter_truth.py` sets `live_fps` /
`display_fps` against the game's own rate: 40 FPS game -> base 40.0-40.2, shown
79.8 / 119.0 (FLOW x2 / x3) and 80.1 / 120.2 (DLSS x2 / x3). It never showed
more than the game drew or the screen got. What it showed in a heavy game was
the truth about three faults:

1. **No pacing to the screen.** With no frame limit nothing tied the pictures
   taken to what the screen can show. A 200 FPS game on the 200 Hz screen: FLOW
   MFG took 114 a second, showed 72 real + 40 generated and dropped ~40 pairs
   (116 shown, uneven); DLSS FG presented 263. `PaceCapture` now spaces the
   pictures at 97% of the refresh rate divided by the multiplier (never under
   30 a second), counted from the last new picture (`PaceTaken`) - a fixed grid
   lost every slot a slightly slower game left empty (60 FPS taken at 52).
2. **The card's scheduling class.** Measured only with Low Latency on, and
   forced to normal for DLSS FG. At the normal class under a game that fills
   the card (130 FPS, 5 ms of the card each frame) FLOW MFG got 38 real frames
   a second and showed 63 - the "base 36, output 76" seen in GTA - and DLSS FG
   62 -> 124, fewer than the game alone. The class is now measured whenever a
   generator runs, for both, and chosen by real frames shown (`g_lat.reals`,
   counted at each real Present; the latency ring undercounts under DLSS FG):
   realtime when it shows 8% more, or as many with clearly less delay.
3. **Frames in between thrown away at the refresh rate.** The presenter dropped
   a frame more than 35% of a slot late; the back buffer comes free once a
   refresh, so with a slot as long as a refresh a third were dropped that would
   have been shown on the very next one. Nearly one refresh is allowed now
   (`late_by`).

| game (alone) | generator | before: base / shown | after: base / shown | the game with it |
|---|---|---|---|---|
| 130 FPS, heavy | FLOW x2 | 37.8 / 63.5 | 74-76 / 138-144 | 78-83 |
| | DLSS x2 | 63.5 / 126.8 | 84-88 / 167-175 | 96-98 |
| | DLSS x3 | - | 62 / 186 | 102 |
| | FLOW x4 | - | 45 / 161 | 98 |
| 200 FPS, light | FLOW x2 | 74.8 / 116 (40 pairs dropped) | 93-96 / 161-170 | 189-193 |
| | DLSS x2 | 131.6 / 263 | 96-97 / 192-194 | 200 |
| 60 FPS | FLOW x2 / x3 | - | 58.6 / 116, 57.8 / 157 | 60 |
| | DLSS x3 | - | 59.5 / 178.5 | 60 |

Still open: FLOW MFG shows x1.8-1.9 of a x2 at the screen's limit (10-20% of
its frames in between still late); under realtime the game itself runs slower
(130 -> 80-100) - the price of the generator, and what makes its cost per frame
the next thing to cut; a recording made with FLOW MFG on holds fewer frames
than it should (`screen_record_flow.py` fails the same way on the worker from
before this round); no physical HDR screen, no RTX 20/40/50 or Radeon card.

Tests: new `source_timer_real.py`, `screen_counter_truth.py`; updated
`test_source_countdown.py` (the switch, not the setter), `screen_smoke.py` (a
window source keeps the output hidden from capture - DXGI),
`screen_flow_performance.py` and `screen_radeon.py` (pick a window, then the
generator). Release 0 warnings.

## Round 28 — DLSS MFG that moves evenly under a heavy game, and a counter drawn in the frame — 2026-10-03

DLSS MFG only (FLOW MFG was not worked on). The report: in a game that fills
the card the frames in between "come in waves", unlike Lossless Scaling, on an
RTX 3060 and an RTX 4070 Ti; the FPS counter makes it worse. Measured on the
RTX 3060, 2560x1440 at 200 Hz, with the stand-in game filling the card
(`tests/gpu_load.cpp --fps 300 --busy 12`: 66-93 FPS alone) and a new test,
`tests/screen_fg_pacing.py`: the presenter writes one line per Present
(`LF_FG_TRACE`) - when, and which moment of the game the picture stands for.
Smooth is a constant distance between the two.

**What was wrong**

1. *The presenter began each pair when it arrived.* Real frame, the next
   pair's frame in between 1 ms later, then 13 ms of nothing: x2 that moved in
   the game's own steps. 43% of the steps on the screen were gaps.
2. *The card's scheduling class was measured, starting at normal.* Under a
   full card DLSS MFG x3 got 5 real frames a second there - too few for a
   `[latency]` summary, so the choice was never made and it stayed at 5. And
   every later measurement (drift, every two minutes) was seconds at the
   normal class again: the lag that came and went.
3. *Real frames were spaced by the clock* (refresh / multiplier). A game a
   little over the limit was taken later and later until a frame was lost;
   each lost frame made a pair twice as long (x4: a quarter of the steps).
4. *A frame was copied to the back buffer at the moment it was due.* The copy
   waits for the card, 1-5 ms and never the same: each frame's own lateness.
5. *The counter was a window on top of the output* (Avalonia, transparent,
   topmost): the compositor blends two windows for every frame instead of
   passing the output's frames straight on.

**What it does now** (`lf_fg_pace.inl`, `lf_dlssg.inl`, `lf_latency.inl`,
`lf_desktop.inl`, `lf_counter.inl`)

- Every picture is shown at *its moment + one delay*. The delay covers what a
  pair needs before its first frame in between can be shown, taken from the
  recent needs by the **Queue target** (Lossless Scaling's name): 0 their
  median, 1 the 90th percentile (default), 2 the largest and half a step. It
  moves only when it is more than half a millisecond off, slowly.
- Pairs are shown in order (four slots); one is passed over only when the
  presenter is a whole pair behind. A frame in between too late to be told
  from the next is left out instead of shown next to it.
- The moments run on a clock of their own: the capture's stamps lie on the
  desktop's refresh grid (a 130 FPS game on 200 Hz arrives as 5, 10, 5, 10 ms)
  and are smoothed while within one refresh of the clock.
- Frames are drawn ahead (copy + fence as soon as the one before is
  presented) and only the Present waits for the time.
- DLSS MFG runs at the realtime GPU class from its first frame, never measured
  (`LF_FG_CLASS=normal` to compare). FLOW MFG keeps its measurement, but a
  starved one now ends, and realtime once chosen is kept.
- The frames taken are chosen by their stamps (`FgGate`, in `DeskTake`): every
  keep-th frame and `count` frames in between, whichever shows most within the
  refresh rate (+4%); more real frames win a tie. 130 FPS x3 on 200 Hz: every
  second frame, two in between (195). 130 FPS x2: its own 130 (NGX rests).
- The counter is copied into the frame before its Present by the presenting
  thread - GDI text into an upload buffer in the back buffer's format (8-bit,
  HDR10, scRGB), only when a figure changes. Figures are counted at the
  Present. `counter.json` (on, dx, dy, hold) is the manager's; "hold" while
  the overlay is open, when the manager's window stands in so it can be
  dragged. Not in recordings or Spout.

| heavy game, card full | before | after |
|---|---|---|
| x2: jitter, mean / p95 | 3.43 / 7.20 ms | 0.10 / 0.25 ms |
| x2: gaps (steps over 1.6x) | 42.9% | 0.2-0.4% |
| x2: step spread (sd) | 6.85 ms | 0.5-0.8 ms |
| x3 | 5 real -> 15 shown | 52-54 real -> 156-160 shown, jitter 0.2-0.3 ms |
| x4 | gaps 27% (first new presenter) | 48 -> 192-197, gaps 2.5% |
| 130 FPS (5 ms) x2 / x3, counter | 84-88 / 167-175, 62 / 186 | 96 / 192, 96 / 192 |

Open: the game still pays for the generator (66 -> 57-60 at x2, 130 -> 97);
a pair is ready 9.6 ms after its frame reached the desktop (capture, motion,
NGX) - the next thing to cut; "wave" (quarter-second averages) 1-2 ms at x3/x4
on a fixed 200 Hz grid; no real game and no Lossless Scaling side by side; the
counter's HDR formats are converted but were not seen on an HDR screen.

Settings: `frame_generation_queue` (0/1/2), reshape bits 21-22 hold it plus
one, `LF_FG_QUEUE` at start. Tests: `screen_fg_pacing.py`,
`screen_counter_native.py` (reads the counter back from the back buffer).

## Round 29 — the page laid out from the person's sketch, colours kept, the output's colour space — 2026-10-03

**The LIVE FLOW page** (`Views/ScreenView.axaml`, from a hand-drawn sketch):

| | |
|---|---|
| top bar | LIVE FLOW, the card, Exit / Start - and the source: one button, "Select window · 5 s". Press, click the game; when the count runs out the window in front is taken (the countdown of 2026-10-02, now the only way in sight). While it counts the button says "Cancel · N s"; the window taken stands beside it. |
| DLSS 5 | shown: DLSS 5, Flow Super Resolution, Multi-pass, Denoiser, Sharpness, Edge smoothing, the network resolution and its three presets |
| EFFECT | folded: profile, model, intensity, local tone / structure, skin structure, **Keep colors** |
| DLSS MFG · ALL RTX CARDS | shown: one list for switch and multiplier (Off, x2 ... x6), MFG resolution, Queue target, Low Latency |
| COLORS / HDR | folded: the four colour sliders, **Color space**, HDR compatibility |
| FPS / VRR | shown: VRR, the frame limit, the FPS counter |
| COMPARISON | shown |
| below | folded: Source (the list of windows, refresh / reconnect, the count's length), My presets, Settings, Capture, **Overlay and shortcuts** (moved down from the top bar) |

A folded card opens from its title like a section of Settings
(`Button.SectionHeader`, `ToggleSection`); which are open is kept in
`sections.json`, and a search opens them all. Titles are upper case.
**FLOW MFG is not offered on RTX cards**: its card takes DLSS MFG's place in
AMD MODE and on a card that is not an RTX (`usesFlow`), and a FLOW MFG left on
by an older version on an RTX card is switched off. The overlay shows the same
cards without the header, source, comparison, presets, settings and its own.

**"DLSS 5 at x1 changes the colours badly, x2 keeps them."** x1 / x2 is
Multi-pass. Measured without the screen (`tests/nr_color_offline.py`: a photo
through the worker's pipe, the result back the same way): one pass of the
network takes colour out of the picture and it gets worse the longer a scene
stays - saturation +1.9 after 8 frames, -7.3 after 40, -12.4 after 80, with red
and green falling more than blue (a cool cast). A second pass happens to put
saturation back on that photo (+3.3) and not on another (-13.9 both); the RTX
compatibility package changes nothing. The network is about light and
structure, so where its result is put back on the picture (`kDetailHlsl`) the
picture now keeps its own colour and takes the network's brightness - every
channel scaled alike, hue and saturation exactly as they were; added instead
in the dark, and brought towards grey only as far as white allows. **Keep
colors**, 100% by default (`keep_colors`, `LF_KEEP_COLOR`, reshape bits 24-31):
saturation -0.0 at 1 and at 2 passes on the photo, -3.4 where it was -13.7.
Not applied where the network writes the full-size picture directly with the
Denoiser at 0 (no compose pass runs there).

**Color space** (`color_space`, `LF_COLOR_SPACE`, `lf_hdr_display.h`): auto
(the HDR switch decides, as before), sRGB (8 bits, also on an HDR screen),
sRGB Linear (half float, linear light, SDR range - white at the screen's SDR
white, `kHdrOutHlsl` bit 4) and scRGB (HDR kept when the screen is in HDR,
linear otherwise). The worker starts over for it. With a generator on, the
linear output goes out as 8-bit sRGB (DLSS MFG takes SDR or HDR10 only); a
screen that refuses the linear swap chain falls back to sRGB for the session.

**HDR, checked as far as it can be without an HDR screen**
(`tests/test_hdr_mfg_gpu.py`, the production passes on an HDR10 swap chain):
scRGB exact, PQ within 1% at 10 / 80 / 200 / 1000 nits, DLSS MFG's frames with
the round-28 presenter, the linear output's levels exact on both kinds of
desktop, and the counter on an HDR10 frame - pill 1.4 nits, digits 203 nits,
through both presenters. **VRR**: the frame gate takes the VRR cap as the room
there is (`DeskTake`).

Also: Multi-pass 5 and 6 were cut to 4 at start (`host.py`, `session.py`).

**The screen was off for most of this round.** With the monitor powered off
Desktop Duplication hands over no frame at all - not for a game, not for a
moving window - while Windows still says the display is on. Every capture test
then reads as "no frames" (and looks like a bug in whatever was changed last).
`tests/capture_alive.py` says in a second whether the capture is alive; run it
before believing a silent capture. What could not be run for that reason: the
live tests of round 28 again (pacing, counter), `tests/screen_color_passes.py`
(the same colour check from the screen, via `LF_SHOW_PROOF`), and the new page
with a game behind it.

Tests: `nr_color_offline.py`, `screen_settings_r29.py`, `capture_alive.py`,
`screen_color_passes.py` (needs the screen); `gpu_load --image FILE --pan N`
shows a picture from a program the capture sees as a game.

## Round 30 — still parts of a page under DLSS MFG, the page tidied, shortcuts while away — 2026-10-03

**"DLSS MFG is fine while the whole screen moves; on a page (YouTube) the text
flickers and lines are pulled about."** Two causes, both in the frames in
between:

1. The motion estimate covers the still text around a moving video with the
   video's motion, and the frame in between is made at the MFG resolution and
   scaled up - so text that never changed is redrawn softer and shifted every
   other frame. `lf_fg_still.inl`: the captured picture is compared with the
   one before at full size; where it did not change the motion is set to zero
   before the generator sees it (`FgStillCalm`), 16-pixel tiles at rest are
   marked and the moving ones grown by `clamp(h/240, 4, 12)` tiles
   (`FgStillTiles`, one map per slot), and the presenter takes the pixels of a
   tile at rest from the pair's real frame (`FgStillShow`). A frame with
   nothing at rest (a game in motion) goes the old way untouched - `protect`
   is false and no pass runs. `LF_FG_STILL=0` is the worker of before.
2. A pair far longer than the ones around it (`FgPace::Irregular`: more than
   2.5 times the usual and 25 ms over it) is a page that changed after a rest -
   a line highlighted, a notice. Its frames in between were shown in a burst
   (the "sudden pull") and its wait was learned as the delay of every later
   frame. Such a pair shows its real frame at once, its frames in between are
   skipped and it teaches the pacing nothing.

`tests/screen_fg_still.py` (a page of text with a moving box, real and made
frames read back with `LF_SHOW_PROOF`), DLSS MFG x2:

| | far from the box | around the box |
|---|---|---|
| MFG at 1080p, before | difference 4.81 levels, 14.1% of pixels changed, worst 236 | 1.32, 15.3%, worst 29 |
| MFG at 1080p, now | 0.00, 0.0%, worst 0 | 0.17 - 0.28, 2.0 - 3.3%, worst 29 |
| MFG at Native, before | 0.00, 0.0%, worst 2 | 0.00, 0.0%, worst 2 |
| MFG at Native, now | 0.00, 0.0%, worst 0 | 0.00, 0.0%, worst 2 |

The ring that stays (2 - 3% beside the moving box) is the grown border: tiles
next to motion are left to the generator on purpose. Pacing is as in round 28
with the new code (`screen_fg_pacing.py`: x2 jitter 0.09 ms, x3 0.25, x4 0.34),
the HDR probe passes.

**Every RTX generation.** What differs by generation is two things, both
before NGX creates the feature: the compatibility package loaded for RTX 20 /
30, and the kernel patch for RTX 40 (`g_fg_runtime_max`). The presenter, the
pacing, the frame gate, the GPU class, the still pixels and the counter are
one code path after that. Measured on an RTX 3060 only - there is no other
card here.

**The page.** The Source card is gone: the top button and a click on the
window are the only way (the worker's list of windows stays in the engine).
FPS / VRR is folded, VRR and the FPS counter on one line and the frame limit
under them at full width. Order: DLSS 5, DLSS MFG (or FLOW MFG), Effect,
Colors / HDR, FPS / VRR, Comparison, My presets, Settings, Record, Overlay and
shortcuts. Capture is called **Record**. The header: title 20, the card's
badge 34 high and hidden while empty.

**Screenshot shortcut.** F10 by default (`hotkeys.json` `shot_mods` /
`shot_key`, hotkey id 0x5D53), changed from the Overlay and shortcuts card
like the other two; the button says "Screenshot · F10". The three shortcuts
refuse each other's key.

**"Minimized, on LIVE FLOW, the overlay does not open."** The shortcuts were
honoured only while the LIVE FLOW tab was the one in view. Started by Windows
into the tray the app is on GAMES, so nothing answered. Now they are honoured
whenever the main window is hidden or minimized (`away ()`), and opening the
overlay moves the app to LIVE FLOW first.

**"Started with Windows it does not launch."** Not reproduced: a Release build
with `--startup`, from System32, with and without the network, to the tray
and to a window, starts every time. Hardened where it could fail silently:
Windows' own switch (`StartupApproved\Run`, first byte odd = disabled in
Task Manager) is read by `isEnabled` and cleared when the person turns the
option on; the tray is waited for before the window is hidden
(`taskbarReady`, the shell is not up yet that early); and a start with
`--startup` writes what it did, and any unhandled exception, to
`%LOCALAPPDATA%\DLSS5Manager\startup.log`. The option itself was not toggled
here.

**Manage sheet.** The one-click sheet (560 wide) has a header of two lines:
the name and the close button, then the mode button on the left and SHARE
RESULT on the right; texts are cut with "…" past 170 / 190. On one line a
language with longer words pushed the buttons into each other. The advanced
sheet keeps one line (it has the room); its name gives way first. Checked in
Russian.

Tests: `screen_fg_still.py` (new), and again `screen_fg_pacing.py`,
`screen_counter_native.py`, `screen_color_passes.py`, `test_hdr_mfg_gpu.py`,
`screen_settings_r29.py`, `screen_radeon.py` (the Radeon worker built with
this round's shared code).

## Round 31 — a picked window always fills the screen; the frames that were not the game's — 2026-10-03 (1.3.5)

**"Select window takes the window but does not make it fill the screen like
Lossless Scaling - and perhaps that is why DLSS MFG has a wave at about 20
FPS."** Both were true, and the second followed from the first.

**Why it filled nothing.** HDR compatibility on and Flow Super Resolution on -
the state most people have. The engine left the game's window as it was
"because Flow Super Resolution fills the screen", and the worker scaled
nothing "because HDR is kept" (`SuperResWanted` is off under `HdrSwitchOn`,
which is the switch alone, whatever the screen is). Reproduced
(`tests/screen_source_pick.py --set hdr=true --set upscaler=true`): taken,
framed, at its own place, the output 1282x752 on a 2560x1440 screen. The
engine now asks the same question the worker does (`processor.super_res_runs`).

**A picked window fills the screen, one way or the other**
(`processor.flow_claim_window`):

1. *Borderless over the monitor* - the game draws at the screen's own size.
   `borderless.take` places it, `settle` looks again 0.15 and 0.35 s later and
   asks once more if the game undid it. The error check was wrong too: the
   last error was read through `ctypes.windll`, which does not keep it - a
   change that had worked could read as refused.
2. *Scaled* - a window that puts its own frame or its own size back, or that
   Windows will not let be touched (a game running elevated), is left exactly
   as it was and its picture is scaled onto the whole screen: `_source_fill`,
   sent with the window (`WINDOW_FILL` / `kWindowFill`), the worker's
   `FillWanted` - Flow Super Resolution's way out for a window (output window
   over the whole screen, edge-directed scaling, the window moved under the
   pointer), without its smaller internal picture. Not while HDR is really
   kept (the switch and a screen in HDR): there the window stays the size it is.

`keep_claim` looks once a second: a borderless window that later goes back to
its own size is asked once more, then scaled. A window picked with the button
keeps filling the screen when the generator goes off (`_source_claimed`);
clearing the selection, another pick or closing LIVE FLOW puts it back. The
old API path (`set window`, used by tests) is as it was.

**Picking.** Press the button, click the game: the window of another program
that has been in front for half a second is taken then (`CLICK_SETTLES`) -
nobody waits out the count, which is only how long there is to click. When it
runs out the window in front is taken, or - when just then Windows names none -
the last one that was in front during the count. Any window a person can be
looking at is taken (`windows_api._could_be_source`): before, only one with a
taskbar button of its own, and a game whose window is owned by a hidden one or
is a tool window was refused with the game in front. The shell's own windows
are refused by class.

| kind of window (`tests/gpu_load.cpp`) | before | now |
|---|---|---|
| popup, framed, maximized | taken after 6.6 s, borderless | 2.3 - 2.6 s, borderless |
| owned by a hidden window, tool window | not taken | 2.4 - 2.5 s, borderless |
| puts its own frame back once | taken, framed over the screen | 2.8 s, borderless (asked twice) |
| puts its frame back always / goes back to its own size | taken, stays small | 3.0 - 3.2 s, scaled onto the screen |

("fills" is read from the output's own back buffer: the monitor's size, the
game's picture from edge to edge.) In the app itself: pressed, clicked,
borderless 0.75 s after the click.

**The frames that were not the game's.** Three things handed DLSS MFG frames
the game never drew; each alone is a wave, most visible at a low frame rate:

1. *A window that is not the whole screen, cut out of the desktop.* The
   cut-out counts whatever changes over the window's place - LIVE FLOW's own
   output among it. A 20 FPS game: 41 to 67 "real" frames in some seconds.
   Now only a window that is its whole screen is taken that way
   (`CropIsWholeScreen`); any other by Graphics Capture.
2. *Graphics Capture's own beat.* Left to itself it handed a game drawing
   evenly every 50 ms over 40, 60, 40, 60 ms apart; `MinUpdateInterval` of
   1 ms gives 50 +- 1. It also answered at once with nothing new, so the
   network ran three times on the same picture (156 a second for 51 shown);
   it waits for a frame now, as the desktop source does.
3. *The desktop handed over again for LIVE FLOW's own frames.* Where the
   compositor draws the output (no direct flip - seen here after the desktop
   compositor restarted; a system without overlay planes is always so),
   Desktop Duplication reports a new desktop frame for every frame LIVE FLOW
   shows: the game's picture unchanged, a new time stamp. A 72 FPS game read
   as 165, the generator switched itself off for "no room". Nothing in the
   frame's metadata tells such a frame apart (probed: one dirty rectangle,
   the whole screen, either way), so the picture decides: a capture whose luma
   is the one before's exactly is nothing new (`LumaChangeNow`, `SourceSeen` -
   the comparison the scene cut already made). While repeats come the frame
   gate counts the game's rate from the frames that proved new (`FgGateFresh`).
   Each repeat still costs a capture before it is known: at x2 / x3 nothing to
   speak of, at x4 on a full card the pace suffers (jitter 2.3 ms).

DLSS MFG x2 / x4 on a 20 FPS game (`screen_fg_pacing.py --fps 20 --busy 5`):

| | wave (max) | step sd |
|---|---|---|
| before, the window left as it was | 4.63 (20.03) / 1.29 (8.96) ms | 5.51 / 2.67 ms |
| now, scaled (`--load "--snapback 50"`) | 1.19 (1.93) / 0.40 (0.51) | 0.59 / 0.43 |
| now, borderless | 0.58 (0.90) / 0.41 (0.52) | 0.58 / 0.84 |

Also at a low rate: the delay the very first pair set was kept and came down
a millisecond a second (50 ms sliding to 34 over seventeen seconds); it is put
right once after six pairs (`FgPace::kSettled`). The round-28 fixture is as it
was (x2 0.10, x3 0.25, x4 0.33 ms jitter, measured before the compositor
restarted).

**The button** is words only. **Version** 1.3.5.

**What happened to this machine during the round.** Every start of the worker
leaves six `nvngx_update.exe` (the NVIDIA driver's own updater, started by
NGX) that stay about forty minutes. 164 worker starts in forty minutes of
tests left 977 of them, 23 GB: memory ran out and the desktop compositor
restarted (18:58:55, `0xc00001ad`). They were closed by hand, and after every
batch of tests since; the test scripts do not do it themselves, and nothing
stops NGX starting them (not fixed). Since that restart this desktop is in state 3 above - a
sign-out puts it back. The measurements of state 3 are from after it, all
others from before or unaffected by it.

Tests: `screen_source_pick.py` (new: every kind of window, `--generator 1`,
`--set`), `test_source_countdown.py` (the new rule), `source_timer_real.py`
(a picked window stays), `screen_fg_pacing.py --load`, and the rest of the
set again.
