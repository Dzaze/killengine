# KillEngine Authenticode signing helper
#
# Signs a single file (exe/dll/installer) with signtool.exe, using whichever
# certificate source is configured through environment variables. Designed to
# be safe to run on a machine with no certificate at all: it warns and no-ops
# unless -RequireSigning is passed, so dev/local packaging never breaks.
#
# Certificate sources (checked in this order):
#   1. KILLENGINE_CODESIGN_THUMBPRINT
#      SHA1 thumbprint of a certificate already visible to signtool's cert
#      store (CurrentUser\My) — this is how hardware tokens (EV USB tokens)
#      and cloud HSM signing agents (Azure Trusted Signing, SSL.com eSigner
#      cloud client, DigiCert KeyLocker) normally expose themselves once
#      their local client/driver is installed and signed-in. This is the
#      only supported path for CA/Browser Forum-compliant certs issued after
#      2023-06-01, which must live on FIPS-validated hardware, not a plain
#      .pfx file.
#   2. KILLENGINE_CODESIGN_PFX (+ KILLENGINE_CODESIGN_PFX_PASSWORD)
#      Legacy path for a software .pfx certificate. Kept for older/self
#      issued certs and CI setups that inject a temporary PFX from a secret
#      store; not valid for standard OV/EV certs issued under current CA
#      baseline requirements.
#
# Usage:
#   .\scripts\codesign.ps1 -Path build\bin\KillEngine.exe
#   .\scripts\codesign.ps1 -Path dist\installer\KillEngine-Setup-0.1.0.exe -RequireSigning
#
# See docs/CODE_SIGNING.md for how to actually obtain a certificate.

param(
    [Parameter(Mandatory = $true)][string]$Path,
    [switch]$RequireSigning,
    [string]$TimestampUrl = $(if ($env:KILLENGINE_TIMESTAMP_URL) { $env:KILLENGINE_TIMESTAMP_URL } else { "http://timestamp.digicert.com" })
)

$ErrorActionPreference = "Stop"

function Find-SignTool {
    $fromPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    )

    $candidates = foreach ($root in $roots) {
        if (Test-Path $root) {
            Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
                ForEach-Object {
                    $x64 = Join-Path $_.FullName "x64\signtool.exe"
                    if (Test-Path $x64) { $x64 }
                }
        }
    }

    $candidates = @($candidates | Sort-Object -Descending)
    if ($candidates.Count -gt 0) {
        return $candidates[0]
    }

    return $null
}

if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "codesign.ps1: file not found: $Path"
}

$thumbprint = $env:KILLENGINE_CODESIGN_THUMBPRINT
$pfxPath = $env:KILLENGINE_CODESIGN_PFX
$pfxPassword = $env:KILLENGINE_CODESIGN_PFX_PASSWORD

$hasCertSource = -not [string]::IsNullOrWhiteSpace($thumbprint) -or -not [string]::IsNullOrWhiteSpace($pfxPath)

if (-not $hasCertSource) {
    $message = "codesign.ps1: no code signing certificate configured (KILLENGINE_CODESIGN_THUMBPRINT or KILLENGINE_CODESIGN_PFX) — '$Path' will ship UNSIGNED. See docs/CODE_SIGNING.md."
    if ($RequireSigning) {
        throw $message
    }
    Write-Warning $message
    return [pscustomobject]@{ Path = $Path; Signed = $false }
}

$signtool = Find-SignTool
if (-not $signtool) {
    $message = "codesign.ps1: signtool.exe not found (install the Windows 10/11 SDK) — '$Path' will ship UNSIGNED."
    if ($RequireSigning) {
        throw $message
    }
    Write-Warning $message
    return [pscustomobject]@{ Path = $Path; Signed = $false }
}

$signArgs = @("sign", "/fd", "SHA256", "/tr", $TimestampUrl, "/td", "SHA256")

if (-not [string]::IsNullOrWhiteSpace($thumbprint)) {
    $signArgs += @("/sha1", $thumbprint)
} else {
    if ([string]::IsNullOrWhiteSpace($pfxPassword)) {
        throw "codesign.ps1: KILLENGINE_CODESIGN_PFX is set but KILLENGINE_CODESIGN_PFX_PASSWORD is missing."
    }
    $signArgs += @("/f", $pfxPath, "/p", $pfxPassword)
}

$signArgs += $Path

Write-Host "Signing $Path ..." -ForegroundColor Cyan
& $signtool @signArgs
if ($LASTEXITCODE -ne 0) {
    throw "codesign.ps1: signtool failed with exit code $LASTEXITCODE for '$Path'."
}

$signature = Get-AuthenticodeSignature -LiteralPath $Path
if ($signature.Status -ne "Valid") {
    throw "codesign.ps1: signature on '$Path' is not valid after signing (status: $($signature.Status))."
}

Write-Host "Signed OK: $Path (subject: $($signature.SignerCertificate.Subject))" -ForegroundColor Green
return [pscustomobject]@{ Path = $Path; Signed = $true }
