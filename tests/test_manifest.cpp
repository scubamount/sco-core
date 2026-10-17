// Unit tests for plugin manifest parsing (sco/plugins.h: ParseManifest, KindName).
#include "sco/plugins.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace P = sco::plugins;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool Parse(std::string_view text, P::Manifest& m, std::string& err) {
    return P::ParseManifest(text, m, err);
}

static std::string ParseError(std::string_view text) {
    P::Manifest m;
    std::string err;
    return Parse(text, m, err) ? std::string("<ok>") : err;
}

static void TestKindName() {
    CHECK(std::strcmp(P::KindName(P::Kind::Native), "native") == 0);
    CHECK(std::strcmp(P::KindName(P::Kind::Lua), "lua") == 0);
    CHECK(std::strcmp(P::KindName(P::Kind::Data), "data") == 0);
    CHECK(std::strcmp(P::KindName(P::Kind::Builtin), "builtin") == 0);
    CHECK(std::strcmp(P::KindName(static_cast<P::Kind>(99)), "?") == 0);
}

static void TestValidManifests() {
    P::Manifest m;
    std::string err;

    // Full native manifest with CRLF, UTF-8 BOM, comments, and optional fields
    const char* nativeIni =
        "\xEF\xBB\xBF; header comment\r\n"
        "# another header comment\r\n"
        "id = my_plugin   ; trailing comment\r\n"
        "name = My Sample Plugin\r\n"
        "version = 1.2.3\r\n"
        "author = Author Name\r\n"
        "api = 1.1\r\n"
        "kind = native\r\n"
        "entry = my_plugin.dll\r\n"
        "requires = teleport.spatial, spawn.ship\r\n"
        "future_key = ignored_value\r\n";

    CHECK(Parse(nativeIni, m, err));
    CHECK(err.empty());
    CHECK(m.id == "my_plugin");
    CHECK(m.name == "My Sample Plugin");
    CHECK(m.version == "1.2.3");
    CHECK(m.author == "Author Name");
    CHECK(m.apiMajor == 1 && m.apiMinor == 1);
    CHECK(m.kind == P::Kind::Native);
    CHECK(m.entry == "my_plugin.dll");
    CHECK((m.requires_ == std::vector<std::string>{ "teleport.spatial", "spawn.ship" }));

    // Lua manifest
    const char* luaIni =
        "id = lua_mod\n"
        "name = Lua Plugin\n"
        "version = 0.1\n"
        "api = 1.0\n"
        "kind = lua\n"
        "entry = main.lua\n";

    CHECK(Parse(luaIni, m, err));
    CHECK(m.id == "lua_mod" && m.kind == P::Kind::Lua && m.entry == "main.lua");

    // Data manifest (no entry allowed)
    const char* dataIni =
        "id = data_pack\n"
        "name = Data Pack\n"
        "version = 2.0\n"
        "api = 1.0\n"
        "kind = data\n";

    CHECK(Parse(dataIni, m, err));
    CHECK(m.id == "data_pack" && m.kind == P::Kind::Data && m.entry.empty());

    // Inline comments without preceding whitespace should NOT be stripped as comments
    const char* inlineCommentIni =
        "id = c_sharp\n"
        "name = C#;x\n"
        "version = 1.0\n"
        "api = 1.0\n"
        "kind = data\n";

    CHECK(Parse(inlineCommentIni, m, err));
    CHECK(m.name == "C#;x");

    // Sections and unknown sections
    const char* sectionsIni =
        "id = sec_plugin\n"
        "name = Section Plugin\n"
        "version = 1.0\n"
        "api = 1.0\n"
        "kind = native\n"
        "entry = sec.dll\n"
        "[future_section]\n"
        "future_setting = 123\n"
        "[settings]\n"
        "speed = int default 10 min 1 max 100\n";

    CHECK(Parse(sectionsIni, m, err));
    CHECK(m.id == "sec_plugin");
    CHECK(m.settings.size() == 1);
    CHECK(m.settings[0].name == "speed");
}

