# Patch P1-7 : test unitaire positif pour le group scan + build UI
$ErrorActionPreference = 'Stop'

# ---- 1. Test group scan positif ----
$f = 'tests\unit\test_power_up_modules.cpp'
$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
$crlfCount = ([regex]::Matches($text, "`r`n")).Count
$lfOnly = ([regex]::Matches($text, "(?<!`r)`n")).Count
$nl = if ($crlfCount -gt $lfOnly) { "`r`n" } else { "`n" }

if ($text -notmatch 'FindsStructureWithTwoNeighbourValues') {
    $anchor = 'TEST(AutoAssembler, RejectsUnsupportedSyntax) {'
    if (-not $text.Contains($anchor)) { throw "Ancre AutoAssembler test introuvable" }
    $newTest = @'
TEST(EncryptedScan, FindsStructureWithTwoNeighbourValues) {
    // Simule une structure joueur : HP=100 (Int32 @+0), Mana=50 (Int32 @+4)
    QByteArray buffer(64, '\0');
    int32_t hp = 100;
    int32_t mana = 50;
    std::memcpy(buffer.data() + 8, &hp, sizeof(hp));
    std::memcpy(buffer.data() + 12, &mana, sizeof(mana));

    GroupScanOptions options;
    GroupScanEntry hpEntry;
    hpEntry.offset = 8;
    hpEntry.type = ValueType::Int32;
    hpEntry.value = 100;
    GroupScanEntry manaEntry;
    manaEntry.offset = 12;
    manaEntry.type = ValueType::Int32;
    manaEntry.value = 50;
    options.entries.append(hpEntry);
    options.entries.append(manaEntry);
    options.maxDistance = 64;

    const auto result = scanGroupInBuffer(buffer, 0x2000, options);

    ASSERT_TRUE(result.success) << result.error.toStdString();
    ASSERT_EQ(result.matches.size(), 1);
    // L'adresse rapportee correspond au premier offset (minOffset=8)
    EXPECT_EQ(result.matches[0].address, 0x2000u + 8u);
}

'@
    $newTest = $newTest.Replace("`r`n", "`n").Replace("`n", $nl)
    # verifier que <cstring> est inclus pour memcpy
    if ($text -notmatch '#include <cstring>') {
        $incAnchor = '#include <QByteArray>'
        if ($text.Contains($incAnchor)) {
            $text = $text.Replace($incAnchor, $incAnchor + $nl + '#include <cstring>')
        }
    }
    $text = $text.Replace($anchor, $newTest + $anchor)
    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text, $encoding)
    Write-Host "OK: test group scan positif ajoute"
} else {
    Write-Host "SKIP: test deja present"
}

# ---- 2. Build UI (vue-tsc passe deja, construire le bundle) ----
Set-Location ui
& npm run build 2>&1 | Select-Object -Last 8
Set-Location ..