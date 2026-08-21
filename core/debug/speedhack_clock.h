#pragma once

#include <cstdint>

namespace killcore {

// Horloge virtuelle a delta scale : convertit un compteur reel monotone
// (QueryPerformanceCounter/GetTickCount/...) en compteur "virtuel" accelere ou
// ralenti par `factor`, sans jamais sauter de valeur au moment de
// l'activation ou d'un changement de facteur en direct — seul le PROCHAIN
// delta est scale par le facteur courant, jamais le passe deja ecoule.
//
// Header-only et sans dependance Windows/Qt : inclus tel quel a la fois par
// speedhack.cpp (cote KillEngine, pour le test unitaire pur) et par
// speedhack_handler.cpp (DLL injectee dans la cible, ou tournent les 4
// fonctions hookees) — une seule implementation, pas de duplication.
//
// L'etat {realBase, virtualBase} vit cote appelant (un exemplaire par
// fonction hookee) ; cette fonction est pure. Apres chaque appel, le site
// appelant doit faire realBase = realNow; virtualBase = <retour>.
inline int64_t scaleClockDelta(int64_t realBase, int64_t realNow, int64_t virtualBase, double factor) {
    const int64_t realDelta = realNow - realBase;
    const int64_t virtualDelta = static_cast<int64_t>(static_cast<double>(realDelta) * factor);
    return virtualBase + virtualDelta;
}

} // namespace killcore
