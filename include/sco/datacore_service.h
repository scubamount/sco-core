#pragma once
// sco::datacore::service: the host side of the "sco.datacore" service (include/sco_datacore.h,
// docs/design/vfs-datacore.md section 6 and decision 9) and the DataCore load that applies data
// packs, saved patches and queued patches in plugin order. C++ and internal: the host kit starts
// the service when the product enables it (sco::app::Platform::dataCore); plugins only see the C
// table.
//
//   sco::datacore::service::Options o;
//   o.dataRoot = "data";                       // saved patches: data/datacore/pending/<plugin id>.toml
//   sco::datacore::service::Start(o);          // publishes sco.datacore 1.0, state SCO_DC_OPEN
//   ...
//   // At the .dcb open, on the loader thread (the CryPak adapter, plan PR 7), with the plugin list
//   // and content index of this launch (sco::plugins::Discover + ContentIndex::Build run no code):
//   auto loaded = sco::datacore::service::Load(schema, list, index, "data");   // state SCO_DC_LOADED
//   ... loaded.result.splices go to sco::vfs ...
//   sco::datacore::service::Stop();            // after every plugin unloaded
//
// Order of one load (decision 7): plugins in list order (built-ins first, then folder-name
// order); for each, its data pack's datacore/*.toml files in name order, then its saved patch
// (skipped, and logged, when the plugin is no longer installed or is disabled, off, refused or
// crashed), then the patches it committed before the load, in commit order. A later source wins a
// field an earlier one set. Each source is atomic unless it says otherwise. After the load the
// host posts "datacore.applied" for the game thread and each committed patch's report() holds
// one entry per operation and one for the patch.
#include "sco_datacore.h"
#include "sco/datacore_pack.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sco::datacore::service {

struct Options {
    std::filesystem::path dataRoot;   // the product's data folder; saved patches in <dataRoot>/datacore/pending/
};

// Publishes "sco.datacore" 1.0 under the host's id, sets the state to SCO_DC_OPEN and installs the
// release hook. Any thread. BadArg: dataRoot empty, already started, or the name already
// published. TooMany: no release hook slot.
Result Start(const Options& opts);

// Withdraws "sco.datacore" and drops every patch; later calls through the table answer
// SCO_UNAVAILABLE (state() answers SCO_DC_LOADED). No-op unless started. Any thread.
void Stop();

bool Started();

// The service table (what query_service hands out). Valid for the life of the process.
const sco_datacore_v1* Table();

// <dataRoot>/datacore/pending/<id>.toml.
std::filesystem::path PendingPath(const std::filesystem::path& dataRoot, std::string_view id);

struct LoadResult {
    PackResult               result;   // every source, in load order; splices for sco::vfs
    std::vector<std::string> notes;    // sources skipped or refused before applying, one line each
};

// The DataCore load. Switches the service (when started) to SCO_DC_LOADED and takes the patches
// committed so far, reads every source in the order above, applies them with ApplyPacks, stores
// each committed patch's report, logs the summary, notes and conflicts ("[datacore] ..."), and
// posts "datacore.applied" (a sco_dc_applied) for the game thread. Works without the service too
// (data packs and saved patches only). Any thread (the loader's); plugin calls never wait for it.
LoadResult Load(const Schema& base, const std::vector<plugins::Plugin>& list, const plugins::ContentIndex& index,
                const std::filesystem::path& dataRoot);

}  // namespace sco::datacore::service
