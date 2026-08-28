#include "ghidra_bridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace killcore {
namespace {

QString hexValue(uint64_t value) {
    return QString::number(value, 16);
}

QString locatorKindString(LocatorKind kind) {
    switch (kind) {
        case LocatorKind::Absolute: return "absolute";
        case LocatorKind::ModuleOffset: return "module_offset";
        case LocatorKind::PointerChain: return "pointer_chain";
        case LocatorKind::ClrField: return "clr_field";
    }
    return "unknown";
}

bool staticAnchorForTarget(const ProfileTarget& target, QString* module, uint64_t* offset) {
    if (!module || !offset) return false;
    if (target.locator.kind == LocatorKind::ModuleOffset) {
        *module = target.locator.module;
        *offset = target.locator.offset;
        return !module->trimmed().isEmpty();
    }
    if (target.locator.kind == LocatorKind::PointerChain) {
        *module = target.locator.pointerChain.module;
        *offset = target.locator.pointerChain.baseOffset;
        return !module->trimmed().isEmpty();
    }
    return false;
}

QJsonArray stringListToJson(const QStringList& values) {
    QJsonArray out;
    for (const auto& value : values) {
        if (!value.trimmed().isEmpty()) {
            out.append(value.trimmed());
        }
    }
    return out;
}

QJsonObject targetArtifact(const ProfileTarget& target) {
    QString module;
    uint64_t offset = 0;
    const bool addressable = staticAnchorForTarget(target, &module, &offset);

    QJsonObject artifact;
    artifact["category"] = "target";
    artifact["name"] = target.name;
    artifact["valueType"] = valueTypeToString(target.type);
    artifact["locatorKind"] = locatorKindString(target.locator.kind);
    artifact["module"] = module;
    artifact["offset"] = addressable ? hexValue(offset) : QString();
    artifact["lastAddress"] = target.locator.lastAddress ? hexValue(target.locator.lastAddress) : QString();
    artifact["note"] = target.description;
    artifact["ghidraSymbol"] = target.ghidraSymbol;
    artifact["ghidraNote"] = target.ghidraNote;
    artifact["ghidraAddressable"] = addressable;
    if (!target.dependsOn.isEmpty()) {
        artifact["dependsOn"] = stringListToJson(target.dependsOn);
    }
    if (target.locator.kind == LocatorKind::PointerChain) {
        const auto& chain = target.locator.pointerChain;
        QJsonObject chainJson;
        chainJson["module"] = chain.module;
        chainJson["baseOffset"] = hexValue(chain.baseOffset);
        QJsonArray offsets;
        for (uint64_t item : chain.offsets) {
            offsets.append(hexValue(item));
        }
        chainJson["offsets"] = offsets;
        artifact["pointerChain"] = chainJson;
    }
    if (target.locator.kind == LocatorKind::ClrField) {
        QJsonObject clr;
        clr["typeSubstring"] = target.locator.clrField.typeSubstring;
        clr["identityField"] = target.locator.clrField.identityField;
        clr["identityValue"] = target.locator.clrField.identityValue;
        clr["targetField"] = target.locator.clrField.targetField;
        artifact["clrField"] = clr;
    }
    return artifact;
}

QJsonObject patchArtifact(const ProfileCodePatch& patch) {
    QJsonObject artifact;
    artifact["category"] = "patch";
    artifact["name"] = patch.name;
    artifact["module"] = patch.module;
    artifact["offset"] = hexValue(patch.moduleOffset);
    artifact["aobPattern"] = patch.aobPattern;
    artifact["patchBytes"] = patch.patchBytes;
    artifact["originalBytes"] = patch.originalBytes;
    artifact["disassembly"] = patch.disassembly;
    artifact["riskLevel"] = patch.riskLevel;
    artifact["note"] = patch.description;
    artifact["ghidraSymbol"] = patch.ghidraSymbol;
    artifact["ghidraNote"] = patch.ghidraNote;
    artifact["ghidraAddressable"] = !patch.module.trimmed().isEmpty() && patch.moduleOffset > 0;

    QJsonObject quality;
    quality["score"] = patch.signatureScore;
    quality["level"] = patch.signatureLevel;
    quality["warning"] = patch.signatureWarning;
    quality["fixedBytes"] = patch.signatureFixedBytes;
    quality["wildcardBytes"] = patch.signatureWildcardBytes;
    quality["uniqueFixedBytes"] = patch.signatureUniqueFixedBytes;
    quality["fixedRatio"] = patch.signatureFixedRatio;
    quality["trainerSafe"] = patch.trainerSafe;
    quality["matches"] = patch.signatureMatches;
    artifact["signatureQuality"] = quality;
    return artifact;
}

QString csvCell(const QStringList& cells, int index) {
    if (index < 0 || index >= cells.size()) return QString();
    QString cell = cells[index].trimmed();
    if (cell.size() >= 2 && cell.front() == '"' && cell.back() == '"') {
        cell = cell.mid(1, cell.size() - 2);
    }
    return cell;
}

QStringList splitCsvLine(const QString& line) {
    QStringList cells;
    QString cell;
    bool quoted = false;
    for (QChar ch : line) {
        if (ch == '"') {
            quoted = !quoted;
        } else if (ch == ',' && !quoted) {
            cells.append(cell);
            cell.clear();
        } else {
            cell.append(ch);
        }
    }
    cells.append(cell);
    return cells;
}

struct ImportedSymbol {
    QString module;
    uint64_t offset{0};
    QString name;
    QString comment;
};

bool parseOffset(const QString& text, uint64_t* offset) {
    if (!offset) return false;
    QString clean = text.trimmed();
    if (clean.isEmpty()) return false;
    if (clean.startsWith("0x", Qt::CaseInsensitive)) {
        clean = clean.mid(2);
    }
    bool ok = false;
    const auto parsed = clean.toULongLong(&ok, 16);
    if (!ok) return false;
    *offset = parsed;
    return true;
}

bool parseModuleOffsetOrAddress(const QString& offsetText,
                                const QString& addressText,
                                const QString& imageBaseText,
                                uint64_t* offset) {
    if (parseOffset(offsetText, offset)) return true;

    uint64_t address = 0;
    if (!parseOffset(addressText, &address)) return false;

    uint64_t imageBase = 0;
    if (parseOffset(imageBaseText, &imageBase) && address >= imageBase) {
        *offset = address - imageBase;
        return true;
    }

    *offset = address;
    return true;
}

QList<ImportedSymbol> parseJsonSymbols(const QJsonDocument& doc) {
    QJsonArray symbols;
    QString rootImageBase;
    if (doc.isArray()) {
        symbols = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        rootImageBase = root.value("imageBase").toString();
        symbols = root.value("symbols").toArray();
        if (symbols.isEmpty()) {
            symbols = root.value("artifacts").toArray();
        }
    }

    QList<ImportedSymbol> out;
    for (const auto& item : symbols) {
        const QJsonObject obj = item.toObject();
        ImportedSymbol symbol;
        symbol.module = obj.value("module").toString();
        symbol.name = obj.value("name").toString(obj.value("symbol").toString(obj.value("label").toString()));
        symbol.comment = obj.value("comment").toString(obj.value("note").toString());
        const QString offsetText = obj.value("offset").toString(obj.value("moduleOffset").toString());
        const QString addressText = obj.value("address").toString();
        const QString imageBaseText = obj.value("imageBase").toString(rootImageBase);
        if (!symbol.name.trimmed().isEmpty()
            && parseModuleOffsetOrAddress(offsetText, addressText, imageBaseText, &symbol.offset)) {
            out.append(symbol);
        }
    }
    return out;
}

QList<ImportedSymbol> parseCsvSymbols(const QString& text) {
    QList<ImportedSymbol> out;
    const QStringList lines = text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    int moduleIndex = 0;
    int offsetIndex = 1;
    int addressIndex = -1;
    int imageBaseIndex = -1;
    int nameIndex = 2;
    int commentIndex = 3;
    bool first = true;
    for (const auto& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QStringList cells = splitCsvLine(line);
        if (first) {
            first = false;
            const QString header = cells.join(",").toLower();
            if ((header.contains("offset") || header.contains("address")) && (header.contains("name") || header.contains("symbol"))) {
                auto findHeader = [&](const QStringList& names, int fallback) {
                    for (int i = 0; i < cells.size(); ++i) {
                        const QString cell = cells[i].trimmed().toLower();
                        if (names.contains(cell)) return i;
                    }
                    return fallback;
                };
                moduleIndex = findHeader({"module", "module_name", "image"}, moduleIndex);
                offsetIndex = findHeader({"offset", "moduleoffset", "module_offset", "rva"}, offsetIndex);
                addressIndex = findHeader({"address", "addr", "va"}, addressIndex);
                imageBaseIndex = findHeader({"imagebase", "image_base", "base"}, imageBaseIndex);
                nameIndex = findHeader({"name", "symbol", "label", "function"}, nameIndex);
                commentIndex = findHeader({"comment", "note", "description"}, commentIndex);
                continue;
            }
        }

        ImportedSymbol symbol;
        symbol.module = csvCell(cells, moduleIndex);
        symbol.name = csvCell(cells, nameIndex);
        symbol.comment = csvCell(cells, commentIndex);
        if (!symbol.name.isEmpty()
            && parseModuleOffsetOrAddress(csvCell(cells, offsetIndex),
                                          csvCell(cells, addressIndex),
                                          csvCell(cells, imageBaseIndex),
                                          &symbol.offset)) {
            out.append(symbol);
        }
    }
    return out;
}

bool moduleMatches(const QString& symbolModule, const QString& artifactModule) {
    return symbolModule.trimmed().isEmpty()
        || artifactModule.trimmed().isEmpty()
        || symbolModule.compare(artifactModule, Qt::CaseInsensitive) == 0;
}

void enrichDescription(QString* description, const QString& symbol, const QString& comment) {
    if (!description) return;
    const QString line = comment.trimmed().isEmpty()
        ? QString("[Ghidra] %1").arg(symbol.trimmed())
        : QString("[Ghidra] %1 - %2").arg(symbol.trimmed(), comment.trimmed());
    if (!description->contains(line)) {
        if (!description->trimmed().isEmpty()) {
            description->append("\n");
        }
        description->append(line);
    }
}

} // namespace

