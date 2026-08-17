# Patch P1-1 : ajoute findWhatAccesses (Find What Accesses) au core hardware_breakpoint
$ErrorActionPreference = 'Stop'

function Read-TextPreserveBom([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    return @{ Text = $text; HasBom = $hasBom }
}

function Write-TextPreserveBom([string]$Path, [string]$Text, [bool]$HasBom) {
    $encoding = New-Object System.Text.UTF8Encoding($HasBom)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

# Normalise une chaine en CRLF (fichiers sources mixtes LF/CRLF)
function Normalize-Crlf([string]$Text) {
    return $Text.Replace("`r`n", "`n").Replace("`n", "`r`n")
}

# --- 1. hardware_breakpoint.h : declarations ---
$hPath = 'core\debug\hardware_breakpoint.h'
$h = Read-TextPreserveBom $hPath
if ($h.Text -notmatch 'findWhatAccesses') {
    $h.Text = Normalize-Crlf $h.Text
    $anchor = @'
    const CancellationToken* cancellation);

} // namespace killcore
'@
    $replacement = @'
    const CancellationToken* cancellation);

/**
 * @brief Utilitaire de haut niveau : trouve les instructions qui lisent une adresse.
 *
 * Variante "Find What Accesses" : pose un breakpoint Access (lecture/ecriture).
 * C'est souvent l'instruction qui LIT la valeur qui revele la structure proprietaire
 * (boucle de rendu UI, calcul gameplay), pas celle qui l'ecrit.
 */
QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size = BreakpointSize::DWord,
    int timeoutMs = 5000,
    size_t maxHits = 10);

QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation);

} // namespace killcore
'@
    if (-not $h.Text.Contains($anchor)) { throw "Ancre introuvable dans $hPath" }
    $h.Text = $h.Text.Replace($anchor, $replacement)
    Write-TextPreserveBom $hPath $h.Text $h.HasBom
    Write-Host "OK: findWhatAccesses declarations ajoutees a $hPath"
} else {
    Write-Host "SKIP: findWhatAccesses deja present dans $hPath"
}

# --- 2. hardware_breakpoint.cpp : implementation ---
$cPath = 'core\debug\hardware_breakpoint.cpp'
$c = Read-TextPreserveBom $cPath
if ($c.Text -notmatch 'QList<BreakpointHit> findWhatAccesses') {
    $c.Text = Normalize-Crlf $c.Text
    $anchorCpp = @'
    hits = session.takeHits();
    session.detach();
#endif

    return hits;
}

} // namespace killcore
'@
    $implCpp = @'
    hits = session.takeHits();
    session.detach();
#endif

    return hits;
}

namespace {

QList<BreakpointHit> findWithBreakpointType(
    uint32_t pid,
    uint64_t address,
    BreakpointType breakpointType,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation) {

    QList<BreakpointHit> hits;

#ifdef Q_OS_WIN
    HardwareBreakpointSession session;

    if (!session.attach(pid)) {
        return hits;
    }

    BreakpointConfig config;
    config.address = address;
    config.type = breakpointType;
    config.size = size;

    const int slot = session.setBreakpoint(config);
    if (slot < 0) {
        session.detach();
        return hits;
    }

    std::atomic_bool watcherDone{false};
    std::thread cancellationWatcher;
    if (cancellation) {
        cancellationWatcher = std::thread([&session, cancellation, &watcherDone]() {
            while (!watcherDone.load()) {
                if (cancellation->isCancelled()) {
                    session.stopMonitoring();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    session.monitorBlocking(maxHits, timeoutMs);
    watcherDone.store(true);
    if (cancellationWatcher.joinable()) {
        cancellationWatcher.join();
    }
    hits = session.takeHits();
    session.detach();
#else
    (void)pid;
    (void)address;
    (void)breakpointType;
    (void)size;
    (void)timeoutMs;
    (void)maxHits;
    (void)cancellation;
#endif

    return hits;
}

} // namespace

QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits) {
    return findWhatAccesses(pid, address, size, timeoutMs, maxHits, nullptr);
}

QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation) {
    return findWithBreakpointType(
        pid,
        address,
        BreakpointType::Access,
        size,
        timeoutMs,
        maxHits,
        cancellation);
}

} // namespace killcore
'@
    if (-not $c.Text.Contains($anchorCpp)) { throw "Ancre introuvable dans $cPath" }
    $c.Text = $c.Text.Replace($anchorCpp, $implCpp)
    Write-TextPreserveBom $cPath $c.Text $c.HasBom
    Write-Host "OK: findWhatAccesses implementation ajoutee a $cPath"
} else {
    Write-Host "SKIP: findWhatAccesses deja present dans $cPath"
}