# Lua runtime

This directory is the local staging area for the Lua interpreter bundled with
KillEngine portable packages.

Run from the repository root:

```powershell
.\scripts\setup-lua-runtime.ps1
```

The script downloads the official Lua source archive, verifies the pinned
SHA256 checksum, builds `lua.exe` with MSVC, and stages it here. Runtime
binaries are intentionally ignored by Git; rebuild them locally before creating
a release package.