QJsonObject exportGhidraArtifacts(const Profile& profile) {
    QJsonArray artifacts;
    for (const auto& target : profile.targets) {
        artifacts.append(targetArtifact(target));
    }
    for (const auto& patch : profile.patches) {
        artifacts.append(patchArtifact(patch));
    }

    QJsonObject root;
    root["format"] = "killengine.ghidra_artifacts";
    root["formatVersion"] = 1;
    root["profileGameName"] = profile.gameName;
    root["executableName"] = profile.executableName;
    root["executableHash"] = profile.executableHash;
    root["artifactCount"] = artifacts.size();
    root["artifacts"] = artifacts;
    return root;
}

QString generateGhidraImportScript(const QJsonObject& artifactExport) {
    const QString json = QString::fromUtf8(QJsonDocument(artifactExport).toJson(QJsonDocument::Compact));
    QJsonArray encodedString;
    encodedString.append(json);
    QString pythonJsonLiteral = QString::fromUtf8(QJsonDocument(encodedString).toJson(QJsonDocument::Compact));
    pythonJsonLiteral = pythonJsonLiteral.mid(1).chopped(1);
    return QString(
        "# KillEngine -> Ghidra bridge import\n"
        "# Paste/run in Ghidra Script Manager while the matching program is open.\n"
        "import json\n"
        "from ghidra.program.model.listing import CodeUnit\n"
        "from ghidra.program.model.symbol import SourceType\n\n"
        "DATA = json.loads(%1)\n"
        "fm = currentProgram.getFunctionManager()\n"
        "st = currentProgram.getSymbolTable()\n"
        "listing = currentProgram.getListing()\n"
        "bookmarks = currentProgram.getBookmarkManager()\n"
        "image_base = currentProgram.getImageBase()\n\n"
        "for artifact in DATA.get('artifacts', []):\n"
        "    if not artifact.get('ghidraAddressable'):\n"
        "        continue\n"
        "    offset_text = str(artifact.get('offset') or '0')\n"
        "    offset = int(offset_text[2:] if offset_text.lower().startswith('0x') else offset_text, 16)\n"
        "    addr = image_base.add(offset)\n"
        "    label = artifact.get('ghidraSymbol') or artifact.get('name') or 'killengine_artifact'\n"
        "    safe_label = ''.join([c if c.isalnum() or c == '_' else '_' for c in label])\n"
        "    if safe_label and safe_label[0].isdigit():\n"
        "        safe_label = 'ke_' + safe_label\n"
        "    st.createLabel(addr, safe_label[:80], SourceType.USER_DEFINED)\n"
        "    note = artifact.get('ghidraNote') or artifact.get('note') or ''\n"
        "    if artifact.get('aobPattern'):\n"
        "        note = (note + '\\n' if note else '') + 'KillEngine AOB: ' + artifact.get('aobPattern')\n"
        "    if artifact.get('patchBytes'):\n"
        "        note = (note + '\\n' if note else '') + 'KillEngine patch bytes: ' + artifact.get('patchBytes')\n"
        "    if note:\n"
        "        listing.setComment(addr, CodeUnit.EOL_COMMENT, note)\n"
        "    bookmarks.setBookmark(addr, 'KillEngine', artifact.get('category', 'artifact'), artifact.get('name', safe_label))\n"
        "print('Imported %d KillEngine artifacts' % len(DATA.get('artifacts', [])))\n")
        .arg(pythonJsonLiteral);
}

