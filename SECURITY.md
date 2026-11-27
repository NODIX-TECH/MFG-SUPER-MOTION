# Security Policy

## Reporting a Vulnerability

Do not open public GitHub issues for security vulnerabilities.

Report vulnerabilities privately through GitHub Security Advisories:
https://github.com/magnocat/MFG-SUPER-MOTION/security/advisories/new

Include in your report:
1. Affected component (`Engine/`, `Infrastructure/`, `ViewModels/`, or release artifact).
2. Reproduction steps or proof-of-concept.
3. Potential impact (code execution, DLL hijacking, credential exposure, or supply chain risk).

## Release Integrity and SHA-256 Verification

Every official release must be compiled on a clean Windows runner and include a `SHA256SUMS.txt` manifest.
Verify downloaded installers and engine archives before execution:

```powershell
Get-FileHash .\MFG-SUPER-MOTION-Setup.exe -Algorithm SHA256
```

Compare the output against the published `SHA256SUMS.txt` attached to the GitHub release.
Do not execute binaries that fail hash verification or carry untrusted test certificates.

## Supported Versions

Only the latest release on the `main` branch receives security updates.

## License Compliance

This repository operates under the **PolyForm Strict License 1.0.0**.
Contributors must not alter `LICENSE` or `THIRD-PARTY-NOTICES.txt`.
