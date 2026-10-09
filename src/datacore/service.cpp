// sco::datacore::service (sco/datacore_service.h): the host service "sco.datacore" 1.1
// (include/sco_datacore.h) and the DataCore load. docs/design/vfs-datacore.md section 6 and
// decision 9: patches committed before the load apply at it; after it, commit saves the patch as
// <dataRoot>/datacore/pending/<plugin id>.toml and it applies from the next launch.
//
// One lock (g_lock) guards every patch; table calls hold it briefly. Load takes the queued patches
// under it, applies them without it, and stores the reports under it again. File writes (commit
// after the load) happen outside it too.
#include "sco/datacore_service.h"
#include "sco/host.h"
#include "sco/log.h"
#include "../api/internal.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace sco::datacore::service {

namespace {

struct Entry {
    uint32_t    state = SCO_DC_QUEUED;
    std::string reason;
};

struct PatchRec {
    enum class Stage { Open, Queued, Taken, Saved, Done };
    const void* owner = nullptr;
    std::string plugin;
    bool        atomic = true;
    Stage       stage = Stage::Open;
    std::vector<PackOp> ops;
    std::map<uint64_t, std::string> instances;   // instance and record ids -> local id in the pack
    std::set<std::string> fieldsSet;             // target + field of every set, to refuse a second one
    std::vector<Entry> results;                  // after the load: one per op, then the patch's
};

std::mutex g_lock;
bool       g_started = false;
Options    g_opts;
uint32_t   g_state = SCO_DC_LOADED;
uint64_t   g_next = 1;   // patch and instance ids; never reused in the process
std::map<uint64_t, PatchRec> g_patches;

bool Utf8(const char* s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        int n = 0;
        if (*p < 0x80) n = 0;
        else if ((*p & 0xE0) == 0xC0 && *p >= 0xC2) n = 1;
        else if ((*p & 0xF0) == 0xE0) n = 2;
        else if ((*p & 0xF8) == 0xF0 && *p <= 0xF4) n = 3;
        else return false;
        ++p;
        for (int k = 0; k < n; ++k, ++p)
            if ((*p & 0xC0) != 0x80) return false;
    }
    return true;
}
bool Text(const char* s) { return s && *s && Utf8(s); }

// The patch for a new operation: NOT_FOUND unknown or released, BAD_ARG not open.
sco_result Open(uint64_t id, PatchRec*& out) {
    if (!g_started) return SCO_UNAVAILABLE;
    const auto it = g_patches.find(id);
    if (it == g_patches.end() || detail::Released(it->second.owner)) return SCO_NOT_FOUND;
    if (it->second.stage != PatchRec::Stage::Open) return SCO_BAD_ARG;
    if (it->second.ops.size() >= SCO_DC_MAX_OPS) return SCO_TOO_MANY;
    out = &it->second;
    return SCO_OK;
}

// record: a name, "guid:...", or "@<instance id>" (an instance this patch added).
bool Target(const PatchRec& p, const char* record, PackOp& op) {
    if (!Text(record)) return false;
    const std::string_view r(record);
    if (r.rfind("guid:", 0) == 0) {
        Guid g;
        if (!ParseGuid(r.substr(5), g)) return false;
        op.record = RecordRef{ g, {} };
        return true;
    }
    if (r[0] == '@') {
        char* end = nullptr;
        const unsigned long long n = std::strtoull(record + 1, &end, 10);
        const auto it = p.instances.find(n);
        if (!end || *end || r.size() < 2 || it == p.instances.end()) return false;
        op.instance = it->second;
        return true;
    }
    op.record = RecordRef{ std::nullopt, std::string(r) };
    return true;
}

bool Field(const char* field, PackOp& op) {
    if (!Text(field) || !CheckFieldPath(field)) return false;
    op.field = field;
    return true;
}

