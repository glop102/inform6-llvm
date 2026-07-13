/* ------------------------------------------------------------------------- */
/*   "llvm_codegen" : Lifting captured Glulx routines to LLVM IR             */
/*                                                                           */
/*   Part of inform6-llvm. Not part of the upstream Inform 6 compiler.       */
/*                                                                           */
/*   The capture seam in asm.c hands us each routine as a list of            */
/*   llvm_event records (instructions and label definitions). This module    */
/*   lifts that stream into an LLVM IR function:                             */
/*                                                                           */
/*     - every local variable becomes an i32 alloca (function parameters     */
/*       are stored into them on entry; mem2reg cleans this up later),       */
/*     - labels become basic blocks; Glulx conditional branches become       */
/*       icmp + br pairs,                                                    */
/*     - stack-mode (sp) operands are tracked as a symbolic value stack,     */
/*     - global variables become external i32 globals,                       */
/*     - operands carrying backpatch markers become calls to the symbolic    */
/*       constant function @i6.sym(marker, value),                           */
/*     - everything without a direct IR meaning (calls, glk, memory and      */
/*       stream opcodes...) becomes a call to an opaque external function    */
/*       @i6.<opcode>, whose side effects pin its ordering.                  */
/*                                                                           */
/*   Any construct the lifter doesn't handle makes the routine "bail":       */
/*   the IR is discarded and asm.c replays the capture buffer through the    */
/*   classic encoder instead. The compiler is never wrong, only sometimes    */
/*   unoptimized.                                                            */
/*                                                                           */
/*   Milestone M2: lift + verify + optional dump ($LLVM=2). The lowering     */
/*   path does not exist yet, so llvm_pipeline_routine() always returns      */
/*   FALSE and every routine is still emitted by the classic encoder.        */
/* ------------------------------------------------------------------------- */

#include "header.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

/* ------------------------------------------------------------------------- */
/*   Lift state                                                              */
/* ------------------------------------------------------------------------- */

#define MAX_SYMBOLIC_STACK 64

static LLVMContextRef llctx;        /* created lazily, lives all compile     */
static FILE *dump_file;             /* $LLVM=2 dump stream, opened lazily    */

static int no_routines_lifted;      /* statistics                            */
static int no_routines_bailed;

/* Per-routine state. */
static LLVMModuleRef  mod;
static LLVMBuilderRef bld;
static LLVMValueRef   fn;
static LLVMTypeRef    i32t;

static LLVMValueRef  *local_slots;  /* allocas, indexed 1..n_locals          */
static int            n_locals;

static LLVMBasicBlockRef *label_blocks; /* indexed by label number           */
static int            n_label_blocks;

static LLVMBasicBlockRef retblock[2];   /* lazily built "ret 0" / "ret 1"    */

static LLVMValueRef   symstack[MAX_SYMBOLIC_STACK];
static int            symstack_depth;

static int            lift_failed;
static const char    *bail_reason;

static void bail(const char *why)
{   if (!lift_failed) {
        lift_failed = TRUE;
        bail_reason = why;
    }
}

/* ------------------------------------------------------------------------- */
/*   Helpers for names, declarations, and operand access                     */
/* ------------------------------------------------------------------------- */

static LLVMValueRef declare_fn(const char *name, LLVMTypeRef ret,
    LLVMTypeRef *params, unsigned nparams, LLVMTypeRef *type_out)
{
    LLVMValueRef f = LLVMGetNamedFunction(mod, name);
    LLVMTypeRef ft = LLVMFunctionType(ret, params, nparams, 0);
    if (!f)
        f = LLVMAddFunction(mod, name, ft);
    if (type_out) *type_out = ft;
    return f;
}

/* The symbolic-constant function: an operand whose value will be fixed up
   by the backpatcher (a routine address, string address, etc.) must survive
   optimization as an opaque token, not fold as an integer. */
static LLVMValueRef symbolic_constant(int marker, int32 value, int symindex)
{
    LLVMTypeRef params[3];
    LLVMTypeRef ft;
    LLVMValueRef f, args[3];
    params[0] = i32t; params[1] = i32t; params[2] = i32t;
    f = declare_fn("i6.sym", i32t, params, 3, &ft);
    args[0] = LLVMConstInt(i32t, (unsigned)marker, 0);
    args[1] = LLVMConstInt(i32t, (uint32_t)value, 0);
    args[2] = LLVMConstInt(i32t, (uint32_t)symindex, 0);
    return LLVMBuildCall2(bld, ft, f, args, 3, "sym");
}

