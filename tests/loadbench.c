#define COMB_TEST
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../src/base.c"
#include "../src/state.c"
#include "../src/load.c"
#include "../src/match.c"
#include "../src/jobs.c"
#include "../src/render.c"
#include "../src/clip.c"
#include "../src/input.c"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(path, sizeof path, "%s", argv[1]);
    use_stdin = 0;
    load_all();
    return nlines ? 0 : 1;
}