// A value into op.value or op.pointer. NULL v: a null pointer.
bool ValueOf(const PatchRec& p, const sco_dc_value* v, PackOp& op) {
    if (!v) { op.pointer.kind = PackPointer::Kind::Null; return true; }
    if (v->size < sizeof(sco_dc_value)) return false;
    switch (v->type) {
    case SCO_DC_BOOL:
        if (v->i != 0 && v->i != 1) return false;
        op.value = Value::OfBool(v->i != 0);
        return true;
    case SCO_DC_INT: op.value = Value::OfInt(v->i); return true;
    case SCO_DC_UINT: op.value = Value::OfUInt(v->u); return true;
    case SCO_DC_FLOAT: op.value = Value::OfFloat(v->f); return true;
    case SCO_DC_STRING:
        if (!v->s || !Utf8(v->s)) return false;
        op.value = Value::OfString(v->s);
        return true;
    case SCO_DC_GUID: {
        Guid g;
        if (!v->s || !ParseGuid(v->s, g)) return false;
        op.value = Value::OfGuid(g);
        return true;
    }
    case SCO_DC_ENUM:
        if (!Text(v->s)) return false;
        op.value = Value::OfEnum(v->s);
        return true;
    case SCO_DC_REF: {
        if (!Text(v->s)) return false;
        RecordRef r;
        const std::string_view s(v->s);
        if (s.rfind("guid:", 0) == 0) {
            Guid g;
            if (!ParseGuid(s.substr(5), g)) return false;
            r.guid = g;
        } else {
            r.name = std::string(s);
        }
        op.value = Value::OfRecord(std::move(r));
        return true;
    }
    case SCO_DC_NULL: op.pointer.kind = PackPointer::Kind::Null; return true;
    case SCO_DC_INSTANCE: {
        const auto it = p.instances.find(v->u);
        if (it == p.instances.end()) return false;
        op.pointer.kind = PackPointer::Kind::Local;
        op.pointer.local = it->second;
        return true;
    }
    default: return false;
    }
}

std::string TargetKey(const PackOp& op) {
    if (!op.instance.empty()) return "@" + op.instance;
    return op.record->name + "|" + (op.record->guid ? FormatGuid(*op.record->guid) : std::string());
}

// ---- the table -------------------------------------------------------------------------------

uint32_t T_state() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_state;
}

sco_result T_begin(sco_plugin* self, uint32_t flags, uint64_t* out) {
    if (out) *out = 0;
    const char* id = host::PluginId(self);
    if (!out || !id || (flags & ~SCO_DC_NON_ATOMIC)) return SCO_BAD_ARG;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started) return SCO_UNAVAILABLE;
    if (detail::Released(self)) return SCO_BAD_ARG;
    size_t mine = 0;
    for (const auto& [k, p] : g_patches)
        if (p.owner == self && (p.stage == PatchRec::Stage::Open || p.stage == PatchRec::Stage::Queued)) ++mine;
    if (mine >= SCO_DC_MAX_PATCHES) return SCO_TOO_MANY;
    PatchRec p;
    p.owner = self;
    p.plugin = id;
    p.atomic = !(flags & SCO_DC_NON_ATOMIC);
    const uint64_t pid = g_next++;
    g_patches.emplace(pid, std::move(p));
    *out = pid;
    return SCO_OK;
}

sco_result T_set(uint64_t patch, const char* record, const char* field, const sco_dc_value* v) {
    std::lock_guard<std::mutex> hold(g_lock);
    PatchRec* p = nullptr;
    if (const sco_result r = Open(patch, p); r != SCO_OK) return r;
    PackOp op;
    op.kind = PackOp::Kind::Set;
    if (!Target(*p, record, op) || !Field(field, op) || !ValueOf(*p, v, op)) return SCO_BAD_ARG;
    if (!p->fieldsSet.insert(TargetKey(op) + "\x1f" + op.field).second) return SCO_BAD_ARG;   // set once per patch
    p->ops.push_back(std::move(op));
    return SCO_OK;
}

