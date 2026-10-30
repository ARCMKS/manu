#include "../include/test.h"
#include <string.h>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : NULL;
    bool quiet = (argc > 2 && strcmp(argv[2], "-q") == 0);
    return test_run_all(filter, quiet);
}
