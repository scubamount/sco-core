/* SPDX-License-Identifier: GPL-3.0-only */
/* C direct-table recipe: synthetic inspector and caller-position NPC primitives.
 * No game provider/streaming/unload semantics are certified by these responses. */
#include "sc_actors.h"
#include "sc_entities.h"
#include "sc_spatial.h"
#include "sc_game_events.h"
#include <stddef.h>
#include <stdio.h>

#define MEMBER(t, type, member) ((t) != NULL && (t)->size >= offsetof(type, member) + sizeof((t)->member) && (t)->member != NULL)
static int passed, failed;
#define CHECK(x) do { if (x) ++passed; else { ++failed; printf("FAIL C line %d: %s\n", __LINE__, #x); } } while (0)

static sco_result Inspect(const sc_actors_v1* a, const sc_entities_v1* e) {
    uint64_t actor = 0, entity = 0, zone = 0;
    float hp = 0, maximum = 0;
    double pos[3] = {0}, rot[4] = {0};
    if (!MEMBER(a, sc_actors_v1, local_player) || !MEMBER(a, sc_actors_v1, health) ||
        !MEMBER(e, sc_entities_v1, get_transform)) return SCO_UNAVAILABLE;
    sco_result r = a->local_player(&actor, &entity);
    if (r != SCO_OK) return r; /* zero IDs are not readings after failure */
    r = a->health(entity, &hp, &maximum);
    if (r != SCO_OK) return r; /* maximum == 0 means unknown, not zero capacity */
    r = e->get_transform(entity, pos, rot, &zone);
    if (r != SCO_OK) return r;
    return actor == 5 && entity == 6 && hp == 12 && maximum == 0 && zone == 9 && rot[3] == 1 ? SCO_OK : SCO_FAILED;
}
static sco_result Local(uint64_t* a, uint64_t* e) { *a = 5; *e = 6; return SCO_OK; }
static sco_result Health(uint64_t id, float* hp, float* maximum) {
    if (id != 6) return SCO_NOT_FOUND;
    *hp = 12; *maximum = 0; return SCO_OK;
}
static sco_result Transform(uint64_t id, double* p, double* q, uint64_t* z) {
    if (id != 6) return SCO_NOT_FOUND;
    p[0] = 1; p[1] = 2; p[2] = 3; q[3] = 1; *z = 9; return SCO_OK;
}
static sco_plugin* owner;
static sco_result Spawn(sco_plugin* self, const char* cls, uint64_t zone, const double* pos, uint64_t* id) {
    (void)cls;
    if (self != owner || zone != 9 || pos[2] != 3) return SCO_BAD_ARG;
    *id = 77; return SCO_OK;
}
static sco_result Remove(sco_plugin* self, uint64_t id) { return self == owner && id == 77 ? SCO_OK : SCO_NOT_FOUND; }
int main(void) {
    sc_actors_v1 a = {0}; sc_entities_v1 e = {0};
    a.size = sizeof(a); a.local_player = Local; a.health = Health; a.spawn_npc = Spawn; a.despawn = Remove;
    e.size = sizeof(e); e.get_transform = Transform;
    CHECK(Inspect(&a, &e) == SCO_OK);
    CHECK(Inspect(NULL, &e) == SCO_UNAVAILABLE);
    CHECK(Inspect(&a, NULL) == SCO_UNAVAILABLE);
    for (uint32_t n = offsetof(sc_actors_v1, health); n < offsetof(sc_actors_v1, health) + sizeof(a.health); ++n) {
        a.size = n; CHECK(Inspect(&a, &e) == SCO_UNAVAILABLE);
    }
    a.size = sizeof(a); a.health = NULL;
    CHECK(Inspect(&a, &e) == SCO_UNAVAILABLE);
    a.health = Health; e.get_transform = NULL;
    CHECK(Inspect(&a, &e) == SCO_UNAVAILABLE);
    const double pos[3] = {1, 2, 3}; uint64_t id = 0;
    owner = (sco_plugin*)&a; /* opaque identity only */
    CHECK(MEMBER(&a, sc_actors_v1, spawn_npc) && a.spawn_npc(owner, "SyntheticNpc", 9, pos, &id) == SCO_OK && id == 77);
    CHECK(a.despawn(NULL, id) == SCO_NOT_FOUND);
    CHECK(a.despawn(owner, id) == SCO_OK); /* request started, not proof of disappearance */
    printf("C language routes: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