sco_result T_add_instance(uint64_t patch, const char* type, const char* cloneRecord, const char* cloneField, uint64_t* out) {
    if (out) *out = 0;
    std::lock_guard<std::mutex> hold(g_lock);
    PatchRec* p = nullptr;
    if (const sco_result r = Open(patch, p); r != SCO_OK) return r;
    if (!out || !Text(type)) return SCO_BAD_ARG;
    PackOp op;
    op.kind = PackOp::Kind::Instance;
    op.type = type;
    if (cloneRecord) {
        PackOp target;
        if (!Target(*p, cloneRecord, target) || !target.record) return SCO_BAD_ARG;   // a record, not an added instance
        op.clone.record = *target.record;
        if (cloneField && *cloneField) {
            if (!Utf8(cloneField) || !CheckFieldPath(cloneField)) return SCO_BAD_ARG;
            op.clone.field = cloneField;
        }
    } else if (cloneField && *cloneField) {
        return SCO_BAD_ARG;
    }
    const uint64_t iid = g_next++;
    op.id = "i" + std::to_string(iid);
    p->instances.emplace(iid, op.id);
    p->ops.push_back(std::move(op));
    *out = iid;
    return SCO_OK;
}

sco_result T_set_pointer(uint64_t patch, const char* record, const char* field, uint64_t instance) {
    sco_dc_value v{};
    v.size = sizeof(v);
    v.type = SCO_DC_INSTANCE;
    v.u = instance;
    return T_set(patch, record, field, &v);
}

sco_result T_append(uint64_t patch, const char* record, const char* field, const sco_dc_value* v) {
    std::lock_guard<std::mutex> hold(g_lock);
    PatchRec* p = nullptr;
    if (const sco_result r = Open(patch, p); r != SCO_OK) return r;
    PackOp op;
    op.kind = PackOp::Kind::Append;
    if (!Target(*p, record, op) || !Field(field, op) || !ValueOf(*p, v, op)) return SCO_BAD_ARG;
    p->ops.push_back(std::move(op));
    return SCO_OK;
}

// A [[record]] operation: the new record's root gets an id like an added instance's ("@<id>" as a
// record argument, SCO_DC_INSTANCE as a value). Without guid, the pack's default applies: derived
// from the plugin id and the name, so a saved patch adds the same GUID at every launch.
sco_result T_add_record(uint64_t patch, const char* type, const char* name, const char* guid, const char* cloneRecord,
                        const char* filePath, uint64_t* out) {
    if (out) *out = 0;
    std::lock_guard<std::mutex> hold(g_lock);
    PatchRec* p = nullptr;
    if (const sco_result r = Open(patch, p); r != SCO_OK) return r;
    if (!out || !Text(type) || !Text(name)) return SCO_BAD_ARG;
    PackOp op;
    op.kind = PackOp::Kind::Record;
    op.type = type;
    RecordRef rec;
    rec.name = name;
    if (guid && *guid) {
        Guid g;
        if (!ParseGuid(guid, g) || g == Guid{}) return SCO_BAD_ARG;
        rec.guid = g;
    }
    PackOp clone;
    if (!Target(*p, cloneRecord, clone) || !clone.record) return SCO_BAD_ARG;   // required, and a record
    op.clone.record = *clone.record;
    if (filePath && *filePath) {
        if (!Utf8(filePath) || !PackRecordFileOk(filePath)) return SCO_BAD_ARG;
        op.file = filePath;
    }
    for (const PackOp& o : p->ops)   // a name or GUID this patch already adds
        if (o.kind == PackOp::Kind::Record && (o.record->name == rec.name || (rec.guid && o.record->guid == rec.guid))) return SCO_BAD_ARG;
    const uint64_t rid = g_next++;
    op.id = "r" + std::to_string(rid);
    op.record = std::move(rec);
    p->instances.emplace(rid, op.id);
    p->ops.push_back(std::move(op));
    *out = rid;
    return SCO_OK;
}

// Writes the pack text to <id>.toml through a temporary file renamed over it.
bool Save(const fs::path& dataRoot, const std::string& plugin, uint64_t patch, const std::string& text, std::string& error) {
    const fs::path file = PendingPath(dataRoot, plugin);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) { error = "can't create " + file.parent_path().string() + ": " + ec.message(); return false; }
    fs::path tmp = file;
    tmp += "." + std::to_string(patch) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = "can't write " + tmp.string();
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        error = "can't replace " + file.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

