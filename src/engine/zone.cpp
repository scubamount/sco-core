// The zone tree (sco/engine/zone.h): a map of zones and walks up their parent chains.
#include "sco/engine/zone.h"
#include <mutex>

namespace sco::engine {

bool ZoneTree::Set(uint64_t id, uint64_t parentId, std::string_view name, const engine::Transform& local) {
    if (id == 0 || parentId == id) return false;
    std::unique_lock lock(mu_);
    Zone& z = zones_[id];
    z.id = id;
    z.parentId = parentId;
    z.name.assign(name.data(), name.size());
    z.local = local;
    return true;
}

bool ZoneTree::Remove(uint64_t id) {
    std::unique_lock lock(mu_);
    return zones_.erase(id) != 0;
}

void ZoneTree::Clear() {
    std::unique_lock lock(mu_);
    zones_.clear();
}

bool ZoneTree::Has(uint64_t id) const {
    std::shared_lock lock(mu_);
    return zones_.find(id) != zones_.end();
}

bool ZoneTree::Find(uint64_t id, Zone* out) const {
    if (!out) return false;
    std::shared_lock lock(mu_);
    auto it = zones_.find(id);
    if (it == zones_.end()) return false;
    *out = it->second;
    return true;
}

size_t ZoneTree::Size() const {
    std::shared_lock lock(mu_);
    return zones_.size();
}

bool ZoneTree::ChainLocked(uint64_t id, const Zone* chain[kMaxDepth], int* n) const {
    *n = 0;
    while (id != 0) {
        if (*n == kMaxDepth) return false;   // too deep, or a cycle
        auto it = zones_.find(id);
        if (it == zones_.end()) return false;
        chain[(*n)++] = &it->second;
        id = it->second.parentId;
    }
    return true;
}

bool ZoneTree::LocalToWorld(uint64_t id, const Vector3d& local, Vector3d* out) const {
    return Transform(id, 0, local, out);
}

bool ZoneTree::WorldToLocal(uint64_t id, const Vector3d& world, Vector3d* out) const {
    return Transform(0, id, world, out);
}

bool ZoneTree::Transform(uint64_t fromId, uint64_t toId, const Vector3d& pos, Vector3d* out) const {
    if (!out) return false;
    const Zone* up[kMaxDepth];
    const Zone* down[kMaxDepth];
    int nUp = 0, nDown = 0;
    std::shared_lock lock(mu_);
    if (!ChainLocked(fromId, up, &nUp) || !ChainLocked(toId, down, &nDown)) return false;
    // Both chains end at their root; drop the shared tail (the common ancestors).
    while (nUp > 0 && nDown > 0 && up[nUp - 1] == down[nDown - 1]) {
        --nUp;
        --nDown;
    }
    Vector3d p = pos;
    for (int i = 0; i < nUp; ++i) p = up[i]->local.TransformPoint(p);
    for (int i = nDown - 1; i >= 0; --i) p = down[i]->local.InverseTransformPoint(p);
    *out = p;
    return true;
}

}  // namespace sco::engine
