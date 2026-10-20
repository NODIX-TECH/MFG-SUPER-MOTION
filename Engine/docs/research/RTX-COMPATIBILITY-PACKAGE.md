# Live Flow RTX compatibility package

This package is loaded only inside the Live Flow NVIDIA worker when desktop
DLSS FG is enabled on SM75 or SM86. It is not installed in games or loaded by
the manager UI. Turning FG on/off restarts that worker so its hooks cannot
leak into a subsequent FLOW MFG session.

- Upstream: https://github.com/sdli1995/dlssg_for_sm86
- Version: 0.3.5, retrieved 2026-10-01 (`version.dll` from the upstream repository).
- SHA256: `C3934A09399F022504227C72DF0BF8C0DE55F9A08880DDDDE898C5262CEFA838`
- Turing research/assets: https://github.com/Coldwood1026/dlssg_for_sm75
- Keep `THIRD_PARTY_NOTICES.txt` with the binary. It is preserved verbatim from
  upstream; its older 310.1 runtime description is not the runtime used here.
  The tested 0.3.5 bundle actually extracts DLSS-G 310.9.1.0 (SHA256
  `FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82`).
- `ADA_LICENSE.txt` accompanies the separately adapted GPL-3.0-only Ada
  implementation in `ScreenNative/lf_dlssg_ada.h`. Source attribution is in
  that header. NVIDIA runtime/kernel assets are separate third-party material.

The worker matches CUDA's adapter LUID to the selected D3D12 device, verifies
the proxy's hash and copies it into the user data cache. It generates a
per-architecture INI with exact kernels and up to five generated frames.
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
