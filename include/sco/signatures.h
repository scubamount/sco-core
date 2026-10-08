#pragma once
// The signature registry: every game address the mod uses is one named row.
//
// A row says how to find its address (a unique byte pattern, or a resolver function for the
// cases a pattern can't express, such as a string reference followed by layout checks), and
// which other rows it needs. ResolveAll() resolves rows in dependency order and records, per
// row, whether it was found. A row whose dependency failed is Blocked, not tried.
//
// Rows are plain data so the same table runs in game and in tools/sco-sigcheck against a
// StarCitizen.exe read from disk.
#include "sco/scan.h"
#include <cstddef>
#include <cstdint>

namespace sco {

enum class SigState : uint8_t { NotRun, Ok, Missing, Ambiguous, Failed, Blocked };
const char* SigStateName(SigState s);

struct SigResult {
    SigState    state   = SigState::NotRun;
    uint8_t*    at      = nullptr;
    int         matches = 0;
    const char* why     = nullptr;   // static string; set for Failed / Blocked
};

// Resolver for rows a single pattern can't express. Reads other rows with Sig(); must not throw.
using SigResolver = SigResult (*)(const Image& img);

constexpr int kMaxSigNeeds = 4;

struct SigDef {
    const char* id;                        // "teleport.to_camera": <feature>.<thing>
    const char* pattern;                   // unique .text pattern, or nullptr when `resolve` is set
    int         ripDisp;                   // pattern rows: >0 = result is the RIP target of the operand at
    int         ripSize;                   //   match+ripDisp in an instruction of ripSize bytes; 0 = the match itself
    SigResolver resolve;                   // resolver rows
    const char* needs[kMaxSigNeeds];       // ids this row reads; nullptr-terminated
};

// Results helpers for resolvers.
inline SigResult SigOk(const void* at)            { return { SigState::Ok, const_cast<uint8_t*>(static_cast<const uint8_t*>(at)), 1, nullptr }; }
inline SigResult SigFail(const char* why)         { return { SigState::Failed, nullptr, 0, why }; }
SigResult        SigPattern(const Section& text, const char* pattern);   // Ok / Missing / Ambiguous

// Tables are registered before ResolveAll(); ids must be unique across all tables.
// Returns false (and logs) on a duplicate id or a table that's too big.
bool RegisterSignatures(const SigDef* rows, size_t n);

// Resolves every registered row. Unknown or circular needs make the row Failed. Safe to call
// once per image; calling it again re-resolves everything.
void ResolveAll(const Image& img);

uint8_t*         Sig(const char* id);                 // address, or nullptr unless Ok
bool             SigReady(const char* id);
const SigResult* SigLookup(const char* id);           // nullptr for an unknown id

size_t        SignatureCount();
const SigDef* SignatureDef(size_t i);
const SigResult& SignatureResult(size_t i);

// "[core] signatures: 61/64 OK" plus one line per row that isn't Ok.
// verbose = also list the Ok rows.
void LogSignatureReport(bool verbose);

}  // namespace sco
