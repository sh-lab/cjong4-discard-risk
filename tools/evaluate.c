#include <stdio.h>
#include <string.h>
#include "cjong4_discard_risk/evaluator.h"

static bool read_exact(const char *path, uint8_t *buffer, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return false;
    }
    bool ok = fread(buffer, 1, size, file) == size;
    if (ok)
        ok = fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "%s: expected exactly %zu bytes\n", path, size);
    return ok;
}

int main(int argc, char **argv)
{
    bool pgm = argc == 4 && strcmp(argv[1], "--pgm") == 0;
    if (argc != 3 && !pgm) {
        fprintf(stderr, "Usage: %s [--pgm] MODEL.i8 INPUT.rgb\n"
                        "Requires supplied model weights and a schema-v3 input.\n", argv[0]);
        return 2;
    }
    uint8_t bytes[CJ4DR_MODEL_SERIALIZED_SIZE], input[CJ4DR_INPUT_SIZE];
    uint8_t danger[CJ4DR_OUTPUT_SIZE];
    cj4dr_model model;
    cj4dr_workspace workspace;
    if (!read_exact(argv[pgm ? 2 : 1], bytes, sizeof(bytes)) ||
        !read_exact(argv[pgm ? 3 : 2], input, sizeof(input)))
        return 1;
    if (!cj4dr_model_decode(bytes, sizeof(bytes), &model)) {
        fputs("Invalid or incompatible model.\n", stderr);
        return 1;
    }
    if (!cj4dr_evaluate(&model, input, sizeof(input), &workspace, danger)) {
        fputs("Invalid input or model.\n", stderr);
        return 1;
    }
    if (pgm)
        printf("P2\n%u 1\n255\n", (unsigned)CJ4DR_OUTPUT_SIZE);
    for (unsigned t = 0; t < CJ4DR_OUTPUT_SIZE; ++t) {
        if (t != 0)
            putchar(' ');
        printf(pgm ? "%u" : "%02X", (unsigned)danger[t]);
    }
    putchar('\n');
    return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
