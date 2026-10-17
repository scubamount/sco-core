#!/usr/bin/env python3
"""Packages the sco SDK. What goes in a zip is decided by sdk/components.json and nothing else.

    sdk/package.py [--profile core|core-source|host-source-sc|game-sc|legacy] [--out DIR]   # default: legacy, sdk/out
    sdk/package.py --check                                        # validate the manifest only
    sdk/package.py --list-headers core,game-sc                    # public headers of those profiles

Profiles (sdk/components.json "profiles"):
  core     sco-sdk-core-<sdk/VERSION>.zip: the game-neutral plugin SDK. Its version comes from
           sdk/VERSION only; it never reads a game-pack header.
  game-sc  sco-sdk-game-sc-<SCO_GAME_PACK_VERSION>.zip: the optional Star Citizen extension (game
           service tables and events, DataCore, their wrappers, game examples and docs). Unpacked
           over the core SDK of the version its VERSIONS.txt names.
  core-source sco-sdk-core-source-<sdk/VERSION>.zip: the kernel source-development kit,
           keeping repository paths, with synthetic tests and no game implementation.
  host-source-sc sco-sdk-host-source-sc-<sdk/VERSION>.zip: standalone Star Citizen
           host source with the neutral kernel, game implementation and synthetic tests.
  legacy   sco-sdk-<sdk/VERSION>.zip: core and game-sc in one zip, as sdk-v1.1.0 shipped. Not
           game-neutral; kept until the consumers move to core + game-sc.

A zip holds one folder with the selected files, versions, compatibility, attributions, SPDX 2.3
SBOM and SHA256SUMS. Generated metadata in game-sc uses a .game-sc suffix, so composition keeps
both archives' records. Its compatibility descriptor binds the exact base core archive hash;
test-zip.sh checks it before copying. The same sources and Python/zlib environment give the same
bytes: entries are sorted and carry a fixed timestamp and fixed modes.

The manifest is checked before anything is written, and any doubt stops the build:
  - every file under the folders in "covers" is a component or an "excluded" entry, and every
    component's source is a regular file in the repository (no folders, no symlinks);
  - every component has an id, source, destination, profiles, origin and license, and no key
    beyond the known ones; ids and destinations are unique; destinations are safe relative paths;
  - a profile names only known profiles in "includes" / "requires", and they form no cycle;
  - within any profile's selection (its own, included and required profiles) each file is
    written once;
  - the core profile contains no game-sc component.

Run by .github/workflows/sdk.yml; CI then builds the examples from the unpacked zip alone.
Tests: python3 sdk/test_package.py
"""
import argparse
import fnmatch
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