static LLVMValueRef global_var(int32 index)
{
    char name[32];
    LLVMValueRef g;
    sprintf(name, "i6.g%d", (int)index);
    g = LLVMGetNamedGlobal(mod, name);
    if (!g)
        g = LLVMAddGlobal(mod, i32t, name);
    return g;
}

static LLVMValueRef sympop(void)
{
    if (symstack_depth <= 0) {
        bail("read from sp with empty symbolic stack");
        return LLVMConstInt(i32t, 0, 0);
    }
    return symstack[--symstack_depth];
}

static void sympush(LLVMValueRef v)
{
    if (symstack_depth >= MAX_SYMBOLIC_STACK) {
        bail("symbolic stack overflow");
        return;
    }
    symstack[symstack_depth++] = v;
}

/* Evaluate a source operand to an i32 value. Stack-mode operands pop the
   symbolic stack; operands are evaluated in Glulx decode order (left to
   right), which matches the VM's pop order. */
static LLVMValueRef load_operand(const assembly_operand *op)
{
    switch (op->type) {
    case CONSTANT_OT:
    case HALFCONSTANT_OT:
    case BYTECONSTANT_OT:
        if (op->marker)
            return symbolic_constant(op->marker, op->value, op->symindex);
        return LLVMConstInt(i32t, (uint32_t)op->value, 0);
    case ZEROCONSTANT_OT:
        return LLVMConstInt(i32t, 0, 0);
    case LOCALVAR_OT:
        if (op->value == 0)
            return sympop();
        if (op->value < 1 || op->value > n_locals) {
            bail("local variable index out of range");
            return LLVMConstInt(i32t, 0, 0);
        }
        return LLVMBuildLoad2(bld, i32t, local_slots[op->value], "");
    case GLOBALVAR_OT:
        return LLVMBuildLoad2(bld, i32t,
            global_var(op->value - MAX_LOCAL_VARIABLES), "");
    case DEREFERENCE_OT:
        {   LLVMTypeRef params[1]; LLVMTypeRef ft; LLVMValueRef f, addr;
            params[0] = i32t;
            f = declare_fn("i6.deref", i32t, params, 1, &ft);
            if (op->marker)
                addr = symbolic_constant(op->marker, op->value, op->symindex);
            else
                addr = LLVMConstInt(i32t, (uint32_t)op->value, 0);
            return LLVMBuildCall2(bld, ft, f, &addr, 1, "");
        }
    default:
        bail("unhandled source operand type");
        return LLVMConstInt(i32t, 0, 0);
    }
}

/* Store an i32 value into a result operand. */
static void store_operand(const assembly_operand *op, LLVMValueRef v)
{
    switch (op->type) {
    case ZEROCONSTANT_OT:
        /* store to "zero" means discard */
        return;
    case LOCALVAR_OT:
        if (op->value == 0) {
            sympush(v);
            return;
        }
        if (op->value < 1 || op->value > n_locals) {
            bail("local variable index out of range (store)");
            return;
        }
        LLVMBuildStore(bld, v, local_slots[op->value]);
        return;
    case GLOBALVAR_OT:
        LLVMBuildStore(bld, v, global_var(op->value - MAX_LOCAL_VARIABLES));
        return;
    case DEREFERENCE_OT:
        {   LLVMTypeRef params[2]; LLVMTypeRef ft; LLVMValueRef f, args[2];
            params[0] = i32t; params[1] = i32t;
            f = declare_fn("i6.deref.store", LLVMVoidTypeInContext(llctx),
                params, 2, &ft);
            if (op->marker)
                args[0] = symbolic_constant(op->marker, op->value, op->symindex);
            else
                args[0] = LLVMConstInt(i32t, (uint32_t)op->value, 0);
            args[1] = v;
            LLVMBuildCall2(bld, ft, f, args, 2, "");
        }
        return;
    default:
        bail("unhandled store operand type");
        return;
    }
}

/* ------------------------------------------------------------------------- */
/*   Basic-block management                                                  */
/* ------------------------------------------------------------------------- */

static LLVMBasicBlockRef block_for_label(int n)
{
    char name[32];
    if (n < 0 || n >= n_label_blocks) {
        bail("branch to out-of-range label");
        return NULL;
    }
    if (!label_blocks[n]) {
        sprintf(name, "L%d", n);
        label_blocks[n] = LLVMAppendBasicBlockInContext(llctx, fn, name);
    }
    return label_blocks[n];
}

