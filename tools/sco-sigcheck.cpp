// sco-sigcheck: run sco-core's signature tables against a StarCitizen.exe on disk.
//
//   sco-sigcheck <StarCitizen.exe> [--catalog <runtime_catalog.inc>] [-v]
//
// Prints the same report the game prints at startup, plus each row's RVA. With --catalog,
// rows whose address equals an entry in an sdk_dumper runtime catalog get that entry's name
// next to them; the catalog is only read for labels, never used to resolve anything.
// Exit code: 0 when every row is OK, 1 otherwise, 2 on a usage or load error.
#include "sco/game/signatures.h"
#include "sco/log.h"
#include "sco/pe_file.h"
#include "sco/signatures.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <string>

static void Print(const char* line) { std::printf("%s\n", line); }

static std::map<uint32_t, std::string> ReadCatalog(const char* path) {
    std::map<uint32_t, std::string> out;
    FILE* f = std::fopen(path, "r");
    if (!f) { std::fprintf(stderr, "can't read %s\n", path); return out; }
    static const std::regex row(R"re(^\{"([^"]+)","[^"]+",0x([0-9a-fA-F]+)ULL)re");
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        std::cmatch m;
        if (std::regex_search(line, m, row)) out[static_cast<uint32_t>(std::strtoul(m[2].str().c_str(), nullptr, 16))] = m[1].str();
    }
    std::fclose(f);
    return out;
}

int main(int argc, char** argv) {
    const char* exe = nullptr; const char* catalog = nullptr; bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--catalog") && i + 1 < argc) catalog = argv[++i];
        else if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (!exe && argv[i][0] != '-') exe = argv[i];
        else { std::fprintf(stderr, "usage: sco-sigcheck <StarCitizen.exe> [--catalog <runtime_catalog.inc>] [-v]\n"); return 2; }
    }
    if (!exe) { std::fprintf(stderr, "usage: sco-sigcheck <StarCitizen.exe> [--catalog <runtime_catalog.inc>] [-v]\n"); return 2; }

    sco::SetLogSink(Print);
    sco::FileImage file;
    if (!file.Load(exe)) { std::fprintf(stderr, "%s: %s\n", exe, file.error.c_str()); return 2; }
    std::printf("image: timestamp 0x%08x, size 0x%x, preferred base 0x%llx\n", file.img.timestamp, file.img.size,
                static_cast<unsigned long long>(file.preferredBase));
    if (!sco::game::RegisterGameSignatures()) return 2;
    sco::ResolveAll(file.img);
    sco::LogSignatureReport(false);

    const auto names = catalog ? ReadCatalog(catalog) : std::map<uint32_t, std::string>{};
    if (catalog) std::printf("catalog: %zu entries from %s (labels only)\n", names.size(), catalog);
    bool allOk = true;
    for (size_t i = 0; i < sco::SignatureCount(); ++i) {
        const sco::SigResult& r = sco::SignatureResult(i);
        allOk &= r.state == sco::SigState::Ok;
        if (!verbose && r.state != sco::SigState::Ok) continue;
        std::string label;
        if (r.at) {
            auto it = names.find(file.Rva(r.at));
            if (it != names.end()) label = "  = " + it->second;
        }
        std::printf("%-8s %-36s %s%s\n", sco::SigStateName(r.state), sco::SignatureDef(i)->id,
                    r.at ? ("0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%x", file.Rva(r.at)); return std::string(b); }()).c_str() : "-",
                    label.c_str());
    }
    return allOk ? 0 : 1;
}