SDK = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SDK)
MANIFEST = os.path.join(SDK, "components.json")
REPO_URL = "https://github.com/scubamount/sco-core/"
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
ORIGINS = {"project-authored", "third-party", "generated", "interoperability-derived"}
REQUIRED = ("id", "source", "destination", "profiles", "origin", "license")
COMPONENT_KEYS = REQUIRED + ("public_header", "group", "note", "upstream")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include\s*([<"])([^>"]+)[>"]', re.M)


class ManifestError(Exception):
    pass


def tracked_files(root):
    """Files git tracks under root, or every file when root isn't a git checkout (an unpacked zip)."""
    try:
        out = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True)
        return [p for p in out.stdout.decode("utf-8").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            files += [os.path.relpath(os.path.join(dirpath, n), root).replace(os.sep, "/") for n in filenames]
        return files


def safe_rel(path):
    return (isinstance(path, str) and path and not path.startswith("/") and "\\" not in path and ":" not in path
            and all(part not in ("", ".", "..") for part in path.split("/")))


def profile_refs(m, name):
    """A profile's 'includes' plus 'requires', as profile names."""
    p = m["profiles"][name]
    refs = []
    for key in ("includes", "requires"):
        r = p.get(key, [])
        refs += [r] if isinstance(r, str) else list(r)
    return refs


def closure(m, name):
    """Every profile a profile's zip touches: itself, what it includes and what it requires,
    transitively. Refuses a cycle with the path that closes it."""
    seen, path = set(), []

    def walk(n):
        if n in path:
            cycle = path[path.index(n):] + [n]
            raise ManifestError(f"profiles form a cycle: {' -> '.join(cycle)}")
        if n in seen:
            return
        path.append(n)
        for r in profile_refs(m, n):
            if r not in m["profiles"]:
                raise ManifestError(f"profile {n}: names unknown profile {r}")
            walk(r)
        path.pop()
        seen.add(n)

    walk(name)
    return seen


def load_manifest(path=MANIFEST, root=ROOT):
    """The parsed manifest after every check. Raises ManifestError."""
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError) as e:
        raise ManifestError(f"{os.path.basename(path)}: can't read it ({e})")
    if not isinstance(m, dict) or m.get("schema_version") != 1:
        raise ManifestError("schema_version must be 1")
    profiles = m.get("profiles")
    if not isinstance(profiles, dict) or not {"core", "game-sc"} <= set(profiles):
        raise ManifestError("profiles must declare core and game-sc")
    for name, p in profiles.items():
        for key in ("includes", "requires"):
            refs = p.get(key, [])
            refs = [refs] if isinstance(refs, str) else refs
            bad = [r for r in refs if r not in profiles or r == name]
            if bad:
                raise ManifestError(f"profile {name}: {key} names unknown profile(s) {', '.join(bad)}")
        if p.get("layout", "sdk") not in ("sdk", "source"):
            raise ManifestError(f"profile {name}: unknown layout")
        if not p.get("zip") or not p.get("version_from"):
            raise ManifestError(f"profile {name}: needs zip and version_from")
    comps = m.get("components")
    if not isinstance(comps, list) or not comps:
        raise ManifestError("no components")
    ids, dsts, srcs = set(), {}, set()
    reserved = {n for prof in profiles for n in metadata_names(m, prof)}   # written by package.py
    for i, c in enumerate(comps):
        if not isinstance(c, dict):
            raise ManifestError(f"component {i} is not an object")
        missing = [k for k in REQUIRED if not c.get(k)]
        if missing:
            raise ManifestError(f"component {c.get('id', i)}: missing {', '.join(missing)}")
        unknown = [k for k in c if k not in COMPONENT_KEYS]
        if unknown:
            raise ManifestError(f"component {c['id']}: unknown key(s) {', '.join(sorted(unknown))}")
        cid = c["id"]
        if cid in ids:
            raise ManifestError(f"component {cid}: id used twice")
        ids.add(cid)
        if c["origin"] not in ORIGINS:
            raise ManifestError(f"component {cid}: origin '{c['origin']}' is not one of {', '.join(sorted(ORIGINS))}")
        if not isinstance(c["profiles"], list) or not set(c["profiles"]) <= set(profiles):
            raise ManifestError(f"component {cid}: profiles must be a list of declared profiles")
        if "core" in c["profiles"] and "game-sc" in c["profiles"] and c["source"] != "LICENSE":
            raise ManifestError(f"component {cid}: in both core and game-sc; core files reach game-sc through 'requires'")
        for key in ("source", "destination"):
            if not safe_rel(c[key]):
                raise ManifestError(f"component {cid}: {key} '{c[key]}' is not a safe relative path")
        if c["destination"] in reserved:
            raise ManifestError(f"component {cid}: destination {c['destination']} is written by package.py")
        full = os.path.join(root, c["source"])
        if os.path.islink(full) or not os.path.isfile(full):
            raise ManifestError(f"component {cid}: source {c['source']} is not a regular file")
        if c["source"] in srcs:
            raise ManifestError(f"component {cid}: source {c['source']} listed twice")
        srcs.add(c["source"])
        for prof in c["profiles"]:
            dst = destination(m, prof, c)
            key = (prof, dst)
            if key in dsts:
                raise ManifestError(f"components {dsts[key]} and {cid}: same destination {dst} in {prof}")
            dsts[key] = cid
    # Coverage: nothing under a covered folder is shipped or skipped by accident.
    excluded = m.get("excluded", [])
    covers = m.get("covers", [])
    def is_excluded(p):
        return any(p == e["path"] or (e["path"].endswith("/") and p.startswith(e["path"])) for e in excluded)
    for e in excluded:
        if not e.get("path") or not e.get("why"):
            raise ManifestError("an excluded entry needs path and why")
    # Closure: within one profile's selection (itself, what it includes and what it requires) each
    # destination is written once. A core file and a game-sc file with one destination would
    # overwrite each other where the extension unpacks over the core.
    closures = {name: closure(m, name) for name in profiles}   # also refuses a cycle, once per profile
    for name, reached in closures.items():
        written = {}
        for c in comps:
            if set(c["profiles"]) & reached:
                dst = destination(m, name, c)
                other = written.get(dst)
                if other is not None and other != c["id"]:
                    raise ManifestError(f"profile {name}: {other} and {c['id']} both write {dst}")
                written[dst] = c["id"]
    unclassified = [p for p in tracked_files(root)
                    if any(p == c or p.startswith(c + "/") for c in covers) and p not in srcs and not is_excluded(p)]
    if unclassified:
        raise ManifestError("not classified in sdk/components.json: " + ", ".join(sorted(unclassified)))
    for name in profiles:
        select(m, name)
    return m


def destination(m, profile, component):
    """Source-development archives retain repository layout; plugin kits retain SDK layout."""
    return component["source"] if m["profiles"][profile].get("layout") == "source" else component["destination"]


