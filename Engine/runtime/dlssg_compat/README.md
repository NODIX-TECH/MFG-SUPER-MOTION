# Live Flow RTX compatibility package

This package is loaded only inside the Live Flow NVIDIA worker when desktop
DLSS FG is enabled on SM75 or SM86. It is not installed in games or loaded by
the manager UI. Turning FG on/off restarts that worker so its hooks cannot
leak into a subsequent FLOW MFG session.

- Upstream: https://github.com/sdli1995/dlssg_for_sm86
- Version: 0.4.0 (dlssg-release-0.4.0-x64, built 2026-10-14), taken in on 2026-10-17 (round 42).
  `version.dll` SHA256 `7B3F90435BE4F94348BF8D128C84871C7F904C704F7690601A263277C327A7B2`,
  `sm86_backend.dll` (the package's `dlssg_sm86\backend\`) `30FFC6E09A347CCDE87ECC6FA54FD7818F5ED8A9353F5C31C2A90835C7F0CA9B`,
  `fg_kernels.dll` (its `dlssg_sm86\kernels\`) `1394C2D5D9CC8938BA3E52B043048D20D6D4C13A019C161B11A3DFD16343EC24`.
  Its runtime is NVIDIA's DLSS-G 310.9.16 (`CADEE1AA...`), the same file as the engine's own
  `nvngx_dlssg.dll`, so it is not shipped twice. Its super-resolution and XeSS packs (`sr\`, `xefg\`)
  are not used here and not shipped. RTX 30 (SM86) and RTX 20 (SM75) from one package; preset C (FG Lite).
  `LICENSE.md` and `THIRD_PARTY_NOTICES.txt` are the package's own, verbatim.
- Turing research/assets: https://github.com/Coldwood1026/dlssg_for_sm75
- Keep `THIRD_PARTY_NOTICES.txt` with the binary. It is preserved verbatim from
  upstream.
- `ADA_LICENSE.txt` accompanies the separately adapted GPL-3.0-only Ada
  implementation in `ScreenNative/lf_dlssg_ada.h`. Source attribution is in
  that header. NVIDIA runtime/kernel assets are separate third-party material.

The worker matches CUDA's adapter LUID to the selected D3D12 device, verifies
every file's hash and lays them out in the user data cache as the package wants
(proxy, INI and dlssg_sm86\ side by side). The INI it writes: exact kernels
(tier 1), up to five generated frames, and the DLSS MFG preset picked in the app.
No manual proxy placement or system CUDA toolkit installation is required.

SM89 uses the Ada temporal-kernel adaptation of
https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass
(derived upstream from y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG).
All changes are planned in memory; both code gates and temporal kernels must
match before writing. Unknown layouts retain ordinary x2 support. SM120 uses
the NVIDIA runtime directly. AMD retains FLOW MFG and cannot enable DLSS FG.

This desktop integration supplies estimated motion and a constant depth plane.
It cannot reproduce engine depth, HUD-free color or engine motion vectors.
RTX 3060 is hardware-tested; other architectures require hardware validation.
