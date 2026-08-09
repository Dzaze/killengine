# KillEngine Packaging

## Portable package

Build and create a local portable package:

```powershell
.\scripts\package-windows.ps1
```

Output:

```text
dist\KillEngine-portable\
dist\KillEngine-portable.zip
```

The package includes the deployed Qt runtime from `build\bin`, the application executable, license, README, project spec, phase tracker, user guide, and `models\README.md`.

Models are not included by default because GGUF files are large and ignored by git. To include local GGUF files from `models\`, run:

```powershell
.\scripts\package-windows.ps1 -IncludeModel
```

## Inno Setup installer

After creating the portable payload, install Inno Setup and run:

```powershell
iscc packaging\windows\KillEngine.iss
```

Output:

```text
dist\installer\KillEngine-Setup-0.1.0.exe
```