def select(m, profile):
    """The components a profile's zip holds: its own, plus those of the profiles it 'includes'."""
    p = m["profiles"].get(profile)
    if p is None:
        raise ManifestError(f"unknown profile {profile}")
    wanted = {profile, *p.get("includes", [])}
    chosen = [c for c in m["components"] if wanted & set(c["profiles"])]
    if profile == "core":
        game = [c["id"] for c in chosen if "game-sc" in c["profiles"] and c["source"] != "LICENSE"]
        if game:
            raise ManifestError(f"core would hold game-sc components: {', '.join(game)}")
    if profile == "core-source":
        forbidden = ("src/game/", "src/datacore/", "include/sco/game/", "include/sco/datacore",
                     "plugins/lua/sco_lua_game.", "third_party/tomlplusplus/")
        game = [c["source"] for c in chosen
                if c["source"].startswith(forbidden) or c.get("group") == "game-contract" or
                ("game-sc" in c["profiles"] and c["source"] != "LICENSE")]
        if game:
            raise ManifestError("core-source would hold game implementation or contracts: " + ", ".join(game))
    seen = {}
    for c in chosen:
        dst = destination(m, profile, c)
        if dst in seen and seen[dst] != c["id"]:
            raise ManifestError(f"{profile}: {seen[dst]} and {c['id']} both write {dst}")
        seen[dst] = c["id"]
    return chosen


def public_headers(m, profiles):
    """Destinations of the public headers (public_header: true) in any of the given profiles."""
    return sorted({c["destination"] for c in m["components"] if c.get("public_header") and set(profiles) & set(c["profiles"])})


def check_example_headers(m, profile, text, source):
    """Resolve each SDK include to an exact public-header file in this profile/overlay.

    A shipped directory is not evidence that an arbitrary nested header ships.
    Quoted local headers must also be individual selected components.
    """
    reached = closure(m, profile)
    components = [c for c in m["components"] if reached & set(c["profiles"])]
    shipped = {destination(m, profile, c) for c in components}
    public = {destination(m, profile, c) for c in components if c.get("public_header")}
    example = next((destination(m, profile, c) for c in components if c["source"] == source), source)
    for delimiter, header in INCLUDE_RE.findall(text):
        candidates = {"include/" + header, "third_party/imgui/" + header,
                      "sdk/third_party/imgui/" + header}
        if candidates & public:
            continue
        local = os.path.normpath(os.path.join(os.path.dirname(example), header)).replace(os.sep, "/")
        if delimiter == '"' and local in shipped:
            continue
        if delimiter == '"' or header.startswith(("scosdk/", "sco_", "sc_", "sco/")) or header == "imgui.h":
            raise ManifestError(f"{profile}: {source} includes unshipped public header {header}")


