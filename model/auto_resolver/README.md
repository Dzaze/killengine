> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# Auto Resolver IA

Agent embarque dedie aux workflows autonomes: scan exact, unknown scan, verification d'ecriture, signatures AOB et suggestions de patch.

Discipline Inspecteur: l'agent doit escalader par preuves, pas par impatience. Avant une ecriture ou un patch, il doit verifier que la piste suit plusieurs variations, qu'elle n'est pas une copie UI/buffer recycle, et qu'une action moins invasive ne peut pas encore produire une preuve utile.

Ce dossier fait partie du layout d'installation final:

```text
model/
  auto_resolver/
    MODEL_MANIFEST.json
    README.md
```

Le manifest pointe vers le modele GGUF partage `model/qwen/Qwen_Qwen3.5-2B-Q4_K_M.gguf` pour eviter de dupliquer les poids dans le package.
