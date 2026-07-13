/* ------------------------------------------------------------------------- */
/*   "llvm_stub" : No-LLVM replacement for llvm_codegen.c + llvm_lower.c     */
/*                                                                           */
/*   Part of inform6-llvm. Not part of the upstream Inform 6 compiler.       */
/*                                                                           */
/*   Built instead of the real pipeline when LLVM development files are      */
/*   not available (the Makefile detects this via llvm-config; the Visual    */
/*   Studio project always uses the stub). Declining every routine makes     */
/*   asm.c replay the capture buffer through the classic encoder, so the     */
/*   output is byte-identical to a classic compile at every $LLVM level:     */
/*   the compiler is fully functional, just never optimizing.                */
/* ------------------------------------------------------------------------- */

#include "header.h"

static int any_routine_captured;

extern int llvm_pipeline_routine(void)
{
    any_routine_captured = TRUE;
    return FALSE;
}

extern void llvm_codegen_free(void)
{
    /* $LLVM=1 is capture/replay by design, so only levels that promise
       optimization deserve a note. Printed where the real pipeline prints
       its statistics line. */
    if (LLVM_CODEGEN >= 2 && any_routine_captured) {
        printf("LLVM: this compiler was built without LLVM support; "
            "all routines were compiled classically\n");
    }
    any_routine_captured = FALSE;
}