def check_installed_example_headers(m, profile, root, work):
    """Compile external copies of installed C/C++ examples; police actual dependencies.

    Compiler/toolchain headers are allowed, but every SDK header must resolve to
    an exact public manifest destination. No include-directory prefix grants access
    to private or unshipped SDK files. Macro and transitive includes are checked too.
    """
    root, work = Path(root).resolve(), Path(work).resolve()
    reached = closure(m, profile)
    components = [c for c in m["components"] if reached & set(c["profiles"])]
    public = {str(root / destination(m, profile, c)): destination(m, profile, c)
              for c in components if c.get("public_header")}
    examples = [c for c in components if c["source"].startswith(("sdk/examples/", "sdk/template/"))
                and c["source"].endswith((".c", ".cpp", ".h", ".hpp"))]
    units = [c for c in examples if c["source"].endswith((".c", ".cpp"))]
    if not units:
        raise ManifestError(f"{profile}: installed example compilation would be empty")
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise ManifestError(f"{profile}: compiler proof workspace must be empty")
    local = set()
    for c in examples:
        installed = root / destination(m, profile, c)
        if not installed.is_file() or installed.resolve() != installed:
            raise ManifestError(f"{profile}: missing or redirected installed example {installed}")
        external = work / destination(m, profile, c)
        external.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(installed, external)
        local.add(str(external))
    for header in public:
        if not Path(header).is_file() or str(Path(header).resolve()) != header:
            raise ManifestError(f"{profile}: missing or redirected installed public header {header}")

    # Visual Studio computes the Windows SDK/CRT paths inside MSBuild, even
    # when CMake's invoking shell has no INCLUDE. Record the evaluated search
    # path immediately before compilation, not a guessed installation prefix.
    import xml.etree.ElementTree as ET
    build = work / 'build'
    for lang in ('C', 'CXX'):
        props = ET.Element('Project', xmlns='http://schemas.microsoft.com/developer/msbuild/2003')
        target = ET.SubElement(props, 'Target', Name='SdkHeaderProofSystemDirs', BeforeTargets='ClCompile')
        ET.SubElement(target, 'WriteLinesToFile', File=str(build / f'system-{lang}.txt'),
                      Lines='$(IncludePath)', Overwrite='true')
        ET.ElementTree(props).write(work / f'system-{lang}.props', encoding='utf-8', xml_declaration=True)

    def cmake_path(path):
        return '[=[' + str(path).replace('\\', '/') + ']=]'

    lines = ['cmake_minimum_required(VERSION 3.20)',
             'if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_SYSROOT)',
             '  execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE CMAKE_OSX_SYSROOT '
             'OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)',
             'endif()', 'project(sdk_header_proof C CXX)',
             # Own stable dependency outputs, independent of Make/Ninja consumption.
             'set(CMAKE_DEPFILE_FLAGS_C "")', 'set(CMAKE_DEPFILE_FLAGS_CXX "")',
             'foreach(lang C CXX)',
             '  set(system_dirs "${CMAKE_${lang}_IMPLICIT_INCLUDE_DIRECTORIES}")',
             '  list(APPEND system_dirs "${CMAKE_SYSROOT}" "${CMAKE_OSX_SYSROOT}")',
             '  if(MSVC)',
             '    list(APPEND system_dirs "$ENV{INCLUDE}")',
             '  endif()',
             '  string(REPLACE ";" "\\n" system_dirs "${system_dirs}")',
             '  file(WRITE "${CMAKE_BINARY_DIR}/system-${lang}.txt" "${system_dirs}")',
             'endforeach()']
    include_dirs = sorted({str(Path(p).parent) for p in public if
                           not Path(p).relative_to(root).as_posix().startswith('include/')}
                          | {str(root / 'include')})
    for i, c in enumerate(units):
        source = work / destination(m, profile, c)
        lines += [f'add_library(proof{i} OBJECT {cmake_path(source)})',
                  f'target_include_directories(proof{i} PRIVATE ' +
                  ' '.join(map(cmake_path, include_dirs)) + ')',
                  f'set_target_properties(proof{i} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON '
                  'CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)',
                  'if(MSVC)',
                  f'  set_target_properties(proof{i} PROPERTIES VS_USER_PROPS ' +
                  cmake_path(work / ('system-C.props' if c['source'].endswith('.c') else 'system-CXX.props')) + ')',
                  f'  target_compile_options(proof{i} PRIVATE /sourceDependencies '
                  f'"${{CMAKE_BINARY_DIR}}/proof{i}.json")',
                  'elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" AND '
                  'CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")',
                  # -MD, not -MMD: even an SDK file treated as a system header is checked.
                  f'  target_compile_options(proof{i} PRIVATE -MD -MF '
                  f'"${{CMAKE_BINARY_DIR}}/proof{i}.d" -MT sdk_header_proof)',
                  'else()',
                  '  message(FATAL_ERROR "No supported compiler dependency format")',
                  'endif()']
    (work / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
    build = work / 'build'
    env = os.environ.copy()
    # Ambient flags are not part of the installed SDK contract. MSVC's INCLUDE
    # is retained for its toolchain, then bounded by the checks below.
    for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
                'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CL', '_CL_', 'CMAKE_PREFIX_PATH'):
        env.pop(key, None)
    for command in (['cmake', '-S', str(work), '-B', str(build)],
                    ['cmake', '--build', str(build), '--config', 'Release']):
        result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True)
        if result.returncode:
            raise ManifestError(f"{profile}: installed example compile failed:\n{result.stdout}{result.stderr}")

    def beneath(path, parent):
        return path == parent or parent in path.parents

    seen_public, languages = set(), set()
    checkout = Path(ROOT).resolve()
    for i, c in enumerate(units):
        lang = 'C' if c['source'].endswith('.c') else 'CXX'
        languages.add(lang)
        system = [Path(p).resolve() for p in
                  (build / f'system-{lang}.txt').read_text(encoding='utf-8-sig').splitlines() if p]
        depfile, msdep = build / f'proof{i}.d', build / f'proof{i}.json'
        if msdep.exists():
            data = json.loads(msdep.read_text(encoding='utf-8-sig'))['Data']
            dependencies = [data['Source'], *data['Includes']]
        elif depfile.exists():
            text = depfile.read_text().replace('\\\n', '')
            target, separator, body = text.partition(':')
            if target != 'sdk_header_proof' or not separator:
                raise ManifestError(f"{profile}: invalid compiler dependency target for {c['source']}")
            dependencies = [re.sub(r'\\(.)', r'\1', p).replace('$$', '$')
                            for p in re.findall(r'(?:\\.|[^\s])+', body)]
        else:
            raise ManifestError(f"{profile}: compiler dependency output absent for {c['source']}")
        source = str(work / destination(m, profile, c))
        resolved = {str((build / p).resolve()) for p in dependencies}
        if source not in resolved or not resolved & set(public):
            raise ManifestError(f"{profile}: compiler dependency proof empty for {c['source']}")
        for dep in sorted(resolved):
            path = Path(dep)
            if dep in public:
                seen_public.add(public[dep])
            elif dep in local:
                continue
            elif not beneath(path, root) and not beneath(path, checkout) and any(
                    p != Path(p.anchor) and not beneath(p, root) and not beneath(p, checkout)
                    and beneath(path, p) for p in system):
                continue
            else:
                raise ManifestError(f"{profile}: {c['source']} resolved dependency {path.as_posix()} "
                                    "outside approved installed files")
    return {'translation_units': len(units), 'languages': sorted(languages),
            'public_headers': sorted(seen_public)}


