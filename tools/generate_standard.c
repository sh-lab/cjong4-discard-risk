/* Full standard(1) self-play. Keep input BEFORE the discard; mark actual ron
 * only after the discard-response step. Chankan is deliberately excluded. */
#include "staged_teacher.h"
#include "cjong4/opponent/opponent_standard.h"
#include "cjong4/core/state_init.h"
#include "cjong4/core/state_query.h"
#include "cjong4/core/state_round.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t interrupted;
static void stop(int number) { interrupted = number; }

typedef struct { uint64_t samples, ron, safe, dangerous, ff; } statistics;

static bool write_sample(FILE *file, const cj4dr_teacher *sample,
                          uint64_t seed, uint64_t game, unsigned round,
                          unsigned step, unsigned player, int discarded,
                          bool actual_ron, uint8_t winner_mask, statistics *stats)
{
    if (actual_ron && (discarded < 0 || !sample->mask[discarded / 4] ||
                      sample->target[discarded / 4] != 255)) {
        fputs("actual ron contradicts pre-discard teacher\n", stderr);
        return false;
    }
    if (fprintf(file, "{\"rgb_hex\":\"") < 0) return false;
    for (unsigned i = 0; i < 408; ++i)
        if (fprintf(file, "%02x", sample->rgb[i]) < 0) return false;
    if (fprintf(file, "\",\"target\":[") < 0) return false;
    for (unsigned t = 0; t < 34; ++t)
        if (fprintf(file, "%s%u", t ? "," : "", sample->target[t]) < 0) return false;
    if (fprintf(file, "],\"mask\":[") < 0) return false;
    for (unsigned t = 0; t < 34; ++t) {
        if (fprintf(file, "%s%u", t ? "," : "", sample->mask[t]) < 0) return false;
        if (sample->mask[t]) {
            if (sample->target[t] == 255) ++stats->ff;
            if (sample->target[t]) ++stats->dangerous;
            else ++stats->safe;
        }
    }
    if (fprintf(file, "],\"group\":\"standard-v1/seed-%" PRIu64 "/game-%" PRIu64 "\","
                "\"metadata\":{\"source\":\"standard-selfplay\",\"generator_version\":2,\"teacher_policy\":\"staged-v1\","
                "\"input_schema\":3,\"rules\":\"cj4-default-v3\",\"opponent\":\"standard(1)\","
                "\"seed\":\"%" PRIu64 "\",\"game\":%" PRIu64 ",\"round\":%u,"
                "\"step\":%u,\"player\":%u,\"discarded_tile\":%d,\"actual_ron\":%s,"
                "\"winner_mask\":%u}}\n", seed, game, seed, game, round, step, player,
                discarded, actual_ron ? "true" : "false", winner_mask) < 0) return false;
    ++stats->samples;
    stats->ron += actual_ron;
    return true;
}

