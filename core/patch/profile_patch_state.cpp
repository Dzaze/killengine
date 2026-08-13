#include "patch/profile_patch_state.h"

namespace killcore {

ProfilePatchMemoryState classifyProfilePatchMemoryState(
    int originalMatches,
    int patchedMatches,
    bool sessionActive,
    bool patternsValid) {

    ProfilePatchMemoryState state;

    if (!patternsValid) {
        state.status = "invalid";
        state.success = false;
        state.active = false;
        return state;
    }

    if (sessionActive || (patchedMatches == 1 && originalMatches == 0)) {
        state.status = "active";
        state.success = true;
        state.active = true;
        return state;
    }

    if (originalMatches == 1 && patchedMatches == 0) {
        state.status = "original";
        state.success = true;
        state.active = false;
        return state;
    }

    if (originalMatches == 0 && patchedMatches == 0) {
        state.status = "missing";
        state.success = false;
        state.active = false;
        return state;
    }

    state.status = "ambiguous";
    state.success = true;
    state.active = false;
    return state;
}

} // namespace killcore
