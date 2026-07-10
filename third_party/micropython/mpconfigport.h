/* Goblin Core embed config for the UPYTHON.* scripting commands. */
#include <port/mpconfigport_common.h>

// Minimal base (no file I/O, no import-from-file, no host modules) -- the right
// starting point for a sandboxed scripting VM.
#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)

#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_PY_GC                           (1)
#define MICROPY_PY_SYS                          (0)

// Byte-based strings so keys/values are binary-safe (no UTF-8 validation).
#define MICROPY_PY_BUILTINS_STR_UNICODE         (0)
// Readable error messages for RESP error replies.
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_NORMAL)
// Floats (zset scores) and full-width integers.
#define MICROPY_PY_BUILTINS_FLOAT               (1)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)