static bool generate_game(FILE *file, uint64_t seed, uint64_t game,
                           unsigned max_steps, statistics *stats)
{
    uint64_t rng = seed + game * UINT64_C(0x9e3779b97f4a7c15);
    rng = cj4dr_random(&rng);
    cj4_tile_id wall[136];
    cj4dr_shuffle(wall, &rng);
    cj4_rules rules = cj4_rules_default();
    cj4_mahjong state = cj4_create_initial_state(wall, &rules);
    cj4m_player_delegate delegates[4];
    for (unsigned p = 0; p < 4; ++p) delegates[p] = cj4_opponent_standard(1);
    cj4dr_teacher pending;
    cj4dr_safety safety = {0};
    bool have_pending = false;
    unsigned pending_step = 0, pending_player = 0, round = 0, step = 0;
    int discarded = -1;
    for (; step < max_steps && !interrupted; ++step) {
        cj4_phase phase = cj4_state_phase(&state);
        if (phase == CJ4_PHASE_GAME_END) return !have_pending;
        if (phase == CJ4_PHASE_DRAW || phase == CJ4_PHASE_AFTER_CALL) {
            if (have_pending || !cj4dr_make_staged_teacher(&state, &rules, &safety, &pending)) break;
            pending_step = step;
            pending_player = cj4_state_current_player(&state);
            discarded = -1;
            have_pending = true;
        }
        cj4_mahjong next;
        if (phase == CJ4_PHASE_SETTLE && !cj4_can_game_end(state)) {
            if (!cj4_can_next_round(state)) break;
            cj4dr_shuffle(wall, &rng);
            next = cj4_do_next_round(state, wall, &rules);
            ++round;
        } else {
            next = cj4m_step(&state, &rules, delegates);
        }
        if (memcmp(&state, &next, sizeof(state)) == 0) break;
        if (have_pending) {
            if (cj4_state_phase(&next) == CJ4_PHASE_DISCARD) {
                discarded = cj4_get_last_discard_tile(&next);
            } else {
                bool ron = phase == CJ4_PHASE_DISCARD &&
                    cj4_state_phase(&next) == CJ4_PHASE_ROUND_END &&
                    cj4_state_round_end_type(&next) == CJ4_ROUND_END_RON;
                if (ron && !cj4dr_mark_actual_ron(&pending, discarded)) break;
                if (!write_sample(file, &pending, seed, game, round, pending_step,
                                  pending_player, discarded, ron,
                                  ron ? next.winner_mask : 0, stats)) break;
                have_pending = false;
            }
        }
        cj4dr_safety_observe(&safety, &state, &next);
        state = next;
    }
    fprintf(stderr, "generation failed: seed=%" PRIu64 " game=%" PRIu64
            " round=%u step=%u phase=%u discards=%u\n", seed, game, round, step,
            (unsigned)cj4_state_phase(&state), state.discard_count);
    return false;
}

static bool number(const char *text, uint64_t *value)
{
    if (!text[0]) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno || *end || parsed > UINT64_MAX) return false;
    *value = (uint64_t)parsed;
    return true;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s --output FILE [--games N] [--seed N] "
            "[--start-game N] [--max-steps N]\n", program);
}

int main(int argc, char **argv)
{
    uint64_t games = 10, start = 0, seed = 1, max_steps = 10000;
    const char *output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (i + 1 == argc) { usage(argv[0]); return 2; }
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--output")) output = value;
        else {
            uint64_t n;
            if (!number(value, &n)) { usage(argv[0]); return 2; }
            if (!strcmp(key, "--games")) games = n;
            else if (!strcmp(key, "--start-game")) start = n;
            else if (!strcmp(key, "--seed")) seed = n;
            else if (!strcmp(key, "--max-steps")) max_steps = n;
            else { usage(argv[0]); return 2; }
        }
    }
    if (!output || !*output || !games || games > UINT32_MAX || start > UINT64_MAX - games ||
        !max_steps || max_steps > UINT32_MAX) { usage(argv[0]); return 2; }
    FILE *existing = fopen(output, "rb");
    if (existing) { fclose(existing); fputs("output already exists\n", stderr); return 1; }
    char *partial = malloc(strlen(output) + sizeof(".partial"));
    if (!partial) return 1;
    sprintf(partial, "%s.partial", output);
    FILE *file = fopen(partial, "wbx");
    if (!file) { perror(partial); free(partial); return 1; }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    statistics stats = {0};
    bool ok = true;
    for (uint64_t i = 0; i < games && ok; ++i) {
        ok = generate_game(file, seed, start + i, (unsigned)max_steps, &stats);
        if (ok) fprintf(stderr, "game=%" PRIu64 " (%" PRIu64 "/%" PRIu64
                        ") samples=%" PRIu64 " actual_ron=%" PRIu64 "\n",
                        start + i, i + 1, games, stats.samples, stats.ron);
    }
    if (fclose(file) != 0 || interrupted) ok = false;
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileA(partial, output) != 0;
#else
        ok = link(partial, output) == 0;
#endif
        if (!ok) fprintf(stderr, "unable to publish %s\n", output);
    }
    remove(partial);
    free(partial);
    if (!ok) return 1;
    fprintf(stderr, "saved samples=%" PRIu64 " actual_ron=%" PRIu64
            " safe_labels=%" PRIu64 " nonzero_labels=%" PRIu64 " ff_labels=%" PRIu64 "\n",
            stats.samples, stats.ron, stats.safe, stats.dangerous, stats.ff);
    return 0;
}
