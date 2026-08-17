# KillEngine Embedded AI Model

Normal product layout:

- `KillEngine.exe`
- `llama-cli.exe`
- `model/qwen/*.gguf`

All embedded AI assets live under `model/<ai-name>/`.
KillEngine discovers GGUF models automatically from those installed model subfolders.
The custom model path in Settings is an advanced override for development or manual testing.

Packaging:

- `.\scripts\package-windows.ps1` copies `model/<ai-name>/*.gguf` by default.
- `.\scripts\package-windows.ps1 -ExcludeModel` is only for lightweight development packages.
- The package step also copies `llama-cli.exe` when present in `third_party/llama.cpp` or `build/bin`.
