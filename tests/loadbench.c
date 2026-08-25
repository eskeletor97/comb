#define COMB_TEST
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../base.c"
#include "../state.c"
#include "../load.c"
#include "../match.c"
#include "../jobs.c"
#include "../render.c"
#include "../clip.c"
#include "../input.c"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(path, sizeof path, "%s", argv[1]);
    use_stdin = 0;
    load_all();
    return nlines ? 0 : 1;
}