def define(text, name, pattern, where):
    mt = re.search(rf'#define {name} "({pattern})"', text)
    if not mt:
        raise ManifestError(f"{where}: no {name}")
    return mt.group(1)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def sdk_version():
    v = read("sdk/VERSION").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9.]+)?", v):
        raise ManifestError(f"sdk/VERSION: '{v}' is not a version")
    return v


def abi_version():
    api = read("include/sco_api.h")
    major = re.search(r"#define SCO_API_MAJOR ([0-9]+)", api)
    minor = re.search(r"#define SCO_API_MINOR ([0-9]+)", api)
    if not (major and minor):
        raise ManifestError("include/sco_api.h: no SCO_API_MAJOR / SCO_API_MINOR")
    return f"{major.group(1)}.{minor.group(1)}"


def game_pack():
    """(version, verified build, PE timestamp, how) from the shipped game-pack header."""
    pack = read("include/sc_game_pack.h")
    w = "include/sc_game_pack.h"
    return (define(pack, "SCO_GAME_PACK_VERSION", r"[0-9]+\.[0-9]+\.[0-9]+", w),
            define(pack, "SCO_GAME_PACK_VERIFIED_BUILD", r"[0-9]+(?:\.[0-9]+)+", w),
            define(pack, "SCO_GAME_PACK_VERIFIED_PE_TIMESTAMP", r"0x[0-9a-f]+", w),
            define(pack, "SCO_GAME_PACK_VERIFICATION", r"[^\"]+", w))


def versions_txt(profile, version):
    """VERSIONS.txt: every line comes from a header the zip ships (or, for game-sc, the core it needs)."""
    abi = abi_version()
    if profile == "core-source":
        return (f"sco SDK core source {version}\n\nplugin ABI (sco_api.h): {abi}\n"
                "Configure with -DSCO_GAME_SC=OFF. Game implementation and contracts are not included.\n"
                "The frozen release header is hash-checked by tools/abi_snapshot.py.\n")
    if profile == "core":
        return (f"sco SDK core {version}\n\nkernel (sdk/VERSION, tag sdk-v{version}): {version}\n"
                f"plugin ABI (sco_api.h): {abi}\n\nThe Star Citizen game services are a separate extension, "
                f"sco-sdk-game-sc-<version>.zip.\n")
    gp, build, stamp, how = game_pack()
    if profile == "host-source-sc":
        return (f"sco SDK Star Citizen host source {version}\n\n"
                f"kernel (sdk/VERSION): {version}\nplugin ABI (sco_api.h): {abi}\n"
                f"game pack (SCO_GAME_PACK_VERSION): {gp}\n\n"
                "Standalone source kit; configure with -DSCO_GAME_SC=ON. Do not overlay plugin SDK zips.\n"
                "Local synthetic packaging proof only; game runtime and release publication are not verified.\n"
                f"signature metadata: {build} (PE timestamp {stamp}): {how}\n")
    if profile == "game-sc":
        return (f"sco SDK Star Citizen extension {gp}\n\ngame pack (SCO_GAME_PACK_VERSION, Star Citizen): {gp}\n"
                f"needs: sco SDK core {sdk_version()} (plugin ABI {abi}); unpack this over it\n\n"
                f"verified game builds:\n  {build} (StarCitizen.exe PE timestamp {stamp}): {how}\n")
    return (f"sco SDK {version}\n\nkernel (sdk/VERSION, tag sdk-v{version}): {version}\n"
            f"plugin ABI (sco_api.h): {abi}\ngame pack (SCO_GAME_PACK_VERSION, Star Citizen): {gp}\n\n"
            f"verified game builds:\n  {build} (StarCitizen.exe PE timestamp {stamp}): {how}\n")


LINK = re.compile(r"\]\(([^)#\s]+)((?:#[^)]*)?)\)")


