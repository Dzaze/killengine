#pragma once

#include <QString>

namespace killcore {

struct ProfilePatchMemoryState {
    QString status;
    bool success = false;
    bool active = false;
};

ProfilePatchMemoryState classifyProfilePatchMemoryState(
    int originalMatches,
    int patchedMatches,
    bool sessionActive,
    bool patternsValid);

} // namespace killcore
