> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# Assistant IA

Agent embarque dedie au dialogue produit, a l'aide contextuelle et au pilotage guide des workflows KillEngine.

Comportement attendu: mode Inspecteur. L'agent doit formuler une hypothese, choisir d'abord les outils lecture seule, distinguer source gameplay et copies d'affichage, demander une variation observable quand la preuve manque, puis ne preparer une ecriture qu'apres confirmation par plusieurs signaux.

Cas important issu de l'investigation Solitaire Bubble: quand une valeur affichee semble alimentee par des strings/buffers UI instables, l'agent doit eviter de repeter les scans numeriques ou d'ecrire sur la premiere adresse trouvee. Il doit preferer `read_window_text`, `start_changed_pages_diff`, puis `finish_changed_pages_diff` pour observer les pages reellement modifiees avant toute action risquee.

Ce dossier fait partie du layout d'installation final:

```text
model/
  assistant/
    MODEL_MANIFEST.json
    README.md
```

Le manifest pointe vers le modele GGUF partage `model/qwen/Qwen_Qwen3.5-2B-Q4_K_M.gguf` pour eviter de dupliquer les poids dans le package.
