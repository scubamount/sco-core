// Unit tests for the sco.datacore service (include/sco_datacore.h, sco/datacore_service.h) over
// the real host table and runtime, with a tests/dcb_builder.h file as Game2.dcb. The core is a
// simulated launch sequence (design decision 9): launch 1 loads (a data pack plus a patch committed
// before the load), then a plugin commits after the load, which saves
// data/datacore/pending/<id>.toml; launch 2 applies that saved patch right after the plugin's own
// position in plugin order; launch 3, the plugin uninstalled, skips it. Also: call-time validation,
// release before the load, per-patch atomicity and SCO_DC_NON_ATOMIC, reports, datacore.applied,
// the service off, and the scosdk wrapper (scosdk/datacore.hpp).
//   test_datacore_service <out dir>
#include "dcb_builder.h"
#include "sco/datacore.h"
#include "sco/datacore_service.h"
#include "sco/host.h"
#include "sco/runtime.h"
#include "sco_datacore.h"
#include "scosdk/datacore.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace dc = sco::datacore;
namespace svc = sco::datacore::service;
namespace P = sco::plugins;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static fs::path g_out;
static const sco_api* g_api;
static const sco_datacore_v1* g_dc;


static Bytes Scene() { return dcb::ValuedFixture(36).Build(); }

static std::string Val(const Bytes& base, const dc::PackResult& r, const char* record, const char* field) {
    Bytes f;
    if (!r.status || !dc::ApplySplices(base, r.splices, f)) return "(not applied)";
    dc::Schema s;
    if (!s.Parse(f)) return "(layout)";
    dc::Patch p(s);
    dc::RecordRef ref;
    ref.name = record;
    std::vector<dc::FieldView> v;
    if (!p.ReadFields(ref, field, v, 0) || v.empty()) return "(missing)";
    return v[0].text;
}

static void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static std::string Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

static P::Plugin MakePlugin(const char* id, P::Kind kind, P::State state, const fs::path& dir = {}) {
    P::Plugin p;
    p.folder = id;
    p.dir = dir;
    p.manifest.id = id;
    p.manifest.kind = kind;
    p.manifestOk = true;
    p.state = state;
    return p;
}

static sco_dc_value F(double f) { sco_dc_value v{}; v.size = sizeof(v); v.type = SCO_DC_FLOAT; v.f = f; return v; }

static sco_dc_report Report(uint64_t patch, uint32_t i, sco_result* r = nullptr) {
    sco_dc_report rep{};
    rep.size = sizeof(rep);
    const sco_result res = g_dc->report(patch, i, &rep);
    if (r) *r = res;
    return rep;
}

// datacore.applied
static int g_events;
static sco_dc_applied g_last;
static void OnApplied(const char*, const void* data, void*) {
    ++g_events;
    if (data) std::memcpy(&g_last, data, sizeof(g_last));
}