/* Target block for a branch operand: a label number, or the special
   values -3 ("branch means return 0") / -4 ("branch means return 1"). */
static LLVMBasicBlockRef block_for_branch_target(int32 target)
{
    if (target == -3 || target == -4) {
        int which = (target == -4);
        if (!retblock[which]) {
            LLVMBasicBlockRef saved = LLVMGetInsertBlock(bld);
            retblock[which] = LLVMAppendBasicBlockInContext(llctx, fn,
                which ? "ret1" : "ret0");
            LLVMPositionBuilderAtEnd(bld, retblock[which]);
            LLVMBuildRet(bld, LLVMConstInt(i32t, (unsigned)which, 0));
            LLVMPositionBuilderAtEnd(bld, saved);
        }
        return retblock[which];
    }
    if (target == -2) {
        /* "branch no-op": branches to the next instruction. The compiler
           only emits this in odd corner cases; not worth modeling. */
        bail("branch no-op target");
        return NULL;
    }
    return block_for_label((int)target);
}

/* The symbolic stack must be empty whenever control flow converges;
   modeling values that cross block boundaries on the VM stack would
   require phi nodes (possible, but not implemented yet). */
static void require_empty_symstack(const char *where)
{
    if (symstack_depth != 0)
        bail(where);
}

static int block_is_terminated(void)
{
    LLVMBasicBlockRef b = LLVMGetInsertBlock(bld);
    return LLVMGetBasicBlockTerminator(b) != NULL;
}

/* ------------------------------------------------------------------------- */
/*   Opcode lifting                                                          */
/* ------------------------------------------------------------------------- */

/* Anything without direct IR semantics becomes a call to an opaque external
   function @i6.<opcode>. Source operands become arguments; if the opcode
   stores, the call result goes to the store operand. extra_args (from the
   symbolic stack, for call/glk) are appended after the regular sources. */
static void lift_opaque(const assembly_instruction *ai, int flags,
    LLVMValueRef *extra_args, int n_extra)
{
    char name[64];
    LLVMTypeRef params[16];
    LLVMTypeRef ft, ret;
    LLVMValueRef f, args[16];
    int n_src = ai->operand_count - ((flags & OPFLAG_STORE) ? 1 : 0);
    int i, n_args = 0;

    if (flags & OPFLAG_STORE2) {
        bail("opcode with two store operands");
        return;
    }
    if (n_src + n_extra > 16) {
        bail("too many operands for opaque call");
        return;
    }

    for (i = 0; i < n_src; i++)
        args[n_args++] = load_operand(&ai->operand[i]);
    for (i = 0; i < n_extra; i++)
        args[n_args++] = extra_args[i];
    if (lift_failed) return;

    for (i = 0; i < n_args; i++)
        params[i] = i32t;
    ret = (flags & OPFLAG_STORE) ? i32t : LLVMVoidTypeInContext(llctx);
    sprintf(name, "i6.%.32s%s", glulx_opcode_name(ai->internal_number),
        n_extra ? ".s" : "");
    f = declare_fn(name, ret, params, n_args, &ft);
    {   LLVMValueRef result = LLVMBuildCall2(bld, ft, f, args, n_args, "");
        if (flags & OPFLAG_STORE)
            store_operand(&ai->operand[ai->operand_count-1], result);
    }
}

/* Binary arithmetic/logic ops: L1 L2 S1. */
static void lift_binop(const assembly_instruction *ai, LLVMOpcode opc)
{
    LLVMValueRef a = load_operand(&ai->operand[0]);
    LLVMValueRef b = load_operand(&ai->operand[1]);
    if (lift_failed) return;
    store_operand(&ai->operand[2], LLVMBuildBinOp(bld, opc, a, b, ""));
}

/* Conditional branches: sources, then a branch-label operand. */
static void lift_condbranch(const assembly_instruction *ai,
    LLVMIntPredicate pred, int with_zero)
{
    LLVMValueRef a, b, cond;
    LLVMBasicBlockRef then_bb, else_bb;
    int32 target = ai->operand[ai->operand_count-1].value;

    a = load_operand(&ai->operand[0]);
    b = with_zero ? LLVMConstInt(i32t, 0, 0) : load_operand(&ai->operand[1]);
    if (lift_failed) return;
    require_empty_symstack("values on stack across conditional branch");
    then_bb = block_for_branch_target(target);
    if (lift_failed) return;
    cond = LLVMBuildICmp(bld, pred, a, b, "");
    else_bb = LLVMAppendBasicBlockInContext(llctx, fn, "next");
    LLVMBuildCondBr(bld, cond, then_bb, else_bb);
    LLVMPositionBuilderAtEnd(bld, else_bb);
}

