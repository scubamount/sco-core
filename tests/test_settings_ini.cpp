// Unit tests for plugin.ini [settings] line parsing and setting value checks
// (src/plugins/settings_ini.cpp, src/plugins/internal.h, include/sco/plugins.h).
#include "sco/plugins.h"
#include "../src/plugins/internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace P = sco::plugins;
namespace detail = sco::plugins::detail;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void TestSettingTypeName() {
    CHECK(std::strcmp(P::SettingTypeName(P::SettingType::Bool), "bool") == 0);
    CHECK(std::strcmp(P::SettingTypeName(P::SettingType::Int), "int") == 0);
    CHECK(std::strcmp(P::SettingTypeName(P::SettingType::Float), "float") == 0);
    CHECK(std::strcmp(P::SettingTypeName(P::SettingType::String), "string") == 0);
    CHECK(std::strcmp(P::SettingTypeName(P::SettingType::Enum), "enum") == 0);
    CHECK(std::strcmp(P::SettingTypeName(static_cast<P::SettingType>(99)), "?") == 0);
}

static void TestStripSettingsComment() {
    // Plain lines
    CHECK(detail::StripSettingsComment("god_mode = bool") == "god_mode = bool");
    CHECK(detail::StripSettingsComment("  speed = int  ") == "  speed = int  ");

    // Comments with ; or #
    CHECK(detail::StripSettingsComment("god_mode = bool ; trailing comment") == "god_mode = bool ");
    CHECK(detail::StripSettingsComment("speed = int # comment") == "speed = int ");
    CHECK(detail::StripSettingsComment("; whole line comment") == "");
    CHECK(detail::StripSettingsComment("# whole line comment") == "");
    CHECK(detail::StripSettingsComment("\t; tab lead comment") == "\t");

    // Comment markers inside double quotes should be ignored
    CHECK(detail::StripSettingsComment("nick = string default \"#hello;world\" ; comment") == "nick = string default \"#hello;world\" ");
    CHECK(detail::StripSettingsComment("label = \"a;b#c\"") == "label = \"a;b#c\"");

    // Comment marker without whitespace preceding it (not at start) is part of the token
    CHECK(detail::StripSettingsComment("name=C#;x") == "name=C#;x");
}

