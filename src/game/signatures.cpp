#include "sco/game/signatures.h"
#include "sco/signatures.h"
#include <cstddef>

namespace sco::game {

extern const SigDef kTeleportSignatures[];
extern const size_t kTeleportSignatureCount;
extern const SigDef kSystemSignatures[];
extern const size_t kSystemSignatureCount;
extern const SigDef kPakSignatures[];
extern const size_t kPakSignatureCount;

bool RegisterGameSignatures() {
    static int done = 0;   // 1 = registered, -1 = failed
    if (done) return done > 0;
    const bool ok = RegisterSignatures(kTeleportSignatures, kTeleportSignatureCount)
                 && RegisterSignatures(kSystemSignatures, kSystemSignatureCount)
                 && RegisterSignatures(kPakSignatures, kPakSignatureCount);
    done = ok ? 1 : -1;
    return ok;
}

}  // namespace sco::game
