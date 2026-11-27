# GitHub Copilot Instructions for MFG SUPER MOTION

Follow these strict engineering and security rules for every suggestion and pull request:

1. **License Protection**: Never modify `LICENSE` or `THIRD-PARTY-NOTICES.txt` (PolyForm Strict License 1.0.0).
2. **Prohibited Code**:
   - Never introduce telemetry, HWID collection, unrequested HTTP calls, automatic executable downloads without mandatory SHA-256 verification, or unverified DLL loading.
   - Never use unsafe C/C++ functions (`strcpy`, `sprintf`, `gets`, `system`).
   - Never introduce dynamic code execution (`Assembly.Load`, shell injection via `Process.Start`) in F#.
   - Never add precompiled binaries (`.exe`, `.dll`, `.obj`, `.lib`, `.rar`, `.zip`) to commits.
3. **Hardware Compatibility**:
   - All frame generation and UI changes must preserve explicit support for non-Tensor Core GPUs (such as `NVIDIA GTX 1060` and `Intel iGPUs`).
4. **Mandatory Human Review**:
   - Changes in `Engine/`, `Infrastructure/`, and DXGI/WGC capture hooks always require human review before merging.