static void TestParseAndFormatSettingValue() {
    P::SettingValue v;
    std::string why;

    // --- Bool ---
    P::Setting sb;
    sb.name = "b";
    sb.type = P::SettingType::Bool;

    CHECK(P::ParseSettingValue(sb, "true", v, why) && v.b == true);
    CHECK(P::FormatSettingValue(sb, v) == "true");
    CHECK(P::ParseSettingValue(sb, "false", v, why) && v.b == false);
    CHECK(P::FormatSettingValue(sb, v) == "false");

    CHECK(!P::ParseSettingValue(sb, "TRUE", v, why) && why == "'TRUE' is not true or false");
    CHECK(!P::ParseSettingValue(sb, "1", v, why) && why == "'1' is not true or false");
    CHECK(!P::ParseSettingValue(sb, "0", v, why) && why == "'0' is not true or false");
    CHECK(!P::ParseSettingValue(sb, "yes", v, why) && why == "'yes' is not true or false");

    // --- Int ---
    P::Setting si;
    si.name = "i";
    si.type = P::SettingType::Int;

    CHECK(P::ParseSettingValue(si, "0", v, why) && v.i == 0);
    CHECK(P::FormatSettingValue(si, v) == "0");
    CHECK(P::ParseSettingValue(si, "123", v, why) && v.i == 123);
    CHECK(P::FormatSettingValue(si, v) == "123");
    CHECK(P::ParseSettingValue(si, "-456", v, why) && v.i == -456);
    CHECK(P::FormatSettingValue(si, v) == "-456");
    CHECK(P::ParseSettingValue(si, "+789", v, why) && v.i == 789);

    // Int Boundaries
    CHECK(P::ParseSettingValue(si, "9223372036854775807", v, why) && v.i == INT64_MAX);
    CHECK(P::ParseSettingValue(si, "-9223372036854775808", v, why) && v.i == INT64_MIN);

    // Int Overflows & Bad Input
    CHECK(!P::ParseSettingValue(si, "9223372036854775808", v, why) && why == "'9223372036854775808' is not an integer");
    CHECK(!P::ParseSettingValue(si, "-9223372036854775809", v, why) && why == "'-9223372036854775809' is not an integer");
    CHECK(!P::ParseSettingValue(si, "99999999999999999999", v, why) && why == "'99999999999999999999' is not an integer");
    CHECK(!P::ParseSettingValue(si, "", v, why) && why == "'' is not an integer");
    CHECK(!P::ParseSettingValue(si, "+", v, why) && why == "'+' is not an integer");
    CHECK(!P::ParseSettingValue(si, "-", v, why) && why == "'-' is not an integer");
    CHECK(!P::ParseSettingValue(si, "12a", v, why) && why == "'12a' is not an integer");
    CHECK(!P::ParseSettingValue(si, "1.5", v, why) && why == "'1.5' is not an integer");

    // Int Range Checks
    si.hasMin = true;
    si.minI = 1;
    si.hasMax = true;
    si.maxI = 10;
    CHECK(P::ParseSettingValue(si, "5", v, why) && v.i == 5);
    CHECK(!P::ParseSettingValue(si, "0", v, why) && why == "0 is below min 1");
    CHECK(!P::ParseSettingValue(si, "11", v, why) && why == "11 is above max 10");

    // --- Float ---
    P::Setting sf;
    sf.name = "f";
    sf.type = P::SettingType::Float;

    CHECK(P::ParseSettingValue(sf, "0", v, why) && v.f == 0.0);
    CHECK(P::ParseSettingValue(sf, "12.34", v, why) && std::abs(v.f - 12.34) < 1e-9);
    CHECK(P::ParseSettingValue(sf, "-0.56", v, why) && std::abs(v.f - (-0.56)) < 1e-9);
    CHECK(P::ParseSettingValue(sf, "+1e-3", v, why) && std::abs(v.f - 0.001) < 1e-9);
    CHECK(P::ParseSettingValue(sf, "1.2E2", v, why) && std::abs(v.f - 120.0) < 1e-9);

    // Float Round Trip
    v.f = 3.14159265358979323846;
    std::string formatted = P::FormatSettingValue(sf, v);
    P::SettingValue back;
    CHECK(P::ParseSettingValue(sf, formatted, back, why) && back.f == v.f);

    // Bad Float Inputs
    for (const char* bad : { "nan", "inf", "-inf", "1e999", ".5", "5.", "1e", "1,5", "--1", "1 2" }) {
        CHECK(!P::ParseSettingValue(sf, bad, v, why));
        CHECK(why.find("is not a number") != std::string::npos);
    }

    // Float Range Checks
    sf.hasMin = true;
    sf.minF = 1.0;
    sf.hasMax = true;
    sf.maxF = 10.0;
    CHECK(P::ParseSettingValue(sf, "5.5", v, why) && v.f == 5.5);
    CHECK(!P::ParseSettingValue(sf, "0.5", v, why) && why.find("below min") != std::string::npos);
    CHECK(!P::ParseSettingValue(sf, "10.5", v, why) && why.find("above max") != std::string::npos);

    // --- String ---
    P::Setting ss;
    ss.name = "s";
    ss.type = P::SettingType::String;

    CHECK(P::ParseSettingValue(ss, "Hello World", v, why) && v.s == "Hello World");
    CHECK(P::FormatSettingValue(ss, v) == "Hello World");
    CHECK(P::ParseSettingValue(ss, "", v, why) && v.s == "");

    // String Length and Printable Checks
    std::string valid255(255, 'a');
    CHECK(P::ParseSettingValue(ss, valid255, v, why) && v.s == valid255);
    std::string invalid256(256, 'a');
    CHECK(!P::ParseSettingValue(ss, invalid256, v, why) && why == "a string is at most 255 printable characters");

    std::string unprintable = "hello\tworld";
    CHECK(!P::ParseSettingValue(ss, unprintable, v, why) && why == "a string is at most 255 printable characters");

    // --- Enum ---
    P::Setting se;
    se.name = "e";
    se.type = P::SettingType::Enum;
    se.choices = { "easy", "normal", "hard" };

    CHECK(P::ParseSettingValue(se, "easy", v, why) && v.s == "easy");
    CHECK(P::FormatSettingValue(se, v) == "easy");
    CHECK(P::ParseSettingValue(se, "hard", v, why) && v.s == "hard");
    CHECK(!P::ParseSettingValue(se, "nightmare", v, why) && why == "'nightmare' is not one of easy, normal, hard");
}

