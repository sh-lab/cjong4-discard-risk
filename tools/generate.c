#include "teacher.h"
#include "cjong4/core/hand_analysis.h"
#include "cjong4/core/state_init.h"
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

typedef struct {
    uint64_t rng;
    unsigned epsilon;
    bool failed;
} policy;

typedef struct {
    uint64_t samples, safe, dangerous;
} statistics;

static volatile sig_atomic_t interrupted;
static void stop_generation(int signal_number) { interrupted = signal_number; }

static bool publish(const char *partial, const char *output)
{
    /* Publish a complete file without replacing an existing destination,
     * including a file created by another writer during generation. */
#if defined(_WIN32)
    return MoveFileA(partial, output) != 0;
#else
    if (link(partial, output) != 0) return false;
    if (remove(partial) != 0) perror("remove completed partial");
    return true;
#endif
}

static cj4_action decide(void *opaque, const cj4_player_view *view,
                         const cj4_action *actions, uint8_t count)
{
    policy *p = opaque;
    cj4_action invalid = {0};
    if (!count) { p->failed = true; return invalid; }
    for (unsigned i = 0; i < count; ++i)
        if (actions[i].type == CJ4_ACTION_RON || actions[i].type == CJ4_ACTION_TSUMO)
            return actions[i];
    if (cj4dr_uniform(&p->rng, 100) < p->epsilon)
        return actions[cj4dr_uniform(&p->rng, count)];
    for (unsigned i = 0; i < count; ++i)
        if (actions[i].type == CJ4_ACTION_RIICHI)
            return actions[i];
    /* Policy observes only the same masked player view, not the teacher's state. */
    cj4_mahjong visible;
    memset(&visible, 0, sizeof(visible));
    memcpy(visible.locations, view->locations, sizeof(visible.locations));
    unsigned candidates[CJ4M_MAX_ACTIONS], n = 0;
    int best = 128;
    for (unsigned i = 0; i < count; ++i) {
        if (actions[i].type != CJ4_ACTION_DISCARD)
            continue;
        cj4_shanten_result shanten;
        if (!cj4_calculate_shanten_after_discard(&visible, view->player,
                                                 actions[i].tile, &shanten)) {
            p->failed = true;
            return actions[i];
        }
        int value = shanten.standard;
        if (shanten.chiitoitsu < value) value = shanten.chiitoitsu;
        if (shanten.kokushi < value) value = shanten.kokushi;
        if (value < best) { best = value; n = 0; }
        if (value == best) candidates[n++] = i;
    }
    if (n)
        return actions[candidates[cj4dr_uniform(&p->rng, n)]];
    for (unsigned i = 0; i < count; ++i)
        if (actions[i].type == CJ4_ACTION_PASS)
            return actions[i];
    return actions[cj4dr_uniform(&p->rng, count)];
}

static bool write_sample(FILE *file, const cj4dr_teacher *sample, const char *group,
                          const char *source, uint64_t seed, uint64_t index,
                          unsigned step, unsigned player, unsigned epsilon,
                          int target_type, statistics *stats)
{
    if (fprintf(file, "{\"rgb_hex\":\"") < 0) return false;
    for (unsigned i = 0; i < CJ4DR_INPUT_SIZE; ++i)
        if (fprintf(file, "%02x", sample->rgb[i]) < 0) return false;
    if (fprintf(file, "\",\"target\":[") < 0) return false;
    for (unsigned t = 0; t < 34; ++t)
        if (fprintf(file, "%s%u", t ? "," : "", sample->target[t]) < 0) return false;
    if (fprintf(file, "],\"mask\":[") < 0) return false;
    for (unsigned t = 0; t < 34; ++t) {
        if (fprintf(file, "%s%u", t ? "," : "", sample->mask[t]) < 0) return false;
        if (sample->mask[t]) {
            if (sample->target[t]) ++stats->dangerous;
            else ++stats->safe;
        }
    }
    /* Strings are fixed program constants, never user-supplied JSON fragments. */
    if (fprintf(file, "],\"group\":\"%s\",\"metadata\":{\"source\":\"%s\","
                "\"generator_version\":1,\"input_schema\":3,\"rules\":\"cj4-default-v3\","
                "\"seed\":\"%" PRIu64 "\",\"index\":%" PRIu64 ",\"step\":%u,"
                "\"player\":%u,\"epsilon_percent\":%u,\"target_type\":%d}}\n",
                group, source, seed, index, step, player, epsilon, target_type) < 0)
        return false;
    ++stats->samples;
    return true;
}

static bool random_round(FILE *file, const cj4_rules *rules, uint64_t seed,
                          uint64_t round, unsigned epsilon, unsigned max_steps,
                          statistics *stats)
{
    /* Each round has an independent stream; --start-round supports sharding. */
    uint64_t rng = seed + round * UINT64_C(0x9e3779b97f4a7c15);
    rng = cj4dr_random(&rng);
    cj4_tile_id wall[136];
    cj4dr_shuffle(wall, &rng);
    cj4_mahjong state = cj4_create_initial_state(wall, rules);
    policy p = {rng, epsilon, false};
    cj4m_player_delegate delegates[4];
    for (unsigned i = 0; i < 4; ++i) {
        delegates[i].ctx = &p;
        delegates[i].decide = decide;
    }
    char group[128];
    snprintf(group, sizeof(group), "random-v1/seed-%" PRIu64 "/round-%" PRIu64, seed, round);
    for (unsigned step = 0; step < max_steps && !interrupted; ++step) {
        cj4_phase phase = cj4_state_phase(&state);
        if (phase == CJ4_PHASE_ROUND_END)
            return true;
        if (phase == CJ4_PHASE_DRAW || phase == CJ4_PHASE_AFTER_CALL) {
            cj4dr_teacher sample;
            if (!cj4dr_make_teacher(&state, rules, &sample) ||
                !write_sample(file, &sample, group, "random", seed, round, step,
                              cj4_state_current_player(&state), epsilon, -1, stats))
                break;
        }
        cj4_mahjong next = cj4m_step(&state, rules, delegates);
        if (p.failed || memcmp(&state, &next, sizeof(state)) == 0)
            break;
        state = next;
    }
    fprintf(stderr, "generation failed: seed=%" PRIu64 " round=%" PRIu64
            " phase=%u discards=%u (interruption, step limit, or invalid transition)\n",
            seed, round, (unsigned)cj4_state_phase(&state), state.discard_count);
    return false;
}

