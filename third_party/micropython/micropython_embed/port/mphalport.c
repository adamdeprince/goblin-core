/* Goblin Core: route MicroPython stdout (print) to the server log (stderr)
 * rather than the process's stdout. Replaces the embed port's stub, whose only
 * content was an stdout printf. */
#include <stdio.h>
#include "py/mphal.h"

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    fprintf(stderr, "%.*s", (int)len, str);
}