sco_result T_commit(uint64_t patch) {
    Pack pack;
    fs::path root;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return SCO_UNAVAILABLE;
        const auto it = g_patches.find(patch);
        if (it == g_patches.end() || detail::Released(it->second.owner)) return SCO_NOT_FOUND;
        PatchRec& p = it->second;
        if (p.stage != PatchRec::Stage::Open) return SCO_BAD_ARG;
        if (g_state == SCO_DC_OPEN) {
            p.stage = PatchRec::Stage::Queued;
            return SCO_OK;
        }
        pack.plugin = p.plugin;
        pack.name = "pending";
        pack.atomic = p.atomic;
        pack.ops = p.ops;
        root = g_opts.dataRoot;
    }
    const std::string text = WritePack(pack, "Saved by sco.datacore for plugin " + pack.plugin +
                                                 " after the DataCore load. It applies at every launch, right after\n"
                                                 "the plugin's own data pack, until the plugin saves another patch or this file is deleted.");
    std::string error;
    const bool saved = Save(root, pack.plugin, patch, text, error);
    std::lock_guard<std::mutex> hold(g_lock);
    const auto it = g_patches.find(patch);
    if (!saved) {
        Log("[datacore] %s: saving its patch failed: %s", pack.plugin.c_str(), error.c_str());
        return SCO_FAILED;
    }
    Log("[datacore] %s: patch saved (%zu operations); applies at the next launch", pack.plugin.c_str(), pack.ops.size());
    if (it != g_patches.end() && it->second.stage == PatchRec::Stage::Open) it->second.stage = PatchRec::Stage::Saved;
    return SCO_OK;
}

sco_result T_discard(uint64_t patch) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started) return SCO_UNAVAILABLE;
    const auto it = g_patches.find(patch);
    if (it == g_patches.end() || detail::Released(it->second.owner)) return SCO_NOT_FOUND;
    g_patches.erase(it);
    return SCO_OK;
}

sco_result T_report(uint64_t patch, uint32_t index, sco_dc_report* out) {
    if (!out || out->size < sizeof(sco_dc_report)) return SCO_BAD_ARG;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started) return SCO_UNAVAILABLE;
    const auto it = g_patches.find(patch);
    if (it == g_patches.end() || detail::Released(it->second.owner)) return SCO_NOT_FOUND;
    const PatchRec& p = it->second;
    const size_t n = p.ops.size();
    if (index > n) return SCO_NOT_FOUND;
    Entry e;
    if (p.stage == PatchRec::Stage::Done && p.results.size() == n + 1) e = p.results[index];
    else if (p.stage == PatchRec::Stage::Saved) e.reason = "applies at the next launch";
    out->state = e.state;
    out->op_index = index == n ? SCO_DC_OP_PATCH : index;
    const size_t k = std::min(e.reason.size(), sizeof(out->reason) - 1);
    std::memcpy(out->reason, e.reason.data(), k);
    out->reason[k] = 0;
    return SCO_OK;
}

const sco_datacore_v1 kTable = {
    sizeof(sco_datacore_v1), 0,   T_state,  T_begin,   T_set,    T_add_instance,
    T_set_pointer,           T_append, T_add_record, T_commit, T_discard, T_report,
};

void OnRelease(const void* owner) {
    std::lock_guard<std::mutex> hold(g_lock);
    for (auto it = g_patches.begin(); it != g_patches.end();)
        it = it->second.owner == owner ? g_patches.erase(it) : std::next(it);
}

// ---- the load --------------------------------------------------------------------------------

struct Applied {
    sco_dc_applied data{};
};
void PostApplied(void* ctx) {
    auto* a = static_cast<Applied*>(ctx);
    const Result r = Dispatch(SCO_DC_APPLIED_EVENT, &a->data);
    if (r != Result::Ok) Log("[datacore] %s: %s", SCO_DC_APPLIED_EVENT, ResultName(r));
    delete a;
}

bool ReadText(const fs::path& path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "can't open"; return false; }
    std::ostringstream s;
    s << in.rdbuf();
    text = s.str();
    return true;
}

