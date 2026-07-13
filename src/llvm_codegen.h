/* ------------------------------------------------------------------------- */
/*   "llvm_codegen.h" : interface between the lifter (llvm_codegen.c) and    */
/*   the lowerer (llvm_lower.c). Only these two modules include this file;   */
/*   everything the rest of the compiler needs is declared in header.h.      */
/* ------------------------------------------------------------------------- */

#ifndef LLVM_CODEGEN_H
#define LLVM_CODEGEN_H

#include <llvm-c/Core.h>

/* Lower the optimized IR function back to Glulx. On success, the capture
   buffer in asm.c has been rewritten with the lowered instruction stream
   (and the routine header's locals count patched if needed) and TRUE is
   returned; asm.c then replays the buffer through the classic encoder.
   On failure nothing has been touched and the caller falls back to
   replaying the original capture. */
extern int llvm_lower_routine(LLVMModuleRef mod, LLVMValueRef fn,
    const char **fail_reason);

#endif
