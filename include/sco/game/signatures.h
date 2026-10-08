#pragma once
// Game signature tables. Each feature area has its own table under src/game/; this registers
// them all. Feature code reads addresses with sco::Sig("<id>") or the typed accessors in the
// matching sco/game/*.h header, never with a pattern of its own.

namespace sco::game {

bool RegisterGameSignatures();   // idempotent; false on a duplicate id

}  // namespace sco::game
