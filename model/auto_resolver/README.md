# Auto Resolver IA

Agent embarque dedie aux workflows autonomes: scan exact, unknown scan, verification d'ecriture, signatures AOB et suggestions de patch.

Ce dossier fait partie du layout d'installation final:

```text
model/
  auto_resolver/
    MODEL_MANIFEST.json
    README.md
```

Le manifest pointe vers le modele GGUF partage `model/qwen/Qwen_Qwen3.5-2B-Q4_K_M.gguf` pour eviter de dupliquer les poids dans le package.
