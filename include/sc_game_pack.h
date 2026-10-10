/* sc_game_pack.h: the Star Citizen game pack's version and the game builds it was verified on.
 * The game pack (sco-core's SCO_GAME_SC: signature rows, the game services under "game", DataCore)
 * has its own version line, separate from the kernel's sco_api 1.x and its sdk-vX.Y tags
 * (docs/design/game-services.md decision 2). Releases are tagged game-sc-vX.Y. A game patch that
 * moves rows gets a game-pack patch release; a game service's own table version changes only when
 * that table changes. docs/game-pack.md lists every verified build with its sigcheck report. */
#ifndef SC_GAME_PACK_H
#define SC_GAME_PACK_H

#define SCO_GAME_PACK_VERSION "0.1.0"

/* The newest game build every row was checked on with sco-sigcheck (all rows OK). */
#define SCO_GAME_PACK_VERIFIED_BUILD "4.10.196.36804"

#endif