bool Enabled(const plugins::Plugin& p) { return p.state == plugins::State::Ready || p.state == plugins::State::Loaded; }

std::string IdOf(const plugins::Plugin& p) { return p.manifestOk || p.manifest.kind == plugins::Kind::Builtin ? p.manifest.id : p.folder; }

}  // namespace

fs::path PendingPath(const fs::path& dataRoot, std::string_view id) {
    return dataRoot / "datacore" / "pending" / (std::string(id) + ".toml");
}

Result Start(const Options& opts) {
    if (opts.dataRoot.empty()) return Result::BadArg;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) return Result::BadArg;
    }
    Result r = AddReleaseHook(OnRelease);
    if (r != Result::Ok) return r;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_opts = opts;
        g_state = SCO_DC_OPEN;
        g_patches.clear();
        g_started = true;
    }
    r = host::ProvideHostService(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_1, &kTable);
    if (r != Result::Ok) {
        RemoveReleaseHook(OnRelease);
        std::lock_guard<std::mutex> hold(g_lock);
        g_started = false;
        g_state = SCO_DC_LOADED;
    }
    return r;
}

void Stop() {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return;
        g_started = false;
        g_state = SCO_DC_LOADED;
        g_patches.clear();
    }
    host::WithdrawHostService(SCO_DATACORE_NAME);
    RemoveReleaseHook(OnRelease);
}

bool Started() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started;
}

const sco_datacore_v1* Table() { return &kTable; }