bool importGhidraSymbols(Profile* profile, const QByteArray& data, GhidraSymbolImportResult* result, QString* error) {
    if (!profile) {
        if (error) *error = "Profil nul.";
        return false;
    }
    GhidraSymbolImportResult local;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    const QList<ImportedSymbol> symbols = parseError.error == QJsonParseError::NoError
        ? parseJsonSymbols(doc)
        : parseCsvSymbols(QString::fromUtf8(data));
    if (symbols.isEmpty()) {
        if (error) *error = "Aucun symbole Ghidra lisible (JSON symbols[] ou CSV module,offset,name,comment attendu).";
        return false;
    }

    for (const auto& symbol : symbols) {
        ++local.symbolsRead;
        bool matched = false;
        for (auto& target : profile->targets) {
            QString module;
            uint64_t offset = 0;
            if (staticAnchorForTarget(target, &module, &offset)
                && offset == symbol.offset
                && moduleMatches(symbol.module, module)) {
                target.ghidraSymbol = symbol.name.trimmed();
                target.ghidraNote = symbol.comment.trimmed();
                enrichDescription(&target.description, target.ghidraSymbol, target.ghidraNote);
                ++local.targetsUpdated;
                matched = true;
            }
        }
        for (auto& patch : profile->patches) {
            if (patch.moduleOffset == symbol.offset && moduleMatches(symbol.module, patch.module)) {
                patch.ghidraSymbol = symbol.name.trimmed();
                patch.ghidraNote = symbol.comment.trimmed();
                enrichDescription(&patch.description, patch.ghidraSymbol, patch.ghidraNote);
                ++local.patchesUpdated;
                matched = true;
            }
        }
        if (!matched) {
            ++local.unmatched;
            local.messages.append(QString("Symbole sans correspondance : %1+0x%2 %3")
                .arg(symbol.module, hexValue(symbol.offset), symbol.name));
        }
    }

    if (result) *result = local;
    return true;
}

} // namespace killcore
