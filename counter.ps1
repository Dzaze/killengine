$Dossier = $PSScriptRoot

$Fichiers = Get-ChildItem -Path $Dossier -File -Recurse
$Dossiers = Get-ChildItem -Path $Dossier -Directory -Recurse

$NombreFichiers = $Fichiers.Count
$NombreDossiers = $Dossiers.Count
$TailleOctets = ($Fichiers | Measure-Object -Property Length -Sum).Sum
$TailleGo = [math]::Round($TailleOctets / 1GB, 2)

Write-Host "Dossier analysé : $Dossier"
Write-Host "Nombre total de fichiers : $NombreFichiers"
Write-Host "Nombre total de sous-dossiers : $NombreDossiers"
Write-Host "Taille totale : $TailleGo Go"