static void TestCheckSettingValue() {
    P::SettingValue v;
    std::string why;

    // Non-finite float
    P::Setting sf;
    sf.type = P::SettingType::Float;
    v.f = std::numeric_limits<double>::quiet_NaN();
    CHECK(!P::CheckSettingValue(sf, v, why) && why == "not a finite number");
    v.f = std::numeric_limits<double>::infinity();
    CHECK(!P::CheckSettingValue(sf, v, why) && why == "not a finite number");

    // Unknown setting type
    P::Setting su;
    su.type = static_cast<P::SettingType>(99);
    CHECK(!P::CheckSettingValue(su, v, why));
}

static void TestParseSettingLine() {
    std::vector<P::Setting> out;
    std::string why;

    // --- Valid Declarations ---
    CHECK(detail::ParseSettingLine("god_mode = bool default false label \"God mode\" help \"No damage\"", out, why));
    CHECK(out.size() == 1 && out.back().name == "god_mode" && out.back().type == P::SettingType::Bool &&
          out.back().def.b == false && out.back().label == "God mode" && out.back().help == "No damage");

    CHECK(detail::ParseSettingLine("speed = int default 5 min 1 max 10", out, why));
    CHECK(out.size() == 2 && out.back().name == "speed" && out.back().type == P::SettingType::Int &&
          out.back().def.i == 5 && out.back().hasMin && out.back().minI == 1 && out.back().hasMax && out.back().maxI == 10 &&
          out.back().label == "speed"); // default label is setting name when omitted

    CHECK(detail::ParseSettingLine("fov = float default 90 min 60 max 120", out, why));
    CHECK(out.size() == 3 && out.back().type == P::SettingType::Float && out.back().def.f == 90.0 &&
          out.back().minF == 60.0 && out.back().maxF == 120.0);

    CHECK(detail::ParseSettingLine("nickname = string default \"Pilot\"", out, why));
    CHECK(out.size() == 4 && out.back().type == P::SettingType::String && out.back().def.s == "Pilot");

    CHECK(detail::ParseSettingLine("difficulty = enum(easy,normal,hard) default normal", out, why));
    CHECK(out.size() == 5 && out.back().type == P::SettingType::Enum && out.back().def.s == "normal" &&
          out.back().choices.size() == 3 && out.back().choices[0] == "easy");

    CHECK(detail::ParseSettingLine("bare = string default Pilot label Name", out, why));
    CHECK(out.size() == 6 && out.back().def.s == "Pilot" && out.back().label == "Name");

    // Default values when default parameter is omitted
    out.clear();
    CHECK(detail::ParseSettingLine("b = bool", out, why) && out.back().def.b == false);
    CHECK(detail::ParseSettingLine("i = int min 3 max 10", out, why) && out.back().def.i == 3);
    CHECK(detail::ParseSettingLine("imax = int max -5", out, why) && out.back().def.i == -5);
    CHECK(detail::ParseSettingLine("f = float min 0.5 max 2.0", out, why) && out.back().def.f == 0.5);
    CHECK(detail::ParseSettingLine("fmax = float max -1.5", out, why) && out.back().def.f == -1.5);
    CHECK(detail::ParseSettingLine("e = enum(opt1,opt2)", out, why) && out.back().def.s == "opt1");

    // --- Invalid Declarations ---
    out.clear();

    // Syntax & Name errors
    CHECK(!detail::ParseSettingLine("just words", out, why) && why == "expected name = type ...");
    CHECK(!detail::ParseSettingLine("Speed = int", out, why) && why == "setting name must be 1-31 of [a-z0-9_]");
    CHECK(!detail::ParseSettingLine(" = int", out, why) && why == "setting name must be 1-31 of [a-z0-9_]");
    CHECK(!detail::ParseSettingLine("a_name_longer_than_thirty_one_bytes_x = int", out, why) && why == "setting name must be 1-31 of [a-z0-9_]");

    // Duplicate name
    CHECK(detail::ParseSettingLine("speed = int", out, why));
    CHECK(!detail::ParseSettingLine("speed = int", out, why) && why == "setting 'speed': declared twice");

    // Max settings count
    out.clear();
    for (int k = 0; k < 32; ++k) {
        std::string line = "s" + std::to_string(k) + " = bool";
        CHECK(detail::ParseSettingLine(line, out, why));
    }
    CHECK(out.size() == 32);
    CHECK(!detail::ParseSettingLine("s32 = bool", out, why) && why == "setting 's32': more than 32 settings");

    out.clear();

    // Type errors
    CHECK(!detail::ParseSettingLine("a =", out, why) && why == "setting 'a': missing type (bool, int, float, string or enum(a,b,c))");
    CHECK(!detail::ParseSettingLine("a = \"int\"", out, why) && why == "setting 'a': missing type (bool, int, float, string or enum(a,b,c))");
    CHECK(!detail::ParseSettingLine("a = integer", out, why) && why == "setting 'a': type must be bool, int, float, string or enum(a,b,c)");
    CHECK(!detail::ParseSettingLine("a = Bool", out, why) && why == "setting 'a': type must be bool, int, float, string or enum(a,b,c)");

    // Enum type errors
    CHECK(!detail::ParseSettingLine("a = enum(x", out, why) && why == "setting 'a': enum( needs a closing )");
    CHECK(!detail::ParseSettingLine("a = enum(x)default x", out, why) && why == "setting 'a': expected a space after the type");
    CHECK(!detail::ParseSettingLine("a = enum(x,x)", out, why) && why == "setting 'a': enum lists a choice twice");
    CHECK(!detail::ParseSettingLine("a = enum()", out, why) && why == "setting 'a': enum choices are a comma list of 1-31 of [a-z0-9_]");
    CHECK(!detail::ParseSettingLine("a = enum(X)", out, why) && why == "setting 'a': enum choices are a comma list of 1-31 of [a-z0-9_]");
    CHECK(!detail::ParseSettingLine("a = enum(" + std::string(32, 'c') + ")", out, why) && why == "setting 'a': enum choices are a comma list of 1-31 of [a-z0-9_]");

    // Enum max choices (16)
    std::string overChoices = "a = enum(c0";
    for (int i = 1; i < 17; ++i) overChoices += ",c" + std::to_string(i);
    overChoices += ")";
    CHECK(!detail::ParseSettingLine(overChoices, out, why) && why == "setting 'a': enum lists more than 16 choices");

    // Fields & Quotes errors
    CHECK(!detail::ParseSettingLine("a = bool default", out, why) && why == "setting 'a': field 'default' needs a value");
    CHECK(!detail::ParseSettingLine("a = bool default true default false", out, why) && why == "setting 'a': field 'default' given twice");
    CHECK(!detail::ParseSettingLine("a = bool defualt true", out, why) && why == "setting 'a': unknown field 'defualt' (default, min, max, label, help)");
    CHECK(!detail::ParseSettingLine("a = bool \"default\" true", out, why) && why == "setting 'a': unknown field 'default' (default, min, max, label, help)");
    CHECK(!detail::ParseSettingLine("a = bool default \"true", out, why) && why == "setting 'a': a quote is not closed, or not at the start of a value");
    CHECK(!detail::ParseSettingLine("a = string default \"x\"y", out, why) && why == "setting 'a': a quote is not closed, or not at the start of a value");
    CHECK(!detail::ParseSettingLine("a = string default x\"y\"", out, why) && why == "setting 'a': a quote is not closed, or not at the start of a value");

    // Field value checks
    CHECK(!detail::ParseSettingLine("a = bool min 0", out, why) && why == "setting 'a': min and max apply to int and float only");
    CHECK(!detail::ParseSettingLine("a = int min 10 max 5", out, why) && why == "setting 'a': min 10 is above max 5");
    CHECK(!detail::ParseSettingLine("a = float min 10.0 max 5.0", out, why) && why == "setting 'a': min 10.0 is above max 5.0");
    CHECK(!detail::ParseSettingLine("a = int default 15 max 10", out, why) && why == "setting 'a': default 15 is above max 10");
    CHECK(!detail::ParseSettingLine("a = int default 0 min 5", out, why) && why == "setting 'a': default 0 is below min 5");

    // Label and Help validation
    CHECK(!detail::ParseSettingLine("a = bool label \"\"", out, why) && why == "setting 'a': label must be 1-63 printable characters");
    CHECK(!detail::ParseSettingLine("a = bool label \"" + std::string(64, 'x') + "\"", out, why) && why == "setting 'a': label must be 1-63 printable characters");
    CHECK(!detail::ParseSettingLine("a = bool help \"\"", out, why) && why == "setting 'a': help must be 1-255 printable characters");
    CHECK(!detail::ParseSettingLine("a = bool help \"" + std::string(256, 'x') + "\"", out, why) && why == "setting 'a': help must be 1-255 printable characters");
}

int main() {
    TestSettingTypeName();
    TestStripSettingsComment();
    TestParseAndFormatSettingValue();
    TestCheckSettingValue();
    TestParseSettingLine();

    std::printf("settings_ini tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