static void lift_instruction(const assembly_instruction *ai)
{
    int flags = glulx_opcode_flags(ai->internal_number);
    int opno = glulx_opcode_operand_count(ai->internal_number);

    if (ai->internal_number == -1) {
        bail("custom @\"...\" opcode");
        return;
    }
    if (ai->operand_count != opno) {
        bail("operand count mismatch (source error)");
        return;
    }

    switch (ai->internal_number) {

    case nop_gc:
        return;

    /* -- arithmetic and logic ------------------------------------------- */
    case add_gc:    lift_binop(ai, LLVMAdd);  return;
    case sub_gc:    lift_binop(ai, LLVMSub);  return;
    case mul_gc:    lift_binop(ai, LLVMMul);  return;
    /* TODO(M3): Glulx division by zero is a VM fault, LLVM's is UB; guard
       or fall back before the optimizer is allowed to exploit this. Same
       for shift counts >= 32 (Glulx defines them; LLVM doesn't). */
    case div_gc:    lift_binop(ai, LLVMSDiv); return;
    case mod_gc:    lift_binop(ai, LLVMSRem); return;
    case bitand_gc: lift_binop(ai, LLVMAnd);  return;
    case bitor_gc:  lift_binop(ai, LLVMOr);   return;
    case bitxor_gc: lift_binop(ai, LLVMXor);  return;
    case shiftl_gc: lift_binop(ai, LLVMShl);  return;
    case sshiftr_gc: lift_binop(ai, LLVMAShr); return;
    case ushiftr_gc: lift_binop(ai, LLVMLShr); return;

    case neg_gc:
        {   LLVMValueRef a = load_operand(&ai->operand[0]);
            if (lift_failed) return;
            store_operand(&ai->operand[1], LLVMBuildNeg(bld, a, ""));
        }
        return;
    case bitnot_gc:
        {   LLVMValueRef a = load_operand(&ai->operand[0]);
            if (lift_failed) return;
            store_operand(&ai->operand[1], LLVMBuildNot(bld, a, ""));
        }
        return;
    case copy_gc:
        {   LLVMValueRef a = load_operand(&ai->operand[0]);
            if (lift_failed) return;
            store_operand(&ai->operand[1], a);
        }
        return;
    case sexs_gc:
    case sexb_gc:
        {   LLVMTypeRef small = (ai->internal_number == sexs_gc)
                ? LLVMInt16TypeInContext(llctx) : LLVMInt8TypeInContext(llctx);
            LLVMValueRef a = load_operand(&ai->operand[0]);
            if (lift_failed) return;
            a = LLVMBuildTrunc(bld, a, small, "");
            a = LLVMBuildSExt(bld, a, i32t, "");
            store_operand(&ai->operand[1], a);
        }
        return;

    /* -- control flow --------------------------------------------------- */
    case jump_gc:
        {   LLVMBasicBlockRef target;
            require_empty_symstack("values on stack across jump");
            target = block_for_branch_target(ai->operand[0].value);
            if (lift_failed) return;
            LLVMBuildBr(bld, target);
        }
        return;
    case jz_gc:   lift_condbranch(ai, LLVMIntEQ,  TRUE);  return;
    case jnz_gc:  lift_condbranch(ai, LLVMIntNE,  TRUE);  return;
    case jeq_gc:  lift_condbranch(ai, LLVMIntEQ,  FALSE); return;
    case jne_gc:  lift_condbranch(ai, LLVMIntNE,  FALSE); return;
    case jlt_gc:  lift_condbranch(ai, LLVMIntSLT, FALSE); return;
    case jle_gc:  lift_condbranch(ai, LLVMIntSLE, FALSE); return;
    case jgt_gc:  lift_condbranch(ai, LLVMIntSGT, FALSE); return;
    case jge_gc:  lift_condbranch(ai, LLVMIntSGE, FALSE); return;
    case jltu_gc: lift_condbranch(ai, LLVMIntULT, FALSE); return;
    case jleu_gc: lift_condbranch(ai, LLVMIntULE, FALSE); return;
    case jgtu_gc: lift_condbranch(ai, LLVMIntUGT, FALSE); return;
    case jgeu_gc: lift_condbranch(ai, LLVMIntUGE, FALSE); return;

    case return_gc:
        {   LLVMValueRef a = load_operand(&ai->operand[0]);
            if (lift_failed) return;
            require_empty_symstack("values on stack across return");
            LLVMBuildRet(bld, a);
        }
        return;

    /* -- calls with stack-passed arguments ------------------------------ */
    case call_gc:
    case glk_gc:
        {   /* (addr, count, store): count operands are popped from the
               stack at runtime; peel them off the symbolic stack and pass
               them explicitly to the opaque call. */
            LLVMValueRef extra[MAX_SYMBOLIC_STACK];
            const assembly_operand *cntop = &ai->operand[1];
            int32 cnt, i;
            if (!(cntop->type == ZEROCONSTANT_OT
                  || ((cntop->type == CONSTANT_OT || cntop->type == HALFCONSTANT_OT
                       || cntop->type == BYTECONSTANT_OT) && !cntop->marker))) {
                static char cntmsg[80];
                sprintf(cntmsg,
                    "call/glk with non-constant argument count (%s ot=%d val=%d mk=%d)",
                    glulx_opcode_name(ai->internal_number),
                    cntop->type, (int)cntop->value, cntop->marker);
                bail(cntmsg);
                return;
            }
            cnt = (cntop->type == ZEROCONSTANT_OT) ? 0 : cntop->value;
            if (cnt < 0 || cnt > symstack_depth) {
                bail("call/glk consumes more than the symbolic stack holds");
                return;
            }
            /* Runtime pop order: first argument is popped first, i.e. it
               is the value most recently pushed. */
            for (i = 0; i < cnt; i++)
                extra[i] = sympop();
            lift_opaque(ai, flags, extra, cnt);
        }
        return;

    /* -- stack manipulation is not modeled ------------------------------ */
    case stkcount_gc:
    case stkpeek_gc:
    case stkswap_gc:
    case stkroll_gc:
    case stkcopy_gc:
        bail("explicit stack-manipulation opcode");
        return;

    case catch_gc:
    case throw_gc:
        bail("catch/throw");
        return;

    default:
        if (flags & OPFLAG_BRANCH) {
            bail("unhandled branch opcode");
            return;
        }
        lift_opaque(ai, flags, NULL, 0);
        /* Opcodes that never return (quit, restart, tailcall...) end the
           basic block. */
        if (!lift_failed && (flags & OPFLAG_RETURNS)) {
            require_empty_symstack("values on stack at noreturn opcode");
            LLVMBuildUnreachable(bld);
        }
        return;
    }
}