def fix_links(src, dst, data, zip_paths, dirs):
    """Markdown links are written to work in the repository. In the zip, a link to a file (or
    folder) this zip ships points at its zip path; a link to anything else points at GitHub."""
    if not dst.endswith(".md"):
        return data
    src_dir, dst_dir = os.path.dirname(src), os.path.dirname(dst)
    commit = source_commit()

    def repl(mt):
        target, anchor = mt.group(1), mt.group(2)
        if re.match(r"[a-z]+:", target):
            return mt.group(0)
        repo_path = os.path.normpath(os.path.join(src_dir, target)).replace(os.sep, "/")
        zip_path = zip_paths.get(repo_path) or dirs.get(repo_path)
        if zip_path is None:
            return f"]({REPO_URL}blob/{commit}/{repo_path}{anchor})"
        return f"]({os.path.relpath(zip_path, dst_dir or '.').replace(os.sep, '/')}{anchor})"

    text = LINK.sub(repl, data.decode("utf-8"))
    # Also pin repository URLs already written as external links, not only rewritten relatives.
    text = re.sub(re.escape(REPO_URL) + r"(blob|tree)/[^/\s)]+/",
                  lambda mt: f"{REPO_URL}{mt.group(1)}/{commit}/", text)
    return text.encode("utf-8")


def source_commit():
    try:
        commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as e:
        raise ManifestError("source commit unavailable; package from a Git checkout") from e
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ManifestError("source commit must be a full Git SHA-1")
    return commit


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def metadata_names(m, profile):
    """Generated metadata keeps archive-specific names when composed."""
    suffix = f".{profile}" if m["profiles"][profile].get("requires") else ""
    return tuple(n + suffix for n in ("VERSIONS.txt", "SHA256SUMS", "COMPATIBILITY.json",
                                      "ATTRIBUTIONS.json", "SBOM.spdx.json"))


def attributions(m, profile):
    provenance = json.loads(read("docs/legal/provenance.json"))["entries"]
    records = {}
    for c in select(m, profile):
        src = c["source"]
        e = next((e for e in provenance if
                  (src.startswith(e["path"][:-2]) if e["path"].endswith("/**") else
                   fnmatch.fnmatchcase(src, e["path"]) and ("/" in e["path"] or "/" not in src))), None)
        if e is None or e["status"] == "unresolved":
            raise ManifestError(f"{src}: missing or unresolved provenance")
        if e["license"] != c["license"] or e["origin"] != c["origin"]:
            raise ManifestError(f"{src}: manifest and provenance disagree")
        dst = destination(m, profile, c)
        records[dst] = {"path": dst, "source": src, "origin": c["origin"], "license": c["license"],
                        "copyright": e["copyright"], "status": e["status"]}
        if e.get("upstream"):
            records[dst]["upstream"] = e["upstream"]
    for name in metadata_names(m, profile):
        records[name] = {"path": name, "source": "sdk/package.py", "origin": "generated",
                         "license": "CC0-1.0" if name.startswith("SBOM.spdx.json") else "GPL-3.0-only",
                         "copyright": "2026 the sco-core contributors", "status": "reviewed"}
    return records


def sbom_document(top, version, entries, records, sbom_name, sums_name, commit):
    files = []
    for name, (data, _) in sorted(entries.items()):
        record = records[name]
        files.append({"SPDXID": "SPDXRef-File-" + hashlib.sha256(name.encode()).hexdigest(),
                      "fileName": "./" + name,
                      "checksums": [{"algorithm": "SHA256", "checksumValue": hashlib.sha256(data).hexdigest()},
                                    {"algorithm": "SHA1", "checksumValue": hashlib.sha1(data).hexdigest()}],
                      "licenseConcluded": record["license"], "licenseInfoInFiles": ["NOASSERTION"],
                      "copyrightText": record["copyright"],
                      "comment": f"source {record['source']}; origin {record['origin']}; status {record['status']}"})
    verification = hashlib.sha1("".join(sorted(f["checksums"][1]["checksumValue"] for f in files)).encode()).hexdigest()
    payload_hash = hashlib.sha256(json_bytes(files)).hexdigest()
    package_id = "SPDXRef-Package"
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": top, "documentNamespace": f"{REPO_URL}sbom/{top}-{payload_hash}",
            "creationInfo": {"created": "2026-01-01T00:00:00Z", "creators": ["Tool: sco-core sdk/package.py"],
                             "comment": f"Source commit {commit}; fixed creation time for byte reproducibility."},
            "packages": [{"SPDXID": package_id, "name": top, "versionInfo": version,
                          "downloadLocation": f"git+https://github.com/scubamount/sco-core@{commit}",
                          "filesAnalyzed": True,
                          "packageVerificationCode": {"packageVerificationCodeValue": verification,
                                                      "packageVerificationCodeExcludedFiles": ["./" + sbom_name, "./" + sums_name]},
                          "licenseConcluded": "NOASSERTION", "licenseDeclared": "NOASSERTION",
                          "copyrightText": "NOASSERTION",
                          "comment": "Self and checksum manifest excluded to avoid circular hashes; SHA256SUMS covers the SBOM."}],
            "files": files,
            "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                               "relatedSpdxElement": package_id}] +
                              [{"spdxElementId": package_id, "relationshipType": "CONTAINS",
                                "relatedSpdxElement": f["SPDXID"]} for f in files]}


