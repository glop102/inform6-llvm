/* ------------------------------------------------------------------------- */
/*   "llvm_codegen.h" : interface between direct codegen and the lowerer     */
/*   the lowerer (llvm_lower.c). Only these two modules include this file;   */
/*   everything the rest of the compiler needs is declared in header.h.      */
/* ------------------------------------------------------------------------- */

#ifndef LLVM_CODEGEN_H
#define LLVM_CODEGEN_H

#include <llvm-c/Core.h>

/* Lower the optimized IR function back to Glulx. On success, the output
   buffer in asm.c holds the lowered instruction stream, insts_in/out hold
   its static before/after counts, and TRUE is returned. */
extern int llvm_lower_routine(LLVMModuleRef mod, LLVMValueRef fn,
    const char **fail_reason, int *insts_in, int *insts_out);

/* Static instruction counts over all successfully lowered routines. */
extern int llvm_lower_insts_in, llvm_lower_insts_out;

/* TRUE if the named Glulx opcode never writes RAM, globals, or locals
   and never calls back into VM code (so it cannot clobber anything the
   lowerer's memory analyses track). Direct codegen grades the same opcodes'
   IR declarations with matching attributes. */
extern int llvm_opcode_no_ram_write(const char *opname);

#endif
