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
extern const SigDef kAsopSignatures[];
extern const size_t kAsopSignatureCount;
extern const SigDef kAtcSignatures[];
extern const size_t kAtcSignatureCount;
extern const SigDef kHangarSignatures[];
extern const size_t kHangarSignatureCount;
extern const SigDef kFeatureSignatures[];
extern const size_t kFeatureSignatureCount;
extern const SigDef kOfflineSignatures[];
extern const size_t kOfflineSignatureCount;
extern const SigDef kContractsSignatures[];
extern const size_t kContractsSignatureCount;
extern const SigDef kActorsSignatures[];
extern const size_t kActorsSignatureCount;
extern const SigDef kWorldSignatures[];
extern const size_t kWorldSignatureCount;

bool RegisterGameSignatures() {
    static int done = 0;   // 1 = registered, -1 = failed
    if (done) return done > 0;
    const bool ok = RegisterSignatures(kTeleportSignatures, kTeleportSignatureCount)
                 && RegisterSignatures(kSystemSignatures, kSystemSignatureCount)
                 && RegisterSignatures(kPakSignatures, kPakSignatureCount)
                 && RegisterSignatures(kAsopSignatures, kAsopSignatureCount)
                 && RegisterSignatures(kAtcSignatures, kAtcSignatureCount)
                 && RegisterSignatures(kHangarSignatures, kHangarSignatureCount)
                 && RegisterSignatures(kFeatureSignatures, kFeatureSignatureCount)
                 && RegisterSignatures(kOfflineSignatures, kOfflineSignatureCount)
                 && RegisterSignatures(kContractsSignatures, kContractsSignatureCount)
                 && RegisterSignatures(kActorsSignatures, kActorsSignatureCount)
                 && RegisterSignatures(kWorldSignatures, kWorldSignatureCount);
    done = ok ? 1 : -1;
    return ok;
}

}  // namespace sco::game