/* ------------------------------------------------------------------------- */
/*   Routine lifting driver                                                  */
/* ------------------------------------------------------------------------- */

static LLVMModuleRef lift_routine(void)
{
    char fname[128];
    LLVMBasicBlockRef entry;
    int i, max_label;

    sprintf(fname, "%.100s.R%d", llvm_current_routine_name(), no_routines);

    mod = LLVMModuleCreateWithNameInContext(fname, llctx);
    bld = LLVMCreateBuilderInContext(llctx);
    i32t = LLVMInt32TypeInContext(llctx);
    lift_failed = FALSE;
    bail_reason = NULL;
    symstack_depth = 0;
    retblock[0] = retblock[1] = NULL;

    /* Locals become parameters (Glulx passes arguments in locals; callers
       of the routine are opaque to us, so the signature is our own
       convention) which are immediately spilled to allocas. */
    n_locals = no_locals;
    {   LLVMTypeRef *ptypes = my_calloc(sizeof(LLVMTypeRef),
            n_locals ? n_locals : 1, "llvm param types");
        for (i = 0; i < n_locals; i++) ptypes[i] = i32t;
        fn = LLVMAddFunction(mod, fname,
            LLVMFunctionType(i32t, ptypes, n_locals, 0));
        my_free(&ptypes, "llvm param types");
    }

    entry = LLVMAppendBasicBlockInContext(llctx, fn, "entry");
    LLVMPositionBuilderAtEnd(bld, entry);

    local_slots = my_calloc(sizeof(LLVMValueRef), n_locals+1,
        "llvm local slots");
    for (i = 1; i <= n_locals; i++) {
        local_slots[i] = LLVMBuildAlloca(bld, i32t, variable_name(i));
        LLVMBuildStore(bld, LLVMGetParam(fn, i-1), local_slots[i]);
    }

    max_label = -1;
    for (i = 0; i < llvm_event_count; i++) {
        if (llvm_events[i].is_label && llvm_events[i].label > max_label)
            max_label = llvm_events[i].label;
        if (!llvm_events[i].is_label) {
            const assembly_instruction *ai = &llvm_events[i].ai;
            int fl = glulx_opcode_flags(ai->internal_number);
            if ((fl & OPFLAG_BRANCH) && ai->operand_count >= 1
                && ai->operand[ai->operand_count-1].value > max_label)
                max_label = ai->operand[ai->operand_count-1].value;
        }
    }
    n_label_blocks = max_label + 1;
    label_blocks = my_calloc(sizeof(LLVMBasicBlockRef),
        n_label_blocks ? n_label_blocks : 1, "llvm label blocks");

    for (i = 0; i < llvm_event_count && !lift_failed; i++) {
        llvm_event *ev = &llvm_events[i];
        if (ev->is_label) {
            LLVMBasicBlockRef b;
            require_empty_symstack("values on stack at label");
            b = block_for_label(ev->label);
            if (lift_failed) break;
            if (!block_is_terminated())
                LLVMBuildBr(bld, b);        /* fall through */
            LLVMPositionBuilderAtEnd(bld, b);
        }
        else {
            if (block_is_terminated()) {
                /* Instruction with no incoming control flow. The classic
                   encoder would still emit it; don't try to be clever. */
                bail("instruction after terminator without label");
                break;
            }
            lift_instruction(&ev->ai);
        }
    }

    if (!lift_failed && !block_is_terminated())
        bail("routine body ends without terminator");

    /* Labels that were never defined (possible in code with errors). */
    if (!lift_failed) {
        for (i = 0; i < n_label_blocks; i++) {
            if (label_blocks[i] && !LLVMGetBasicBlockTerminator(label_blocks[i])
                && LLVMGetFirstInstruction(label_blocks[i]) == NULL) {
                /* Block created by a branch but never positioned: it has no
                   contents. If it's genuinely unreachable LLVM would cope,
                   but a branch targets it, so give up. */
                bail("branch to undefined label");
                break;
            }
        }
    }

    my_free(&local_slots, "llvm local slots");
    my_free(&label_blocks, "llvm label blocks");
    LLVMDisposeBuilder(bld);
    bld = NULL;

    if (lift_failed) {
        LLVMDisposeModule(mod);
        mod = NULL;
        return NULL;
    }
    return mod;
}