static void TestLaunches() {
    const Bytes base = Scene();
    dc::Schema s;
    CHECK(s.Parse(base));
    const fs::path data = g_out / "svc" / "data", plugins = g_out / "svc" / "plugins";
    fs::remove_all(g_out / "svc");
    Write(plugins / "alpha" / "datacore" / "a.toml", "format = 1\n[[set]]\nrecord = \"ShipA\"\nfield = \"speed\"\nvalue = 1.0\n");
    Write(svc::PendingPath(data, "gamma"), "format = 1\n[[set]]\nrecord = \"ShipA\"\nfield = \"mass\"\nvalue = 99.0\n");
    Write(svc::PendingPath(data, "ghost"), "format = 1\n");
    std::vector<P::Plugin> list = { MakePlugin("alpha", P::Kind::Data, P::State::Ready, plugins / "alpha"),
                                     MakePlugin("beta", P::Kind::Native, P::State::Ready),
                                     MakePlugin("gamma", P::Kind::Native, P::State::Disabled) };
    P::ContentIndex index;
    index.Build(list);
    CHECK(index.Items(P::ContentKind::DataCore).size() == 1);

    sco_plugin* watcher = sco::host::NewPlugin("watcher");
    CHECK(g_api->subscribe(watcher, SCO_DC_APPLIED_EVENT, OnApplied, nullptr) == SCO_OK);

    // ---- launch 1 ------------------------------------------------------------------------------
    svc::Options o;
    o.dataRoot = data;
    CHECK(svc::Start(o) == sco::Result::Ok && svc::Started());
    CHECK(svc::Start(o) == sco::Result::BadArg);
    CHECK(g_api->query_service(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0, reinterpret_cast<const void**>(&g_dc)) == SCO_OK);
    CHECK(g_dc == svc::Table() && g_dc->size == sizeof(sco_datacore_v1));
    CHECK(g_dc->state() == SCO_DC_OPEN);

    sco_plugin* beta = sco::host::NewPlugin("beta");
    sco_plugin* delta = sco::host::NewPlugin("delta");
    uint64_t pre = 0, dropped = 0, bad = 0;
    const sco_dc_value mass = F(7.0);
    CHECK(g_dc->begin(beta, 0, &pre) == SCO_OK && pre != 0);
    CHECK(g_dc->set(pre, "ShipB", "mass", &mass) == SCO_OK);
    CHECK(g_dc->commit(pre) == SCO_OK);
    sco_result rr;
    CHECK(Report(pre, 0, &rr).state == SCO_DC_QUEUED && rr == SCO_OK);
    CHECK(Report(pre, 1).op_index == SCO_DC_OP_PATCH);
    Report(pre, 2, &rr);
    CHECK(rr == SCO_NOT_FOUND);
    // An atomic patch with a bad field: refused whole at the load.
    const sco_dc_value one = F(1.0);
    CHECK(g_dc->begin(beta, 0, &bad) == SCO_OK);
    CHECK(g_dc->set(bad, "ShipB", "speed", &one) == SCO_OK && g_dc->set(bad, "ShipB", "nope", &one) == SCO_OK);
    CHECK(g_dc->commit(bad) == SCO_OK);
    // A non-atomic one: the good operation applies.
    uint64_t loose = 0;
    CHECK(g_dc->begin(beta, SCO_DC_NON_ATOMIC, &loose) == SCO_OK);
    const sco_dc_value two = F(2.0);
    CHECK(g_dc->set(loose, "PartX", "weight", &two) == SCO_OK && g_dc->set(loose, "PartX", "nope", &two) == SCO_OK);
    CHECK(g_dc->commit(loose) == SCO_OK);
    // delta's patch is dropped when it unloads before the load.
    CHECK(g_dc->begin(delta, 0, &dropped) == SCO_OK && g_dc->set(dropped, "ShipA", "mass", &mass) == SCO_OK);
    CHECK(g_dc->commit(dropped) == SCO_OK);
    CHECK(sco::Release(delta) == sco::Result::Ok);
    Report(dropped, 0, &rr);
    CHECK(rr == SCO_NOT_FOUND && g_dc->commit(dropped) == SCO_NOT_FOUND);

    g_events = 0;
    svc::LoadResult l1 = svc::Load(s, list, index, data);
    CHECK(g_dc->state() == SCO_DC_LOADED);
    const auto& r1 = l1.result;
    CHECK(r1.status.ok() && r1.packs.size() == 4);   // alpha, then beta's three patches; gamma skipped (a note)
    if (r1.packs.size() == 4) {
        CHECK(r1.packs[0].plugin == "alpha" && r1.packs[0].name == "datacore/a.toml");
        CHECK(r1.packs[1].plugin == "beta" && r1.packs[1].name == "patch " + std::to_string(pre));
        CHECK(r1.packs[2].state == dc::PackState::Refused && r1.packs[3].state == dc::PackState::Partial);
    }
    CHECK(Val(base, r1, "ShipA", "speed") == "1.0" && Val(base, r1, "ShipB", "mass") == "7.0");
    CHECK(Val(base, r1, "ShipB", "speed") == "101.0" && Val(base, r1, "PartX", "weight") == "2.0");
    CHECK(Val(base, r1, "ShipA", "mass").find("99") == std::string::npos);   // gamma is disabled
    bool gammaNote = false, ghostNote = false;
    for (const std::string& n : l1.notes) {
        gammaNote |= n == "saved patch of gamma skipped: the plugin is disabled";
        ghostNote |= n == "saved patch of ghost skipped: the plugin is not installed";
    }
    CHECK(gammaNote && ghostNote);
    // Reports after the load.
    CHECK(Report(pre, 0).state == SCO_DC_APPLIED && Report(pre, 1).state == SCO_DC_APPLIED && Report(pre, 1).reason[0] == 0);
    CHECK(Report(bad, 0).state == SCO_DC_REFUSED && Report(bad, 1).state == SCO_DC_SKIPPED);
    CHECK(std::strstr(Report(bad, 1).reason, "no property \"nope\" in Ship") != nullptr);
    CHECK(Report(bad, 2).state == SCO_DC_REFUSED && Report(bad, 2).op_index == SCO_DC_OP_PATCH);
    CHECK(Report(loose, 0).state == SCO_DC_APPLIED && Report(loose, 1).state == SCO_DC_SKIPPED);
    CHECK(Report(loose, 2).state == SCO_DC_APPLIED && std::strncmp(Report(loose, 2).reason, "1 of 2 operations skipped", 25) == 0);
    // datacore.applied on the next tick: alpha 1, pre 1, loose 1 applied, loose 1 skipped, bad 2 refused.
    CHECK(g_events == 0);
    CHECK(sco::GameThreadTick(1) == sco::Result::Ok);
    CHECK(g_events == 1 && g_last.size == sizeof(sco_dc_applied));
    CHECK(g_last.applied == 3 && g_last.skipped == 1 && g_last.refused == 2);

    // After the load: begin still works, commit saves for the next launch.
    uint64_t post = 0, inst = 0;
    CHECK(g_dc->begin(beta, 0, &post) == SCO_OK);
    const sco_dc_value fast = F(2.0);
    CHECK(g_dc->set(post, "ShipA", "speed", &fast) == SCO_OK);
    CHECK(g_dc->add_instance(post, "Part", "PartX", nullptr, &inst) == SCO_OK && inst != 0);
    const std::string at = "@" + std::to_string(inst);
    const sco_dc_value w = F(11.0);
    CHECK(g_dc->set(post, at.c_str(), "weight", &w) == SCO_OK);
    CHECK(g_dc->set_pointer(post, "ShipB", "engine", inst) == SCO_OK);
    sco_dc_value str{};
    str.size = sizeof(str);
    str.type = SCO_DC_STRING;
    str.s = "saved \"label\"";
    CHECK(g_dc->set(post, "guid:27262524-2322-2120-2f2e-2d2c2b2a2928", "label", &str) == SCO_OK);   // ShipB by GUID
    // Call-time validation: refused, not queued.
    sco_dc_value bogus = F(1.0);
    CHECK(g_dc->set(post, "ShipA", "speed..x", &bogus) == SCO_BAD_ARG);
    CHECK(g_dc->set(post, "ShipA", "speed", &bogus) == SCO_BAD_ARG);               // set twice in one patch
    CHECK(g_dc->set(post, "guid:nope", "speed", &bogus) == SCO_BAD_ARG);
    CHECK(g_dc->set(post, "", "speed", &bogus) == SCO_BAD_ARG);
    CHECK(g_dc->set(post, "@999999", "weight", &bogus) == SCO_BAD_ARG);
    CHECK(g_dc->set_pointer(post, "ShipA", "engine", 424242) == SCO_BAD_ARG);
    bogus.size = 8;
    CHECK(g_dc->set(post, "ShipA", "mass", &bogus) == SCO_BAD_ARG);
    sco_dc_value badStr{};
    badStr.size = sizeof(badStr);
    badStr.type = SCO_DC_STRING;
    badStr.s = "\xff\xfe";
    CHECK(g_dc->set(post, "ShipA", "label", &badStr) == SCO_BAD_ARG);               // not UTF-8
    sco_dc_value badBool = F(0);
    badBool.type = SCO_DC_BOOL;
    badBool.i = 2;
    CHECK(g_dc->set(post, "ShipA", "flag", &badBool) == SCO_BAD_ARG);
    CHECK(g_dc->add_instance(post, "Part", nullptr, "weight", &inst) == SCO_BAD_ARG);
    uint64_t rec = 1;
    CHECK(g_dc->add_record(post, "Ship", "ShipC", "guid", "ShipA", nullptr, &rec) == SCO_UNAVAILABLE && rec == 0);
    uint64_t none = 0;
    CHECK(g_dc->begin(beta, 0x8, &none) == SCO_BAD_ARG && g_dc->begin(nullptr, 0, &none) == SCO_BAD_ARG);
    CHECK(g_dc->set(987654321, "ShipA", "speed", &fast) == SCO_NOT_FOUND);
    CHECK(g_dc->commit(post) == SCO_OK);
    CHECK(g_dc->commit(post) == SCO_BAD_ARG && g_dc->set(post, "ShipA", "mass", &mass) == SCO_BAD_ARG);
    const sco_dc_report saved = Report(post, 0);
    CHECK(saved.state == SCO_DC_QUEUED && std::strcmp(saved.reason, "applies at the next launch") == 0);
    const fs::path file = svc::PendingPath(data, "beta");
    CHECK(fs::is_regular_file(file));
    for (const auto& e : fs::directory_iterator(file.parent_path())) CHECK(e.path().extension() != ".tmp");
    dc::Pack savedPack;
    std::string error;
    CHECK(dc::ParsePack(Read(file), savedPack, error) && savedPack.ops.size() == 5);

    svc::Stop();
    CHECK(!svc::Started() && g_dc->state() == SCO_DC_LOADED);
    CHECK(g_dc->begin(beta, 0, &none) == SCO_UNAVAILABLE);
    const void* gone = nullptr;
    CHECK(g_api->query_service(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0, &gone) == SCO_NOT_FOUND);
    CHECK(sco::Release(beta) == sco::Result::Ok);

    // ---- launch 2: the saved patch applies right after beta's position, beta wins the speed ----
    CHECK(svc::Start(o) == sco::Result::Ok && g_dc->state() == SCO_DC_OPEN);
    svc::LoadResult l2 = svc::Load(s, list, index, data);
    const auto& r2 = l2.result;
    CHECK(r2.status.ok() && r2.packs.size() == 2);
    if (r2.packs.size() == 2) CHECK(r2.packs[1].plugin == "beta" && r2.packs[1].name == "pending" && r2.packs[1].state == dc::PackState::Applied);
    CHECK(Val(base, r2, "ShipA", "speed") == "2.0");
    CHECK(Val(base, r2, "ShipB", "engine.weight") == "11.0" && Val(base, r2, "ShipB", "engine.partName") == "\"p0\"");
    CHECK(Val(base, r2, "ShipB", "label") == "\"saved \\\"label\\\"\"");
    CHECK(r2.conflicts.size() == 1 && r2.conflicts[0].find("alpha (datacore/a.toml:2) overridden by beta (pending:") != std::string::npos);
    CHECK(sco::GameThreadTick(2) == sco::Result::Ok && g_last.applied == 6 && g_last.refused == 0);
    svc::Stop();

    // ---- launch 3: beta uninstalled; its saved patch is skipped, alpha's value is back ----------
    std::vector<P::Plugin> without = { list[0], list[2] };
    CHECK(svc::Start(o) == sco::Result::Ok);
    svc::LoadResult l3 = svc::Load(s, without, index, data);
    CHECK(l3.result.packs.size() == 1 && Val(base, l3.result, "ShipA", "speed") == "1.0");
    bool betaNote = false;
    for (const std::string& n : l3.notes) betaNote |= n == "saved patch of beta skipped: the plugin is not installed";
    CHECK(betaNote);

    // ---- an empty commit after the load clears the saved patch ----------------------------------
    sco_plugin* beta2 = sco::host::NewPlugin("beta");
    uint64_t empty = 0;
    CHECK(g_dc->begin(beta2, 0, &empty) == SCO_OK && g_dc->commit(empty) == SCO_OK);
    dc::Pack cleared;
    CHECK(dc::ParsePack(Read(file), cleared, error) && cleared.ops.empty());
    CHECK(g_dc->discard(empty) == SCO_OK && g_dc->discard(empty) == SCO_NOT_FOUND);
    sco::GameThreadTick(3);
    svc::Stop();

    // ---- the scosdk wrapper, before a load ---------------------------------------------------
    CHECK(svc::Start(o) == sco::Result::Ok);
    {
        sco::sdk::DataCore sdk;
        CHECK(sdk.Open(g_api, beta2) == SCO_OK && sdk.State() == SCO_DC_OPEN);
        sco::sdk::DataCorePatch p = sdk.Begin();
        CHECK(static_cast<bool>(p));
        CHECK(p.Set("ShipA", "i8", -5) == SCO_OK && p.Set("ShipA", "u64", uint64_t{ 1 } << 40) == SCO_OK);
        CHECK(p.Set("ShipA", "flag", true) == SCO_OK && p.Set("ShipA", "label", "sdk") == SCO_OK);
        CHECK(p.Set("ShipA", "kind", sco::sdk::DataCoreEnum{ "Small" }) == SCO_OK);
        CHECK(p.Set("ShipA", "id", sco::sdk::DataCoreGuid{ "93929190-9594-9796-9f9e-9d9c9b9a9998" }) == SCO_OK);
        sco::sdk::DataCoreInstance part = p.AddInstance("Part", "PartX");
        CHECK(part && part.Result() == SCO_OK);
        CHECK(p.Set(part.Ref(), "weight", 3.25f) == SCO_OK);
        CHECK(p.Append("ShipA", "parts", part) == SCO_OK && p.Append("ShipA", "parts", nullptr) == SCO_OK);
        CHECK(p.Append("ShipA", "counts", 30) == SCO_OK);
        CHECK(p.AddRecord("Ship", "ShipC", "x", "ShipA") == SCO_UNAVAILABLE);
        CHECK(p.Commit() == SCO_OK);
        {
            sco::sdk::DataCorePatch scrap = sdk.Begin();   // discarded by its destructor
            CHECK(scrap.Set("ShipB", "speed", 5.0) == SCO_OK);
        }
        svc::LoadResult l4 = svc::Load(s, list, index, data);   // alpha, beta's (empty) saved patch, its queued patch
        const auto& r4 = l4.result;
        CHECK(r4.status.ok() && r4.packs.size() == 3 && r4.packs[2].state == dc::PackState::Applied);
        CHECK(Val(base, r4, "ShipA", "i8") == "-5" && Val(base, r4, "ShipA", "u64") == "1099511627776" && Val(base, r4, "ShipA", "label") == "\"sdk\"");
        CHECK(Val(base, r4, "ShipA", "parts[0].weight") == "3.25" && Val(base, r4, "ShipA", "parts[1]") == "null");
        CHECK(Val(base, r4, "ShipA", "counts[0]") == "30" && Val(base, r4, "ShipB", "speed") == "101.0");
        const std::vector<sco_dc_report> reps = p.Reports();
        CHECK(reps.size() == 12 && reps.back().op_index == SCO_DC_OP_PATCH && reps.back().state == SCO_DC_APPLIED);
    }
    sco::GameThreadTick(4);
    svc::Stop();
    CHECK(sco::Release(beta2) == sco::Result::Ok);

    // ---- the service off: nothing published, Load still applies packs ------------------------
    const void* off = nullptr;
    CHECK(g_api->query_service(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0, &off) == SCO_NOT_FOUND);
    svc::LoadResult l5 = svc::Load(s, list, index, data);
    CHECK(l5.result.packs.size() == 2 && Val(base, l5.result, "ShipA", "speed") == "1.0");
    sco::GameThreadTick(5);
    CHECK(sco::Release(watcher) == sco::Result::Ok);
    CHECK(svc::Start(svc::Options{}) == sco::Result::BadArg);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_datacore_service <out dir>\n");
        return 2;
    }
    g_out = argv[1];
    sco::SetGameThread();
    g_api = sco::host::BuildApi({ "test_datacore_service" });
    TestLaunches();
    std::printf("sco-core datacore service tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
