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

extern void llvm_direct_routine_begin(const char *name, int local_count,
    int embedded_flag, int stack_arguments)
{
    (void)name;
    (void)local_count;
    (void)embedded_flag;
    (void)stack_arguments;
}

extern void llvm_direct_routine_finish(int embedded_flag,
    int fallthrough_reachable)
{
    (void)embedded_flag;
    (void)fallthrough_reachable;
}

extern void llvm_direct_routine_abandon(void)
{
}

extern void llvm_direct_reject(const char *reason) { (void)reason; }
extern void llvm_direct_note_statement(int statement_code)
{ (void)statement_code; }
extern llvm_direct_value llvm_direct_constant(int32 value, int marker,
    int32 symindex)
{ (void)value; (void)marker; (void)symindex; return NULL; }
extern llvm_direct_value llvm_direct_load_local(int source)
{ (void)source; return NULL; }
extern llvm_direct_value llvm_direct_load_global(int source)
{ (void)source; return NULL; }
extern llvm_direct_value llvm_direct_unary(int operator_number,
    llvm_direct_value operand)
{ (void)operator_number; (void)operand; return NULL; }
extern llvm_direct_value llvm_direct_binary(int operator_number,
    llvm_direct_value left, llvm_direct_value right)
{ (void)operator_number; (void)left; (void)right; return NULL; }
extern llvm_direct_value llvm_direct_division(int operator_number,
    llvm_direct_value left, llvm_direct_value right, int check_zero)
{ (void)operator_number; (void)left; (void)right; (void)check_zero; return NULL; }
extern llvm_direct_value llvm_direct_compare(int operator_number,
    llvm_direct_value left, llvm_direct_value right)
{ (void)operator_number; (void)left; (void)right; return NULL; }
extern llvm_direct_value llvm_direct_store_local_value(int destination,
    llvm_direct_value value)
{ (void)destination; (void)value; return NULL; }
extern llvm_direct_value llvm_direct_store_global_value(int destination,
    llvm_direct_value value)
{ (void)destination; (void)value; return NULL; }
extern void llvm_direct_return_value(llvm_direct_value value) { (void)value; }
extern void llvm_direct_store_local_constant(int destination, int32 value)
{ (void)destination; (void)value; }
extern void llvm_direct_store_local(int destination, int source)
{ (void)destination; (void)source; }
extern void llvm_direct_return_constant(int32 value) { (void)value; }
extern void llvm_direct_return_local(int source) { (void)source; }
extern void llvm_direct_jump(int label) { (void)label; }
extern void llvm_direct_bind_label(int label) { (void)label; }

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
