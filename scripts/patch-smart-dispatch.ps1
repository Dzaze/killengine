# Patch Smart Search dispatcher: supporter auto_resolve, encrypted_scan, trace_ui_string
$ErrorActionPreference = 'Stop'
$path = 'apps\desktop\application_controller.cpp'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

$old = @'
    } else if (tool == "unknown_compare") {
        actionResult = unknownNextScan(args.value("mode").toString(), args.value("valueType").toString());
    } else if (tool == "write_value" || tool == "freeze_value") {
'@

$new = @'
    } else if (tool == "unknown_compare") {
        actionResult = unknownNextScan(args.value("mode").toString(), args.value("valueType").toString());
    } else if (tool == "auto_resolve") {
        QVariantMap autoOptions;
        autoOptions["executeSafe"] = true;
        if (!explicitValueType.isEmpty()) autoOptions["valueType"] = defaultValueType;
        actionResult = startAutoResolve(args.value("query").toString(), autoOptions);
        if (!actionResult.value("message").toString().isEmpty()) {
            result["message"] = actionResult.value("message");
        }
        if (!actionResult.value("executedSafeSteps").isNull()) {
            result["executedSafeSteps"] = actionResult.value("executedSafeSteps");
        }
    } else if (tool == "encrypted_scan") {
        QVariantMap encryptedOptions;
        encryptedOptions["mode"] = args.value("mode", "xor");
        encryptedOptions["key"] = args.value("key", "0");
        encryptedOptions["keySearchBits"] = args.value("keySearchBits", 16);
        actionResult = scanEncryptedValue(
            args.value("value").toString(),
            args.value("valueType", explicitValueType.isEmpty() ? QString("Int32") : defaultValueType).toString(),
            encryptedOptions);
        if (actionResult.value("success").toBool()) {
            const auto encryptedMatches = actionResult.value("matchesFound").toULongLong();
            result["workflowStatus"] = encryptedMatches > 0 ? "awaiting_value_change" : "no_candidate";
            result["message"] = encryptedMatches > 0
                ? QString("Scan chiffré : %1 adresse(s) correspondent à %2 sous une forme chiffrée (XOR/Add/Sub). Fais varier la valeur puis redonne-la moi pour affiner.")
                      .arg(encryptedMatches)
                      .arg(args.value("value").toString())
                : QString("Scan chiffré : aucune adresse ne correspond à %1 sous forme chiffrée. On peut tenter la Trace UI string ou Unknown.")
                      .arg(args.value("value").toString());
        }
    } else if (tool == "trace_ui_string") {
        QVariantMap traceOptions;
        traceOptions["ascii"] = true;
        traceOptions["utf16"] = true;
        traceOptions["writableOnly"] = true;
        actionResult = scanUiStrings(args.value("value").toString(), traceOptions);
        if (actionResult.value("success").toBool()) {
            const auto stringsFound = actionResult.value("matchesFound").toULongLong();
            result["workflowStatus"] = stringsFound > 0 ? "awaiting_value_change" : "no_candidate";
            result["message"] = stringsFound > 0
                ? QString("Trace UI string : %1 occurrence(s) du texte \"%2\" trouvées en mémoire. J'ai chargé les pistes dans Expert > Trace UI string. Utilise Filtrer strings après avoir changé la valeur, puis Analyser sources.")
                      .arg(stringsFound)
                      .arg(args.value("value").toString())
                : QString("Trace UI string : le texte \"%1\" n'a pas été trouvé en mémoire. Vérifie la valeur affichée exacte, ou passe en Unknown.")
                      .arg(args.value("value").toString());
            if (!actionResult.value("candidates").isNull()) {
                result["uiStringCandidates"] = actionResult.value("candidates");
            }
        }
    } else if (tool == "write_value" || tool == "freeze_value") {
'@

if (-not $content.Contains($old)) { Write-Host 'DISPATCH ANCHOR NOT FOUND'; exit 1 }
$content = $content.Replace($old, $new)
[System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding($false)))
Write-Host 'Smart Search dispatcher patched'