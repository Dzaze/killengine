# KillEngine — Script de build de l'UI Vue
# Usage: .\scripts\build_ui.ps1

$ErrorActionPreference = "Stop"

Write-Host "Building KillEngine UI..." -ForegroundColor Cyan

Push-Location ui

if (-not (Test-Path "node_modules")) {
    Write-Host "Installing npm dependencies..." -ForegroundColor Yellow
    npm install
    if ($LASTEXITCODE -ne 0) {
        Write-Host "npm install FAILED!" -ForegroundColor Red
        Pop-Location
        exit 1
    }
}

Write-Host "Building Vue app..." -ForegroundColor Yellow
npm run build
if ($LASTEXITCODE -ne 0) {
    Write-Host "UI build FAILED!" -ForegroundColor Red
    Pop-Location
    exit 1
}

Pop-Location

Write-Host "`nUI build successful! Output: ui/dist/" -ForegroundColor Green
