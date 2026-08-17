# KillEngine Embedded AI Folder

Product convention:

```text
model/
  assistant/
    README.md
    MODEL_MANIFEST.json
  auto_resolver/
    README.md
    MODEL_MANIFEST.json
  qwen/
    *.gguf
    README.md
    MODEL_MANIFEST.md
  <other-ai>/
    *.gguf
    README.md
    MODEL_MANIFEST.md
```

Each embedded AI owns its own subfolder. Agent folders use `MODEL_MANIFEST.json` and can point to a shared GGUF folder when several agents use the same weights. Shared model-weight folders such as `qwen/` keep the actual `*.gguf` files.

The installer must deploy every required AI folder under `model/` next to `KillEngine.exe`:

- `model/assistant/` for the conversational/product agent.
- `model/auto_resolver/` for the autonomous investigation agent.
- `model/qwen/` for the shared local GGUF weights.

Development-only lightweight packages may omit GGUF files with `-ExcludeModel`, but sellable/product packages must include the embedded AI runtime and models.
