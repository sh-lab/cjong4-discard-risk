#include <stdio.h>
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
    if (argc != 3) {
        fprintf(stderr, "Usage: %s MODEL.i8 INPUT.rgb\n"
                        "Requires supplied model weights and a schema-v2 input.\n", argv[0]);
        return 2;
    }
    uint8_t bytes[CJ4DR_MODEL_SERIALIZED_SIZE], input[CJ4DR_INPUT_SIZE], danger;
    cj4dr_model model;
    cj4dr_workspace workspace;
    if (!read_exact(argv[1], bytes, sizeof(bytes)) ||
        !read_exact(argv[2], input, sizeof(input)))
        return 1;
    if (!cj4dr_model_decode(bytes, sizeof(bytes), &model)) {
        fputs("Invalid or incompatible model.\n", stderr);
        return 1;
    }
    if (!cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger)) {
        fputs("Invalid input or model.\n", stderr);
        return 1;
    }
    printf("%u\n", (unsigned)danger);
    return 0;
}