LoadResult Load(const Schema& base, const std::vector<plugins::Plugin>& list, const plugins::ContentIndex& index,
                const fs::path& dataRoot) {
    LoadResult out;
    // The patches committed before the load, by plugin, in commit (id) order.
    std::map<std::string, std::vector<std::pair<uint64_t, Pack>>> queued;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) g_state = SCO_DC_LOADED;
        for (auto& [id, p] : g_patches) {
            if (p.stage != PatchRec::Stage::Queued) continue;
            p.stage = PatchRec::Stage::Taken;
            Pack pack;
            pack.plugin = p.plugin;
            pack.name = "patch " + std::to_string(id);
            pack.atomic = p.atomic;
            pack.ops = p.ops;
            queued[p.plugin].emplace_back(id, std::move(pack));
        }
    }

    // The sources in load order: a parsed pack (index into packs) or a refusal made before applying.
    struct Source { size_t pack = SIZE_MAX; PackReport refused; uint64_t patch = 0; };
    std::vector<Source> sources;
    std::vector<Pack> packs;
    auto add = [&](Pack pack, uint64_t patch) {
        sources.push_back({ packs.size(), {}, patch });
        packs.push_back(std::move(pack));
    };
    auto refuse = [&](const std::string& plugin, const std::string& name, std::string reason) {
        Source s;
        s.refused.plugin = plugin;
        s.refused.name = name;
        s.refused.state = PackState::Refused;
        s.refused.reason = std::move(reason);
        out.notes.push_back(plugin + " " + name + ": refused: " + s.refused.reason);
        sources.push_back(std::move(s));
    };
    auto parse = [&](const std::string& plugin, const std::string& name, const fs::path& file) {
        std::string text, error;
        Pack pack;
        pack.plugin = plugin;
        pack.name = name;
        if (!ReadText(file, text, error)) return refuse(plugin, name, error);
        if (!ParsePack(text, pack, error)) return refuse(plugin, name, "line " + error);
        add(std::move(pack), 0);
    };

    std::set<std::string> listed;
    for (const plugins::Plugin& p : list) {
        const std::string id = IdOf(p);
        listed.insert(id);
        for (const plugins::ContentItem* item : index.FromPlugin(id))
            if (item->kind == plugins::ContentKind::DataCore) parse(id, item->name, item->path);
        std::error_code ec;
        const fs::path saved = PendingPath(dataRoot, id);
        if (!dataRoot.empty() && fs::is_regular_file(saved, ec)) {
            if (Enabled(p)) parse(id, "pending", saved);
            else out.notes.push_back("saved patch of " + id + " skipped: the plugin is " + plugins::StateName(p.state));
        }
        if (const auto it = queued.find(id); it != queued.end()) {
            for (auto& [pid, pack] : it->second) add(std::move(pack), pid);
            queued.erase(it);
        }
    }
    for (auto& [plugin, patches] : queued)   // a plugin this load's list doesn't have (not discovered)
        for (auto& [pid, pack] : patches) {
            out.notes.push_back(pack.plugin + " " + pack.name + ": skipped: the plugin isn't in this load's list");
            Source s;
            s.patch = pid;
            s.refused.plugin = pack.plugin;
            s.refused.name = pack.name;
            s.refused.state = PackState::Refused;
            s.refused.reason = "the plugin isn't in this load's list";
            s.refused.skipped = pack.ops.size();
            sources.push_back(std::move(s));
        }
    if (!dataRoot.empty()) {   // saved patches of plugins that aren't installed any more
        std::error_code ec;
        for (fs::directory_iterator it(dataRoot / "datacore" / "pending", ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path f = it->path();
            if (f.extension() != ".toml" || listed.count(f.stem().string())) continue;
            out.notes.push_back("saved patch of " + f.stem().string() + " skipped: the plugin is not installed");
        }
    }

    PackResult applied = ApplyPacks(base, packs);
    out.result.status = applied.status;
    out.result.splices = std::move(applied.splices);
    out.result.conflicts = std::move(applied.conflicts);
    std::map<uint64_t, const PackReport*> byPatch;
    for (Source& s : sources) {
        if (s.pack != SIZE_MAX) out.result.packs.push_back(std::move(applied.packs[s.pack]));
        else out.result.packs.push_back(std::move(s.refused));
    }
    for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i].patch) byPatch[sources[i].patch] = &out.result.packs[i];

    sco_dc_applied counts{};
    counts.size = sizeof(counts);
    for (const PackReport& r : out.result.packs) {
        if (r.state == PackState::Refused) counts.refused += static_cast<uint32_t>(std::max(r.ops.size(), r.skipped));
        else {
            counts.applied += static_cast<uint32_t>(r.applied);
            counts.skipped += static_cast<uint32_t>(r.skipped);
        }
    }

    {   // each committed patch's report
        std::lock_guard<std::mutex> hold(g_lock);
        for (const auto& [pid, rep] : byPatch) {
            const auto it = g_patches.find(pid);
            if (it == g_patches.end()) continue;   // discarded or released meanwhile
            PatchRec& p = it->second;
            p.results.assign(p.ops.size() + 1, Entry{});
            for (size_t k = 0; k < p.ops.size(); ++k) {
                Entry& e = p.results[k];
                const PackOpReport* op = k < rep->ops.size() ? &rep->ops[k] : nullptr;
                if (op && !op->status) { e.state = SCO_DC_SKIPPED; e.reason = op->status.message; }
                else if (rep->state == PackState::Refused) { e.state = SCO_DC_REFUSED; e.reason = "not applied: " + rep->reason; }
                else e.state = SCO_DC_APPLIED;
            }
            Entry& whole = p.results.back();
            if (rep->state == PackState::Refused) { whole.state = SCO_DC_REFUSED; whole.reason = rep->reason; }
            else {
                whole.state = SCO_DC_APPLIED;
                if (rep->skipped) whole.reason = std::to_string(rep->skipped) + " of " + std::to_string(rep->applied + rep->skipped) +
                                                 " operations skipped: " + rep->reason;
            }
            p.stage = PatchRec::Stage::Done;
        }
    }

    Log("%s", Summary(out.result).c_str());
    for (const std::string& n : out.notes) Log("[datacore] %s", n.c_str());
    for (const std::string& c : out.result.conflicts) Log("[datacore] conflict: %s", c.c_str());
    auto* event = new Applied{ counts };
    if (Post(PostApplied, event) != Result::Ok) {
        Log("[datacore] %s not posted: the task queue is full", SCO_DC_APPLIED_EVENT);
        delete event;
    }
    return out;
}

}  // namespace sco::datacore::service
