/* Hand-written replacement for Jim's autosetup-generated _load-static-exts.c.
 * The upstream file wires in every static extension the build selected; this
 * embedded subset ships only the two Tcl-level libraries the TCL.* commands
 * need (stdlib: lambda/alias/...; tclcompat: puts/lassign/...). No eventloop,
 * interp, oo, tree, history, or I/O extensions are compiled in. */
#include "jim.h"
#include "jimautoconf.h"

int Jim_InitStaticExtensions(Jim_Interp *interp)
{
    extern int Jim_stdlibInit(Jim_Interp *);
    extern int Jim_tclcompatInit(Jim_Interp *);
    Jim_stdlibInit(interp);
    Jim_tclcompatInit(interp);
    return JIM_OK;
}