def validate_sbom(doc, content, sbom_name, sums_name):
    """Offline stdlib structural/byte check of our SPDX 2.3 subset, not a full SPDX validator."""
    def require(ok, message):
        if not ok:
            raise ManifestError("SPDX 2.3: " + message)

    def strings(obj, fields):
        require(isinstance(obj, dict), "expected an object")
        for key in fields:
            require(isinstance(obj.get(key), str) and bool(obj[key]), f"missing or invalid {key}")

    strings(doc, ("spdxVersion", "dataLicense", "SPDXID", "name", "documentNamespace"))
    require(doc["spdxVersion"] == "SPDX-2.3" and doc["dataLicense"] == "CC0-1.0"
            and doc["SPDXID"] == "SPDXRef-DOCUMENT", "document identity")
    require(re.fullmatch(r"https://[^\s#]+", doc["documentNamespace"]) is not None, "document namespace")
    creation = doc.get("creationInfo")
    strings(creation, ("created",))
    require(creation["created"] == "2026-01-01T00:00:00Z", "creation time")
    creators = creation.get("creators")
    require(isinstance(creators, list) and bool(creators) and all(
        isinstance(c, str) and re.fullmatch(r"(?:Tool|Person|Organization): .+", c) for c in creators), "creators")
    require(isinstance(doc.get("packages"), list) and len(doc["packages"]) == 1, "one package required")
    package = doc["packages"][0]
    strings(package, ("SPDXID", "name", "downloadLocation", "licenseDeclared", "licenseConcluded", "copyrightText"))
    require(package.get("filesAnalyzed") is True, "filesAnalyzed")
    require(isinstance(doc.get("files"), list), "file list")
    ids, names, sha1s = {doc["SPDXID"], package["SPDXID"]}, set(), []
    require(len(ids) == 2 and re.fullmatch(r"SPDXRef-[A-Za-z0-9.-]+", package["SPDXID"]), "package ID")
    for f in doc["files"]:
        strings(f, ("SPDXID", "fileName", "licenseConcluded", "copyrightText"))
        fid, filename = f["SPDXID"], f["fileName"]
        require(re.fullmatch(r"SPDXRef-[A-Za-z0-9.-]+", fid) is not None and fid not in ids, "unique file ID")
        ids.add(fid)
        require(filename.startswith("./") and safe_rel(filename[2:]), "safe file name")
        name = filename[2:]
        require(name in content and name not in names, "unique existing file name")
        names.add(name)
        licenses = f.get("licenseInfoInFiles")
        require(isinstance(licenses, list) and bool(licenses) and all(isinstance(l, str) and l for l in licenses),
                "licenseInfoInFiles")
        checks = f.get("checksums")
        require(isinstance(checks, list) and len(checks) == 2, "SHA256 and SHA1 checksums required")
        by_algorithm = {}
        for checksum in checks:
            strings(checksum, ("algorithm", "checksumValue"))
            require(checksum["algorithm"] not in by_algorithm, "duplicate checksum")
            by_algorithm[checksum["algorithm"]] = checksum["checksumValue"]
        require(by_algorithm == {"SHA256": hashlib.sha256(content[name]).hexdigest(),
                                 "SHA1": hashlib.sha1(content[name]).hexdigest()}, "file checksum mismatch")
        sha1s.append(by_algorithm["SHA1"])
    require(names == set(content) - {sbom_name, sums_name}, "exact file inventory")
    code = package.get("packageVerificationCode")
    require(isinstance(code, dict), "packageVerificationCode")
    require(code.get("packageVerificationCodeValue") == hashlib.sha1("".join(sorted(sha1s)).encode()).hexdigest(),
            "package verification code")
    excluded = code.get("packageVerificationCodeExcludedFiles")
    require(isinstance(excluded, list) and len(excluded) == 2 and set(excluded) == {"./" + sbom_name, "./" + sums_name},
            "explicit self/checksum exclusions")
    relationships = doc.get("relationships")
    require(isinstance(relationships, list), "relationships")
    actual = []
    for rel in relationships:
        strings(rel, ("spdxElementId", "relationshipType", "relatedSpdxElement"))
        require(rel["spdxElementId"] in ids and rel["relatedSpdxElement"] in ids, "relationship endpoint")
        actual.append((rel["spdxElementId"], rel["relationshipType"], rel["relatedSpdxElement"]))
    expected = {(doc["SPDXID"], "DESCRIBES", package["SPDXID"])} | {
        (package["SPDXID"], "CONTAINS", f["SPDXID"]) for f in doc["files"]}
    require(len(actual) == len(expected) and set(actual) == expected, "exact relationships")