static void TestInvalidManifests() {
    P::Manifest m;
    std::string err;

    // Too big (> 16KB)
    std::string bigIni = "id=a\nname=N\nversion=1\napi=1.0\nkind=data\n";
    bigIni.append(P::kMaxManifestBytes - bigIni.size() + 1, ' ');
    CHECK(ParseError(bigIni) == "too big");

    // UTF-16 BOM
    CHECK(!Parse(std::string_view("\xFF\xFEi\0d\0=\0a\0", 10), m, err) && err == "plugin.ini must be UTF-8");
    CHECK(!Parse(std::string_view("\xFE\xFF\0i\0d", 6), m, err) && err == "plugin.ini must be UTF-8");

    // Missing required keys
    CHECK(ParseError("name=N\nversion=1\napi=1.0\nkind=data\n").find("missing key 'id'") != std::string::npos);
    CHECK(ParseError("id=a\nversion=1\napi=1.0\nkind=data\n").find("missing key 'name'") != std::string::npos);
    CHECK(ParseError("id=a\nname=N\napi=1.0\nkind=data\n").find("missing key 'version'") != std::string::npos);
    CHECK(ParseError("id=a\nname=N\nversion=1\nkind=data\n").find("missing key 'api'") != std::string::npos);
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\n").find("missing key 'kind'") != std::string::npos);

    // Entry requirements per kind
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=native\n") == "missing key 'entry'");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=lua\n") == "missing key 'entry'");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=data\nentry=data.dll\n") == "a data pack has no entry");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=data\n[settings]\na = bool\n") == "a data pack has no settings");

    // Duplicate key
    CHECK(ParseError("id=a\nid=b\nname=N\nversion=1\napi=1.0\nkind=data\n") == "line 2: duplicate key 'id'");

    // Malformed line
    CHECK(ParseError("id=a\njust words\n") == "line 2: expected key = value");

    // Invalid id
    const char* baseData = "name=N\nversion=1\napi=1.0\nkind=data\n";
    auto checkId = [&](const char* id) { return ParseError(std::string("id=") + id + "\n" + baseData); };

    CHECK(checkId("") .find("id must be") != std::string::npos);
    CHECK(checkId("A").find("id must be") != std::string::npos); // uppercase
    CHECK(checkId("a-b").find("id must be") != std::string::npos); // hyphen
    CHECK(checkId("a.b").find("id must be") != std::string::npos); // dot
    std::string longId(32, 'a');
    CHECK(checkId(longId.c_str()).find("id must be") != std::string::npos); // 32 chars
    std::string valid31Id(31, 'a');
    CHECK(checkId(valid31Id.c_str()) == "<ok>"); // 31 chars

    // Reserved ids
    for (const char* r : P::kReservedIds) {
        CHECK(checkId(r) == "line 1: id is reserved");
    }

    // Invalid Text fields (name, version, author)
    CHECK(ParseError("id=a\nname=\nversion=1\napi=1.0\nkind=data\n").find("name must be") != std::string::npos);
    CHECK(ParseError("id=a\nname=\x01\nversion=1\napi=1.0\nkind=data\n").find("name must be") != std::string::npos);
    std::string longName(64, 'n');
    CHECK(ParseError("id=a\nname=" + longName + "\nversion=1\napi=1.0\nkind=data\n").find("name must be") != std::string::npos);

    CHECK(ParseError("id=a\nname=N\nversion=\napi=1.0\nkind=data\n").find("version must be") != std::string::npos);
    std::string longVer(32, 'v');
    CHECK(ParseError("id=a\nname=N\nversion=" + longVer + "\napi=1.0\nkind=data\n").find("version must be") != std::string::npos);

    std::string longAuthor(64, 'a');
    CHECK(ParseError("id=a\nname=N\nversion=1\nauthor=" + longAuthor + "\napi=1.0\nkind=data\n").find("author must be") != std::string::npos);

    // Invalid api
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=.1\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.x\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.65536\nkind=data\n") == "line 4: api must be <major>.<minor>");

    // Invalid kind
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=unknown\n") == "line 5: kind must be native, lua or data");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=Native\n") == "line 5: kind must be native, lua or data");

    // Invalid entry
    const char* baseNative = "id=a\nname=N\nversion=1\napi=1.0\nkind=native\nentry=";
    for (const char* badEntry : { "../x.dll", "sub/x.dll", "sub\\x.dll", "C:x.dll", "..", ".", "a*b.dll", "a?b.dll" }) {
        CHECK(ParseError(std::string(baseNative) + badEntry + "\n") == "line 6: entry must be a file name in the plugin folder");
    }

    // Invalid requires
    CHECK(ParseError(std::string(baseData) + "id=a\nrequires=Cap\n").find("requires must be") != std::string::npos); // uppercase
    CHECK(ParseError(std::string(baseData) + "id=a\nrequires=a..b\n").find("requires must be") != std::string::npos); // consecutive dots
    CHECK(ParseError(std::string(baseData) + "id=a\nrequires=.a\n").find("requires must be") != std::string::npos); // leading dot
    CHECK(ParseError(std::string(baseData) + "id=a\nrequires=a.\n").find("requires must be") != std::string::npos); // trailing dot
    CHECK(ParseError(std::string(baseData) + "id=a\nrequires=a, a\n").find("twice") != std::string::npos); // duplicate

    std::string manyReq = std::string(baseData) + "id=a\nrequires=c0";
    for (int i = 1; i < 16; ++i) manyReq += ",c" + std::to_string(i);
    CHECK(ParseError(manyReq + "\n") == "<ok>");
    CHECK(ParseError(manyReq + ",c16\n").find("more than 16") != std::string::npos);

    // Section syntax error
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=native\nentry=a.dll\n[settings\n") == "line 7: a [section] line ends with ]");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=native\nentry=a.dll\n[settings]\n[settings]\n") == "line 8: duplicate section '[settings]'");
}

static void TestCleanStateOnFailure() {
    P::Manifest m;
    std::string err;

    // Initialize m with dummy values
    m.id = "dummy";
    m.name = "Dummy";
    m.version = "1.0";
    m.apiMajor = 1;
    m.kind = P::Kind::Native;

    // Fail parse on invalid input
    bool res = Parse("id = my_plugin\nname = ", m, err);
    CHECK(!res);
    CHECK(!err.empty());
    // On failure, out is reset to default Manifest{}
    CHECK(m.id.empty());
    CHECK(m.name.empty());
    CHECK(m.version.empty());
    CHECK(m.apiMajor == 0);
}

int main() {
    TestKindName();
    TestValidManifests();
    TestInvalidManifests();
    TestCleanStateOnFailure();

    std::printf("test_manifest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
