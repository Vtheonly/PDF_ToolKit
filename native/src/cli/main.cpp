// pdftoolkit-cli — native headless CLI (Phase 0 scaffolding).
//
// Only --version is functional. The real subcommands (search, corpus-scan,
// slice, merge, inspect — audit task 7.3) arrive with the native core and
// will emit Arrow IPC / JSON-lines streams on stdout.

#include <cstdio>
#include <cstring>

#include "pdftoolkit/pdftoolkit.h"

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "pdftoolkit-cli - native headless PDF engine CLI (scaffolding)\n"
                 "\n"
                 "Usage:\n"
                 "  pdftoolkit-cli --version        Print the native core version\n"
                 "\n"
                 "Planned subcommands (issue #1, task 7.3):\n"
                 "  search, corpus-scan, slice, merge, inspect\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) {
        char version[64];
        const int32_t status =
            pdftoolkit_version(version, static_cast<uint32_t>(sizeof(version)));
        if (status != PDTK_OK) {
            std::fprintf(stderr, "error: pdftoolkit_version() failed with code %d\n",
                         static_cast<int>(status));
            return 1;
        }
        std::printf("pdftoolkit-cli %s (native core)\n", version);
        return 0;
    }
    if (argc == 1) {
        print_usage(stdout);
        return 0;
    }
    print_usage(stderr);
    return 2;
}
