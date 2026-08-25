#define COMB_TEST
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../comb.c"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(path, sizeof path, "%s", argv[1]);
    use_stdin = 0;
    load_all();
    return nlines ? 0 : 1;
}
