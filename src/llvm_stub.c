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
extern int llvm_direct_can_generate(void) { return FALSE; }
extern void llvm_direct_suspend(void) { }
extern void llvm_direct_resume(void) { }
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
extern llvm_direct_value llvm_direct_call(llvm_direct_value function,
    llvm_direct_value *arguments, int count)
{ (void)function; (void)arguments; (void)count; return NULL; }
extern llvm_direct_value llvm_direct_glulx_op(const char *opcode,
    llvm_direct_value *arguments, int count)
{ (void)opcode; (void)arguments; (void)count; return NULL; }
extern llvm_direct_value llvm_direct_quantity(assembly_operand AO)
{ (void)AO; return NULL; }
extern llvm_direct_value llvm_direct_store_local_value(int destination,
    llvm_direct_value value)
{ (void)destination; (void)value; return NULL; }
extern llvm_direct_value llvm_direct_store_global_value(int destination,
    llvm_direct_value value)
{ (void)destination; (void)value; return NULL; }
extern void llvm_direct_adjust_variable(int variable, int amount)
{ (void)variable; (void)amount; }
extern void llvm_direct_return_value(llvm_direct_value value) { (void)value; }
extern void llvm_direct_store_local_constant(int destination, int32 value)
{ (void)destination; (void)value; }
extern void llvm_direct_store_local(int destination, int source)
{ (void)destination; (void)source; }
extern void llvm_direct_return_constant(int32 value) { (void)value; }
extern void llvm_direct_return_local(int source) { (void)source; }
extern void llvm_direct_jump(int label) { (void)label; }
extern void llvm_direct_bind_label(int label) { (void)label; }
extern void llvm_direct_resolve_label(int label, int used)
{ (void)label; (void)used; }
extern llvm_direct_block llvm_direct_new_block(void) { return NULL; }
extern llvm_direct_block llvm_direct_source_block(int label)
{ (void)label; return NULL; }
extern void llvm_direct_bind_block(llvm_direct_block block) { (void)block; }
extern void llvm_direct_jump_block(llvm_direct_block block) { (void)block; }
extern void llvm_direct_branch(llvm_direct_value condition,
    llvm_direct_block true_block, llvm_direct_block false_block)
{ (void)condition; (void)true_block; (void)false_block; }
extern llvm_direct_block llvm_direct_current_block(void) { return NULL; }
extern llvm_direct_value llvm_direct_phi(llvm_direct_value first,
    llvm_direct_block first_block, llvm_direct_value second,
    llvm_direct_block second_block)
{
    (void)first; (void)first_block; (void)second; (void)second_block;
    return NULL;
}
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
