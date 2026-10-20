# Thin-object preservation, 8 October 2026

The user reported lamp posts disappearing or flickering during driving in GTA V.
The reconstruction sources still matched the pre-XeSS backup byte for byte:
this investigation found incomplete protection in the existing repair, not a
XeSS change that removed round 10's fix.

## Correction

`sm_rebuild.inl` used a nearby colour in **one** real endpoint as evidence that
the generator had displaced a stationary feature. A pole passing that nearby
position in just one endpoint could consequently be classified as stationary
background. The check now requires that colour in both motion-aligned endpoints.

The final reconstruction also returned the real pixel unconditionally whenever
the endpoint-based still map was calm. This could erase the intermediate pole
even after the earlier pass had correctly preserved it. It now respects that
decision where generated and endpoint colours disagree.

`lf_fg_still.inl` applies the equivalent safeguard in the full-resolution path:
an intermediate feature absent from the nearby real picture survives. A shifted
static line still finds its colour nearby and receives the existing repair.
No setting, default, capture path, DLSS runtime, XeSS path, or frame queue changed.

## Reproducible validation

Build and run from the repository root:

```powershell
Engine/tests/build-poles.cmd
Engine/tests/poles.exe Engine
Engine/tests/poles.exe Engine/tests/dump/poles-baseline-20261008
```

`poles.exe` compiles the actual production HLSL and executes it through D3D11
WARP. It supplies controlled endpoint pictures, intermediate poles and static
lines to `ChooseMain`, `BuildMain`, and `ShowMain`, then reads back their output.
This does not depend on NVIDIA, screen capture, or scene timing.

| Case | Before | After |
| --- | --- | --- |
| Moving pole retained instead of classifying it as background | Fail | Pass |
| Displaced static feature still receives repair | Pass | Pass |
| Calm tile does not erase generated pole in reconstruction | Fail | Pass |
| Confirmed stationary region retains original pixels | Pass | Pass |
| Full-resolution calm tile retains generated pole | Fail | Pass |
| Full-resolution shifted static line still repaired | Pass | Pass |

The native engine build succeeded. A live `jelly.ps1` driving fixture with Flow
35, x2, 30 FPS and still protection enabled completed normally, reporting about
59–60 presented FPS and 0.0% HUD pixels over the comparison threshold in the
sampled pairs. Its captured scene phases differ from the baseline run, so their
pole error percentages are **not** an aligned A/B quality measurement.

## Limits

This proves three engine-side ways of erasing intermediate poles are corrected.
It does not prove that every GTA V flicker is fixed. The generator's own errors,
flat-depth approximation, and insufficient source detail remain. A generated
artifact absent from both endpoints may also survive the more conservative
repair. The small extra texture reads have not been independently GPU-timed.
GTA V and other GPU families were not tested in this change.

Sources, the previous executable, and test outputs are preserved under
`Engine/tests/dump/poles-baseline-20261008`.
