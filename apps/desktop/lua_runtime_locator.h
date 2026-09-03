#pragma once

#include <QString>

namespace killengine {

/// Cherche un interprete Lua (lua.exe/lua54.exe/lua5.4.exe/luajit.exe) --
/// d'abord `overridePath` si fourni, puis les dossiers bundles a cote de
/// KillEngine.exe (`runtime/lua`, etc.) et du repertoire courant, puis le
/// PATH systeme. Retourne une chaine vide si rien n'est trouve. Extrait
/// d'`application_controller.cpp` (executeLuaScript) pour etre partage avec
/// `LuaReplManager` (Live Lua REPL) sans dupliquer la recherche.
QString findLuaExecutable(const QString& overridePath = {});

/// Cherche `scripts/killengine.lua` (le module `ke.*` requis par les
/// scripts/le REPL) a cote de KillEngine.exe ou du repertoire courant.
/// Retourne une chaine vide si introuvable.
QString findKillEngineLuaHelper();

} // namespace killengine