/* ------------------------------------------------------------------------- */
/*   Entry points (called from asm.c)                                        */
/* ------------------------------------------------------------------------- */

/* Process the captured routine. Returns TRUE if this pipeline emitted the
   routine's code (in which case asm.c must not replay the buffer), FALSE
   to fall back to the classic encoder. */
extern int llvm_pipeline_routine(void)
{
    LLVMModuleRef m;
    char *errmsg = NULL;

    if (!llctx)
        llctx = LLVMContextCreate();

    m = lift_routine();
    if (!m) {
        no_routines_bailed++;
        if (LLVM_CODEGEN >= 2)
            printf("! LLVM: bailed on %s: %s\n",
                llvm_current_routine_name(),
                bail_reason ? bail_reason : "unknown");
        return FALSE;
    }

    if (LLVMVerifyModule(m, LLVMReturnStatusAction, &errmsg)) {
        printf("! LLVM: verifier rejected %s: %s\n",
            llvm_current_routine_name(), errmsg ? errmsg : "");
        LLVMDisposeMessage(errmsg);
        LLVMDisposeModule(m);
        no_routines_bailed++;
        return FALSE;
    }
    LLVMDisposeMessage(errmsg);

    no_routines_lifted++;

    if (LLVM_CODEGEN >= 2) {
        char *ir = LLVMPrintModuleToString(m);
        if (!dump_file)
            dump_file = fopen("inform6-llvm-dump.ll", "w");
        if (dump_file)
            fprintf(dump_file, "%s\n", ir);
        LLVMDisposeMessage(ir);
    }

    LLVMDisposeModule(m);

    /* M2: lowering doesn't exist yet, so the classic encoder still emits
       every routine. */
    return FALSE;
}

extern void llvm_codegen_free(void)
{
    if (LLVM_CODEGEN && (no_routines_lifted || no_routines_bailed)) {
        printf("LLVM: lifted %d of %d captured routines\n",
            no_routines_lifted, no_routines_lifted + no_routines_bailed);
    }
    no_routines_lifted = 0;
    no_routines_bailed = 0;
    if (dump_file) {
        fclose(dump_file);
        dump_file = NULL;
    }
    if (llctx) {
        LLVMContextDispose(llctx);
        llctx = NULL;
    }
}
