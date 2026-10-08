/* Pure-C compilation smoke test for the pdftoolkit C-ABI.
 *
 * Proves pdftoolkit.h is a valid C header and the exported symbols are
 * consumable from a plain C translation unit compiled with the same
 * strict warning profile (-Wall -Wextra -Wpedantic -Wconversion).
 * This is the Phase-0 direction of audit task 7.1's acceptance criterion
 * ("C integration test compiles with gcc -Wall -Wextra -Werror").
 */

#include <stdio.h>

#include "pdftoolkit/pdftoolkit.h"

int main(void) {
    char version[64];
    EngineHandle* engine = NULL;
    uint32_t doc_id = 0;

    if (pdftoolkit_version(version, (uint32_t)sizeof(version)) != PDTK_OK) {
        return 1;
    }
    if (pdftoolkit_engine_create(&engine) != PDTK_OK || engine == NULL) {
        return 2;
    }
    /* register_document must reject a missing file, not crash. */
    if (pdftoolkit_register_document(engine, "/nonexistent/c-smoke.pdf", &doc_id) !=
        PDTK_ERR_DOCUMENT_NOT_FOUND) {
        return 3;
    }
    if (pdftoolkit_search_wand(engine, "q", 1u, 100u, NULL) != PDTK_ERR_INVALID_ARGUMENT) {
        return 4;
    }
    if (pdftoolkit_engine_destroy(engine) != PDTK_OK) {
        return 5;
    }

    printf("c-abi smoke ok, native core %s\n", version);
    return 0;
}
