# AI Assistant and Contributor Guidelines

These rules govern all AI coding assistants and automated contributors working in this repository.

## 1. License and Scope Constraints

- Respect the **PolyForm Strict License 1.0.0**. Never modify `LICENSE` or `THIRD-PARTY-NOTICES.txt`.
- Never commit precompiled binaries (`.exe`, `.dll`, `.rar`, `.zip`, `.obj`, `.lib`, `.pdb`, `.so`).

## 2. Prohibited Behaviors (Zero Tolerance)

- **No Telemetry or Tracking**: Do not add telemetry, analytics, crash-reporting beacons, or HWID collection.
- **No Unrequested Network Calls**: Do not add background HTTP/WebSocket calls, external beacons, or hardcoded webhooks.
- **No Unverified Binary Downloads**: Do not download executables or DLLs at runtime without mandatory SHA-256 verification (fail closed when hash is missing).
- **No Unverified DLL Loading**: Do not load dynamic libraries via `LoadLibrary`, `LoadLibraryEx`, or `NativeLibrary` without path restriction and signature/hash validation.

## 3. Memory Safety and Language Restrictions

- **C / C++ (`Engine/`)**: Never use unsafe C functions (`strcpy`, `strcat`, `sprintf`, `vsprintf`, `gets`, `system`, `popen`). Use bounded alternatives (`strncpy_s`, `snprintf`, `swprintf_s`).
- **F# (`Infrastructure/`, `ViewModels/`, `Views/`)**: Never use dynamic assembly loading (`Assembly.Load`, `Assembly.LoadFrom`) or unsanitized shell execution (`cmd.exe`, `powershell`, `UseShellExecute = true` on untrusted input).

## 4. Hardware Compatibility Requirements

- Maintain explicit compatibility with **non-Tensor Core GPUs**, including **NVIDIA GTX 1060** and **Intel integrated GPUs (iGPUs)**.
- Keep fallback paths for `SMFG`, `SMAI`, and `SMT` functional without requiring RTX Tensor Cores.

## 5. Mandatory Human Review

Any pull request touching the following paths or subsystems requires explicit human maintainer review and approval before merge:
- `Engine/` (all C/C++ native frame generation and shader code)
- `Infrastructure/` (network, updates, startup registration, console bridges)
- DXGI Desktop Duplication and Windows.Graphics.Capture (`WGC`) hooks
