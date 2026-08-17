# Signature Authenticode de KillEngine

Ce document est pour un humain (celui qui paie), pas pour un agent IA : obtenir un
certificat de signature de code est une démarche d'achat/identité, aucun agent ne
peut la faire à ta place. Le tooling scripté (`scripts/codesign.ps1`) est prêt et
attend seulement que ce certificat existe.

## Pourquoi c'est important pour KillEngine spécifiquement

`KillEngine.exe` lit/écrit la mémoire d'autres processus, pose des breakpoints
matériels et injecte du code — exactement le profil comportemental que Windows
Defender SmartScreen et la plupart des antivirus flaguent par heuristique, même
sans signature de malware connue. Un exécutable non signé neuf ("pas de
réputation") déclenche quasi systématiquement un avertissement SmartScreen au
premier lancement, ce qui tue la conversion pour un produit payant avant même que
l'utilisateur ait vu l'Assistant. Cheat Engine (gratuit, réputation ancienne) et
WeMod (signé) n'ont pas ce problème au même degré ; c'est un vrai écart à combler
pour vendre KillEngine à 50 USD avec confiance.

## Ce qui a changé depuis 2023 : pas de simple fichier .pfx

Avant le 1er juin 2023, on pouvait acheter un certificat OV (Organization
Validation) livré en fichier `.pfx` + mot de passe, et signer avec `signtool sign
/f cert.pfx /p motdepasse`. **Ce n'est plus permis** par les règles du CA/Browser
Forum : la clé privée de tout certificat de signature de code (OV *et* EV) doit
maintenant vivre sur un module matériel certifié FIPS 140-2 niveau 2+ — un token
USB physique, ou un service de signature cloud (HSM cloud) qui joue le même rôle
sans expédier de matériel.

`scripts/codesign.ps1` supporte donc en priorité le chemin **thumbprint** (le
certificat apparaît dans le magasin Windows `CurrentUser\My` une fois le
token/l'agent cloud installé et connecté), et garde le chemin `.pfx` seulement
pour compatibilité avec un vieux certificat auto-émis ou un besoin CI particulier.

## Options concrètes, de la moins chère à la plus "entreprise"

| Option | Type | Coût approx. | Ce que ça implique |
| --- | --- | --- | --- |
| **Azure Trusted Signing** | Signature cloud Microsoft | ~10 USD/mois | Le plus adapté à un éditeur solo/petite structure. Nécessite un compte Azure, une identité vérifiée (particulier ou société), pas de token physique. S'intègre à `signtool` via le plugin `Trusted Signing` (dlib) — le certificat apparaît comme "public trust". |
| **SSL.com eSigner** | Cloud HSM | ~130-230 USD/an | Client cloud + app d'authentification (2FA), pas de token à recevoir par courrier. Certificat OV ou EV selon l'offre. |
| **DigiCert KeyLocker** | Cloud HSM | ~300-500 USD/an | Équivalent DigiCert, souvent vendu avec le certificat EV DigiCert. |
| **Token USB EV classique** (DigiCert, Sectigo, GlobalSign) | Matériel | ~300-500 USD/an | Réputation SmartScreen immédiate dès la première signature (contrairement à OV/cloud qui construit une réputation progressivement). Nécessite de recevoir et brancher un token physique pour signer — pénible en CI. |

Recommandation raisonnable pour un premier lancement solo : **Azure Trusted
Signing** si l'identité vérifiable (toi ou une micro-entreprise) est éligible —
c'est le seul qui reste abordable à l'échelle d'un produit à 50 USD. Bascule vers
EV plus tard si le volume de ventes justifie la réputation SmartScreen immédiate.

Toutes ces options demandent une vérification d'identité (pièce d'identité,
parfois numéro d'entreprise/SIRET) qui prend de quelques heures à quelques jours
ouvrés — à anticiper avant une date de sortie.

## Une fois le certificat obtenu

1. Installer le client/agent du fournisseur (il enregistre le certificat dans le
   magasin `CurrentUser\My` de la machine qui signe, ou expose un plugin
   `signtool`).
2. Récupérer l'empreinte SHA1 du certificat :
   ```powershell
   Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert
   ```
3. Configurer les variables d'environnement avant de packager :
   ```powershell
   $env:KILLENGINE_CODESIGN_THUMBPRINT = "<empreinte SHA1>"
   ```
4. Packager en exigeant la signature :
   ```powershell
   .\scripts\package-windows.ps1 -RequireSigning
   ```
   ou, pipeline complet :
   ```powershell
   .\scripts\release-check.ps1 -Package -RequireSigning
   ```
5. Signer aussi l'installeur Inno Setup généré séparément :
   ```powershell
   iscc packaging\windows\KillEngine.iss
   .\scripts\codesign.ps1 -Path "dist\installer\KillEngine-Setup-0.1.0.exe" -RequireSigning
   ```

Sans certificat configuré, `-RequireSigning` fait échouer volontairement le
packaging au lieu de livrer silencieusement un exécutable non signé — c'est le
comportement voulu pour un vrai build de release. Sans ce flag, le packaging
continue et avertit seulement (comportement de dev par défaut).

## Ce que `scripts/codesign.ps1` fait déjà

- Détecte `signtool.exe` (PATH, ou Windows 10/11 SDK).
- Signe avec horodatage RFC3161 (`/tr .../ td SHA256`) pour que la signature
  reste valide après l'expiration du certificat.
- Vérifie la signature après coup (`Get-AuthenticodeSignature`) et échoue si elle
  n'est pas `Valid`.
- `scripts/package-windows.ps1` l'appelle automatiquement sur `KillEngine.exe`
  avant de zipper le paquet portable ; `scripts/release-check.ps1 -Package`
  relaie `-RequireSigning`.

Ce qui reste hors scope de ce tooling, volontairement : acheter le certificat,
vérifier l'identité auprès du fournisseur, et — si Azure Trusted Signing ou
équivalent est retenu — le brancher dans `codesign.ps1` via son propre
plugin `signtool` si l'intégration diffère du chemin thumbprint standard
(à vérifier au moment de l'implémentation réelle, les intégrations cloud signing
évoluent plus vite que ce document).