static bool cases(FILE *file, const cj4_rules *rules, uint64_t seed,
                  unsigned variants, statistics *stats)
{
    uint64_t rng = seed;
    for (unsigned type = 0; type < 34; ++type)
        for (unsigned variant = 0; variant < variants; ++variant)
            for (unsigned kind = 0; kind < CJ4DR_CASE_COUNT; ++kind) {
                if (interrupted) return false;
                uint64_t case_seed = cj4dr_random(&rng);
                cj4_mahjong state;
                cj4dr_teacher sample;
                if (!cj4dr_make_case((cj4dr_case)kind, (cj4_tile_type)type,
                                    case_seed, rules, &state) ||
                    !cj4dr_make_teacher(&state, rules, &sample)) {
                    fprintf(stderr, "case failed: type=%u variant=%u kind=%u seed=%" PRIu64 "\n",
                            type, variant, kind, case_seed);
                    return false;
                }
                /* Only the specified case target is a teacher for this sample. */
                for (unsigned t = 0; t < 34; ++t)
                    if (t != type) sample.target[t] = sample.mask[t] = 0;
                char group[64];
                /* Share group across seeds, variants AND related case families. */
                snprintf(group, sizeof(group), "manual-v1/type-%u", type);
                if (!write_sample(file, &sample, group, cj4dr_case_name((cj4dr_case)kind),
                                  case_seed, variant, 8, 0, 0, (int)type, stats))
                    return false;
            }
    return true;
}

static bool number(const char *text, uint64_t *value)
{
    if (!text[0]) return false;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno || *end || parsed > UINT64_MAX) return false;
    *value = (uint64_t)parsed;
    return true;
}

static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s --output PATH [--mode random|cases] [--seed N]\n"
            "  random: --rounds N --start-round N --epsilon-percent 0..100 --max-steps N\n"
            "  cases:  --variants N (102 samples per variant)\n", name);
}

int main(int argc, char **argv)
{
    const char *output = NULL, *mode = "random";
    uint64_t seed = 1, rounds = 10, start = 0, epsilon = 20, max_steps = 1024, variants = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (i + 1 == argc) { usage(argv[0]); return 2; }
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--output")) output = value;
        else if (!strcmp(key, "--mode")) mode = value;
        else {
            uint64_t n;
            if (!number(value, &n)) { usage(argv[0]); return 2; }
            if (!strcmp(key, "--seed")) seed = n;
            else if (!strcmp(key, "--rounds")) rounds = n;
            else if (!strcmp(key, "--start-round")) start = n;
            else if (!strcmp(key, "--epsilon-percent")) epsilon = n;
            else if (!strcmp(key, "--max-steps")) max_steps = n;
            else if (!strcmp(key, "--variants")) variants = n;
            else { usage(argv[0]); return 2; }
        }
    }
    if (!output || !output[0] || (strcmp(mode, "random") && strcmp(mode, "cases")) ||
        !rounds || rounds > UINT32_MAX || start > UINT64_MAX - rounds || epsilon > 100 ||
        !variants || variants > UINT32_MAX || !max_steps || max_steps > UINT32_MAX) {
        usage(argv[0]); return 2;
    }
    FILE *existing = fopen(output, "rb");
    if (existing) { fclose(existing); fprintf(stderr, "output already exists\n"); return 1; }
    size_t length = strlen(output) + sizeof(".partial");
    char *partial = malloc(length);
    if (!partial) return 1;
    snprintf(partial, length, "%s.partial", output);
    FILE *file = fopen(partial, "wbx");
    if (!file) { perror(partial); free(partial); return 1; }
    signal(SIGINT, stop_generation);
    signal(SIGTERM, stop_generation);
    cj4_rules rules = cj4_rules_default();
    statistics stats = {0};
    bool ok = true;
    if (!strcmp(mode, "cases"))
        ok = cases(file, &rules, seed, (unsigned)variants, &stats);
    else
        for (uint64_t i = 0; i < rounds && ok; ++i) {
            ok = random_round(file, &rules, seed, start + i, (unsigned)epsilon,
                              (unsigned)max_steps, &stats);
            if (ok) fprintf(stderr, "round=%" PRIu64 " completed (%" PRIu64 "/%" PRIu64 ")\n",
                            start + i, i + 1, rounds);
        }
    if (fclose(file) != 0) ok = false;
    if (interrupted) ok = false;
    if (ok && !publish(partial, output)) {
        fprintf(stderr, "unable to publish output (existing destination or filesystem error): %s\n", output);
        ok = false;
    }
    if (!ok) remove(partial);
    free(partial);
    if (!ok) return 1;
    fprintf(stderr, "saved %" PRIu64 " samples, safe_labels=%" PRIu64
            " dangerous_labels=%" PRIu64 " to %s\n", stats.samples, stats.safe, stats.dangerous, output);
    return 0;
}