def archive_entries(m, profile):
    chosen = select(m, profile)
    p = m["profiles"][profile]
    version = sdk_version() if p["version_from"] == "sdk/VERSION" else game_pack()[0]
    top = p["zip"].format(version=version, game_pack_version=version)
    zip_paths = {c["source"]: destination(m, profile, c) for c in chosen}
    dirs = {}
    for src, dst in zip_paths.items():   # folders: the source folder maps to the destination folder
        s, d = os.path.dirname(src), os.path.dirname(dst)
        while s and d and os.path.basename(s) == os.path.basename(d):
            dirs.setdefault(s, d)
            s, d = os.path.dirname(s), os.path.dirname(d)
    entries = {}
    for c in chosen:
        if c["source"].startswith(("sdk/examples/", "sdk/template/")) and c["source"].endswith((".c", ".cpp", ".h", ".hpp")):
            check_example_headers(m, profile, read(c["source"]), c["source"])
        full = os.path.join(ROOT, c["source"])
        with open(full, "rb") as f:
            dst = destination(m, profile, c)
            entries[dst] = (fix_links(c["source"], dst, f.read(), zip_paths, dirs),
                                         os.access(full, os.X_OK))
    versions_name, sums_name, compat_name, inventory_name, sbom_name = metadata_names(m, profile)
    entries[versions_name] = (versions_txt(profile, version).encode("ascii"), False)
    commit = source_commit()
    desc = {"schema_version": 1, "profile": profile, "core_sdk_version": sdk_version(),
            "plugin_abi_version": abi_version(),
            "game_overlay_version": game_pack()[0] if profile in ("game-sc", "legacy", "host-source-sc") else None,
            "source_commits": {"core": commit}, "base_core_sha256": None}
    if profile in ("game-sc", "legacy", "host-source-sc"):
        desc["source_commits"]["game-sc"] = commit
    if profile == "game-sc":
        core_top, core_entries = archive_entries(m, "core")
        desc["base_core_sha256"] = hashlib.sha256(archive_bytes(core_top, core_entries)).hexdigest()
    entries[compat_name] = (json_bytes(desc), False)
    records = attributions(m, profile)
    entries[inventory_name] = (json_bytes({"schema_version": 1, "profile": profile,
                                         "files": [records[n] for n in sorted(records)]}), False)
    sbom = sbom_document(top, version, entries, records, sbom_name, sums_name, commit)
    entries[sbom_name] = (json_bytes(sbom), False)
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, (data, _) in sorted(entries.items()))
    entries[sums_name] = (sums.encode("ascii"), False)
    return top, entries


def archive_bytes(top, entries):
    # Fail before writing any archive. This is a bounded credential-shape check,
    # not a claim that arbitrary secrets can be recognized from their contents.
    credential = re.compile(rb"-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----|"
                            rb"\bgh[pousr]_[A-Za-z0-9]{36,}\b|"
                            rb"\bgithub_pat_[A-Za-z0-9_]{22,}\b")
    for name, (data, _) in entries.items():
        if credential.search(data):
            raise ManifestError(f"private credential in archive member {name}")
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(entries):
            data, executable = entries[name]
            info = zipfile.ZipInfo(f"{top}/{name}", FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # unix, so the mode bits below are honored
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            z.writestr(info, data)
    return buffer.getvalue()


def build(m, profile, out):
    top, entries = archive_entries(m, profile)
    data = archive_bytes(top, entries)
    os.makedirs(out, exist_ok=True)
    zip_path = os.path.join(out, f"{top}.zip")
    with open(zip_path, "wb") as f:
        f.write(data)
    digest = hashlib.sha256(data).hexdigest()
    print(f"{zip_path}: {len(entries)} files, sha256 {digest}")
    return zip_path


def main():
    ap = argparse.ArgumentParser(description="Package the sco SDK from sdk/components.json.")
    ap.add_argument("--profile", default="legacy")
    ap.add_argument("--out", default=os.path.join(SDK, "out"))
    ap.add_argument("--check", action="store_true", help="validate the manifest and every profile; write nothing")
    ap.add_argument("--list-headers", metavar="PROFILES", help="comma list; print their public headers")
    ap.add_argument("--check-installed-headers", metavar="ROOT",
                    help="compile installed external examples and check exact public dependencies")
    args = ap.parse_args()
    try:
        m = load_manifest()
        if args.check_installed_headers:
            with tempfile.TemporaryDirectory(prefix="sdk-header-proof-") as work:
                proof = check_installed_example_headers(m, args.profile, args.check_installed_headers, work)
            print(f"{args.profile}: {proof['translation_units']} external C/C++ examples compiled; "
                  f"{len(proof['public_headers'])} exact installed public header dependencies approved")
            return
        if args.list_headers:
            print("\n".join(public_headers(m, args.list_headers.split(","))))
            return
        if args.check:
            for prof in m["profiles"]:
                print(f"{prof}: {len(select(m, prof))} components")
            print("sdk/components.json: ok")
            return
        build(m, args.profile, args.out)
    except ManifestError as e:
        sys.exit(f"package.py: {e}")


if __name__ == "__main__":
    main()
