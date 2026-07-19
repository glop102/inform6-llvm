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
/*   classic encoder instead.                                                */
/*                                                                           */
/*   Lifted routines run through LLVM's optimization passes and are lowered  */
/*   back to Glulx by llvm_lower.c, which rewrites the                        */
/*   capture buffer with the optimized instruction stream. Whichever         */
/*   stream the buffer ends up holding, asm.c replays it through the         */
/*   classic encoder, so llvm_pipeline_routine() always returns FALSE.       */
/* ------------------------------------------------------------------------- */

#include "header.h"
#include "llvm_codegen.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>

/* MemoryEffects bitmask values for the "memory" function attribute: two
   bits of ModRef (Ref=1, Mod=2) per memory location. LLVM 20 inserted the
   errno location, growing the mask from 3 locations to 4. */
#if LLVM_VERSION_MAJOR >= 20
#define MEMATTR_READONLY        0x55u   /* memory(read)  */
#else
#define MEMATTR_READONLY        0x15u
#endif
#define MEMATTR_INACCESSIBLE_RW 0x0Cu   /* memory(inaccessiblemem: readwrite) */

/* ------------------------------------------------------------------------- */
/*   Lift state                                                              */
/* ------------------------------------------------------------------------- */

#define MAX_SYMBOLIC_STACK 64

static LLVMContextRef llctx;        /* created lazily, lives all compile     */
static FILE *dump_file;             /* $LLVM=3 dump stream, opened lazily    */

static int no_routines_lifted;      /* statistics                            */
static int no_routines_bailed;
static int no_routines_lowered;
static int no_routines_unlowered;

/* The optimization pipeline (new pass manager syntax). Deliberately no
   inlining or IPO: one IR function per routine. licm hoists loop-invariant
   computations, loads, and readonly opcode calls out of loops (a pure
   dynamic win under the instructions-dispatched cost model).

   loop-rotate was tried and rejected: the rotated bottom test reads the
   *incremented* induction variable, giving the increment a third use,
   which defeats the lowerer's edge-copy folding — the saved backedge jump
   comes back as a phi copy per iteration, and the duplicated guards cost
   static size for nothing (measured: cloak +744 instructions, life bench
   ~40ms slower). Revisit if the lowerer learns interference-based
   coalescing of the increment with its phi's slot. */
#define LLVM_PASS_PIPELINE \
    "function(mem2reg,instcombine,simplifycfg,reassociate," \
    "loop-mssa(licm),gvn,dce,simplifycfg)"

/* Per-routine state. */
static LLVMModuleRef  mod;
static LLVMBuilderRef bld;
static LLVMValueRef   fn;
static LLVMTypeRef    i32t;

static LLVMValueRef  *local_slots;  /* allocas, indexed 1..n_locals          */
static int            n_locals;

static LLVMBasicBlockRef *label_blocks; /* indexed by label number           */
static int            n_label_blocks;

/* Values living on the VM stack where control flow converges (Inform's
   condition-to-value idioms) become phis at the target label. Each label
   remembers its entry-stack shape; every edge must arrive with the same
   depth or the routine bails. */
#define MAX_ENTRY_STACK 8
typedef struct label_entry_s {
    int depth;                              /* -1 until the first edge     */
    LLVMValueRef phis[MAX_ENTRY_STACK];
} label_entry;
static label_entry   *label_entries;        /* parallel to label_blocks    */

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
   optimization as an opaque token, not fold as an integer.

   It behaves like a pure function of its arguments — no memory effects, no
   unwinding, always returns — and is marked as such so that identical
   symbolic constants can merge, dead ones can vanish, and the calls never
   act as optimization barriers. The *values* still can't fold, which is
   the point. */
static void add_fn_attr(LLVMValueRef f, const char *name, uint64_t val)
{
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    LLVMAddAttributeAtIndex(f, LLVMAttributeFunctionIndex,
        LLVMCreateEnumAttribute(llctx, kind, val));
}

static void mark_fn_as_constant(LLVMValueRef f)
{
    add_fn_attr(f, "memory", 0);            /* memory(none) */
    add_fn_attr(f, "nounwind", 0);
    add_fn_attr(f, "willreturn", 0);
    add_fn_attr(f, "speculatable", 0);
}

/* A function that reads memory but never writes it. Do not mark these calls
   nounwind or willreturn: invalid addresses produce observable VM faults, and
   searches over malformed data may not terminate. */
static void mark_fn_as_readonly(LLVMValueRef f)
{
    add_fn_attr(f, "memory", MEMATTR_READONLY);
}

/* Touches VM state outside the memory map (the RNG): calls stay ordered
   with respect to each other and never merge, but loads and stores of
   RAM and globals move freely across them. */
static void mark_fn_as_inaccessible_rw(LLVMValueRef f)
{
    add_fn_attr(f, "memory", MEMATTR_INACCESSIBLE_RW);
    add_fn_attr(f, "nounwind", 0);
    add_fn_attr(f, "willreturn", 0);
}

/* ------------------------------------------------------------------------- */
/*   Opcode memory behavior                                                  */
/*                                                                           */
/*   Glulx opcodes that never write RAM, globals, or locals and never call   */
/*   back into VM code. The lifter grades their IR declarations so LLVM      */
/*   can optimize around them; the lowerer's clobber analysis (via           */
/*   llvm_codegen.h) uses the same answer. Stream opcodes are deliberately   */
/*   absent: under @setiosys 1 ("filter") every one of them invokes an       */
/*   arbitrary routine per character.                                        */
/* ------------------------------------------------------------------------- */

/* Pure functions of their operands (Glulx float math is deterministic
   and cannot fault). Only single-store opcodes appear: two-store float
   opcodes never lift. */
static const char *const pure_opcodes[] = {
    "numtof", "ftonumz", "ftonumn", "ceil", "floor",
    "fadd", "fsub", "fmul", "fdiv",
    "sqrt", "exp", "log", "pow",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
    "dtonumz", "dtonumn", "dtof",
    NULL
};

/* Read RAM (or query fixed VM state) without writing anything. */
static const char *const readonly_opcodes[] = {
    "aload", "aloads", "aloadb", "aloadbit",
    "linearsearch", "binarysearch", "linkedsearch",
    "gestalt",
    NULL
};

/* Touch only state outside the memory map (the RNG). */
static const char *const inaccessible_opcodes[] = {
    "random", "setrandom",
    NULL
};

static int name_in_list(const char *name, const char *const *list)
{
    int i;
    for (i = 0; list[i]; i++)
        if (strcmp(name, list[i]) == 0) return TRUE;
    return FALSE;
}

extern int llvm_opcode_no_ram_write(const char *opname)
{
    return name_in_list(opname, pure_opcodes)
        || name_in_list(opname, readonly_opcodes)
        || name_in_list(opname, inaccessible_opcodes);
}

static void mark_opaque_fn_attrs(LLVMValueRef f, const char *opname)
{
    if (name_in_list(opname, pure_opcodes))
        mark_fn_as_constant(f);
    else if (name_in_list(opname, readonly_opcodes))
        mark_fn_as_readonly(f);
    else if (name_in_list(opname, inaccessible_opcodes))
        mark_fn_as_inaccessible_rw(f);
}

static LLVMValueRef symbolic_constant(int marker, int32 value, int symindex)
{
    LLVMTypeRef params[3];
    LLVMTypeRef ft;
    LLVMValueRef f, args[3];
    int is_new;
    params[0] = i32t; params[1] = i32t; params[2] = i32t;
    is_new = (LLVMGetNamedFunction(mod, "i6.sym") == NULL);
    f = declare_fn("i6.sym", i32t, params, 3, &ft);
    if (is_new)
        mark_fn_as_constant(f);
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
            int is_new;
            params[0] = i32t;
            is_new = (LLVMGetNamedFunction(mod, "i6.deref") == NULL);
            f = declare_fn("i6.deref", i32t, params, 1, &ft);
            if (is_new)
                mark_fn_as_readonly(f);
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

/* Record an edge from the current block to label n, carrying the current
   symbolic stack: the first edge fixes the label's entry-stack depth and
   creates its phis; every edge adds its stack values as incomings. */
static LLVMBasicBlockRef edge_to_label(int n)
{
    LLVMBasicBlockRef b = block_for_label(n);
    LLVMBasicBlockRef pred;
    label_entry *e;
    int i;

    if (!b) return NULL;
    e = &label_entries[n];
    if (e->depth < 0) {
        if (symstack_depth > MAX_ENTRY_STACK) {
            bail("too many values on stack at branch");
            return NULL;
        }
        e->depth = symstack_depth;
        if (e->depth > 0) {
            /* The block has no contents yet (it gets them when its label
               is defined), so the phis land at its start. */
            LLVMBasicBlockRef saved = LLVMGetInsertBlock(bld);
            LLVMPositionBuilderAtEnd(bld, b);
            for (i = 0; i < e->depth; i++)
                e->phis[i] = LLVMBuildPhi(bld, i32t, "stk");
            LLVMPositionBuilderAtEnd(bld, saved);
        }
    }
    else if (e->depth != symstack_depth) {
        bail("stack depth mismatch where control flow joins");
        return NULL;
    }
    pred = LLVMGetInsertBlock(bld);
    for (i = 0; i < e->depth; i++)
        LLVMAddIncoming(e->phis[i], &symstack[i], &pred, 1);
    return b;
}

/* Target block for a branch operand: a label number, or the special
   values -3 ("branch means return 0") / -4 ("branch means return 1").
   Branching to a return discards the VM stack, so no edge bookkeeping. */
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
    return edge_to_label((int)target);
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
    {   int is_new = (LLVMGetNamedFunction(mod, name) == NULL);
        f = declare_fn(name, ret, params, n_args, &ft);
        if (is_new)
            mark_opaque_fn_attrs(f, glulx_opcode_name(ai->internal_number));
    }
    {   LLVMValueRef result = LLVMBuildCall2(bld, ft, f, args, n_args, "");
        if (flags & OPFLAG_STORE)
            store_operand(&ai->operand[ai->operand_count-1], result);
    }
}

/* If the operand is an unmarked integer constant, put its value in *v. */
static int operand_constant_value(const assembly_operand *op, int32 *v)
{
    if (op->marker) return FALSE;
    switch (op->type) {
    case ZEROCONSTANT_OT:
        *v = 0; return TRUE;
    case CONSTANT_OT:
    case HALFCONSTANT_OT:
    case BYTECONSTANT_OT:
        *v = op->value; return TRUE;
    }
    return FALSE;
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
    then_bb = block_for_branch_target(target);
    if (lift_failed) return;
    cond = LLVMBuildICmp(bld, pred, a, b, "");
    else_bb = LLVMAppendBasicBlockInContext(llctx, fn, "next");
    LLVMBuildCondBr(bld, cond, then_bb, else_bb);
    /* The fallthrough path keeps the same stack values (single pred). */
    LLVMPositionBuilderAtEnd(bld, else_bb);
}

static void lift_instruction(const assembly_instruction *ai)
{
    int flags, opno;

    if (ai->internal_number == -1) {
        bail("custom @\"...\" opcode");
        return;
    }
    flags = glulx_opcode_flags(ai->internal_number);
    opno = glulx_opcode_operand_count(ai->internal_number);
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
    case bitand_gc: lift_binop(ai, LLVMAnd);  return;
    case bitor_gc:  lift_binop(ai, LLVMOr);   return;
    case bitxor_gc: lift_binop(ai, LLVMXor);  return;

    /* Division: Glulx division by zero is a VM fault (an observable side
       effect), and INT_MIN/-1 overflows; both are UB for LLVM's sdiv/srem.
       Lift only divisions whose safety is visible in the divisor; the
       rest become opaque calls, which pins the fault's ordering. */
    case div_gc:
    case mod_gc:
        {   int32 dv;
            if (operand_constant_value(&ai->operand[1], &dv)
                && dv != 0 && dv != -1)
                lift_binop(ai, ai->internal_number == div_gc
                    ? LLVMSDiv : LLVMSRem);
            else
                lift_opaque(ai, flags, NULL, 0);
        }
        return;

    /* Shifts: Glulx defines counts >= 32 (result 0, or all sign bits for
       sshiftr, with negative counts read as unsigned); LLVM shifts give
       poison. Lift constant counts, folding oversized ones to their
       defined Glulx results; variable counts become opaque calls. */
    case shiftl_gc:
    case sshiftr_gc:
    case ushiftr_gc:
        {   int32 cnt;
            if (!operand_constant_value(&ai->operand[1], &cnt)) {
                lift_opaque(ai, flags, NULL, 0);
                return;
            }
            if ((uint32_t)cnt < 32) {
                lift_binop(ai, ai->internal_number == shiftl_gc ? LLVMShl
                    : ai->internal_number == sshiftr_gc ? LLVMAShr
                    : LLVMLShr);
                return;
            }
            {   LLVMValueRef a = load_operand(&ai->operand[0]);
                LLVMValueRef r;
                if (lift_failed) return;
                if (ai->internal_number == sshiftr_gc)
                    r = LLVMBuildAShr(bld, a, LLVMConstInt(i32t, 31, 0), "");
                else
                    r = LLVMConstInt(i32t, 0, 0);
                store_operand(&ai->operand[2], r);
            }
        }
        return;

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
    case copys_gc:
    case copyb_gc:
        /* Byte/short copies read and write memory-mode (and RAM-mode)
           operands at their own width; the word-based deref abstraction
           would misread them (e.g. the "inversion" statement's header
           bytes). Rare enough to just fall back. */
        bail("byte/short copy opcode");
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
            /* Anything left on the VM stack dies with the call frame. */
            LLVMBuildRet(bld, a);
        }
        return;

    /* -- calls with stack-passed arguments ------------------------------ */
    case call_gc:
    case glk_gc:
    case tailcall_gc:
        {   /* (addr, count[, store]): count operands are popped from the
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
            /* @tailcall never returns. */
            if (!lift_failed && (flags & OPFLAG_RETURNS))
                LLVMBuildUnreachable(bld);
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
        if (!lift_failed && (flags & OPFLAG_RETURNS))
            LLVMBuildUnreachable(bld);
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
    /* Declare i32 the only legal integer width: instcombine then leaves
       32-bit arithmetic alone instead of narrowing it to i8/i16, which
       Glulx has no instructions for (the "narrow select" lower-bail). */
    LLVMSetDataLayout(mod, "e-p:32:32-i32:32-n32");
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
    label_entries = my_calloc(sizeof(label_entry),
        n_label_blocks ? n_label_blocks : 1, "llvm label entries");
    for (i = 0; i < n_label_blocks; i++)
        label_entries[i].depth = -1;

    for (i = 0; i < llvm_event_count && !lift_failed; i++) {
        llvm_event *ev = &llvm_events[i];
        if (ev->is_label) {
            LLVMBasicBlockRef b;
            label_entry *e;
            int k;
            if (!block_is_terminated()) {
                /* Falling through is an edge like any other. */
                b = edge_to_label(ev->label);
                if (lift_failed) break;
                LLVMBuildBr(bld, b);
            }
            else {
                b = block_for_label(ev->label);
                if (lift_failed) break;
            }
            e = &label_entries[ev->label];
            if (e->depth < 0)
                e->depth = 0;   /* no edges yet: only reachable backward */
            LLVMPositionBuilderAtEnd(bld, b);
            /* The block's entry stack is its phis. */
            symstack_depth = e->depth;
            for (k = 0; k < e->depth; k++)
                symstack[k] = e->phis[k];
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
    my_free(&label_entries, "llvm label entries");
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
/*   Post-optimization cleanup                                               */
/*                                                                           */
/*   simplifycfg/instcombine merge conditional reads of two globals into a   */
/*   load of a select-of-pointers, which has no Glulx encoding. Rewrite      */
/*   such loads back into selects of loads (globals are always readable,     */
/*   so hoisting the loads is safe), leaving IR the lowerer understands.     */
/* ------------------------------------------------------------------------- */

static int is_global_select_tree(LLVMValueRef p, int depth)
{
    if (LLVMIsAGlobalVariable(p)) {
        size_t len;
        const char *name = LLVMGetValueName2(p, &len);
        return name && strncmp(name, "i6.g", 4) == 0;
    }
    if (depth >= 4 || !LLVMIsASelectInst(p)) return FALSE;
    return is_global_select_tree(LLVMGetOperand(p, 1), depth + 1)
        && is_global_select_tree(LLVMGetOperand(p, 2), depth + 1);
}

static LLVMValueRef load_of_select_tree(LLVMBuilderRef b, LLVMValueRef p)
{
    if (LLVMIsAGlobalVariable(p))
        return LLVMBuildLoad2(b, i32t, p, "");
    return LLVMBuildSelect(b, LLVMGetOperand(p, 0),
        load_of_select_tree(b, LLVMGetOperand(p, 1)),
        load_of_select_tree(b, LLVMGetOperand(p, 2)), "");
}

static void unmerge_pointer_selects(LLVMValueRef f)
{
    LLVMBuilderRef b = LLVMCreateBuilderInContext(llctx);
    LLVMBasicBlockRef bb;
    LLVMValueRef in, next;
    int changed;

    for (bb = LLVMGetFirstBasicBlock(f); bb; bb = LLVMGetNextBasicBlock(bb)) {
        for (in = LLVMGetFirstInstruction(bb); in; in = next) {
            LLVMValueRef ptr, v;
            next = LLVMGetNextInstruction(in);
            if (!LLVMIsALoadInst(in)) continue;
            if (LLVMTypeOf(in) != i32t) continue;
            ptr = LLVMGetOperand(in, 0);
            if (LLVMIsAGlobalVariable(ptr)
                || !is_global_select_tree(ptr, 0)) continue;
            LLVMPositionBuilderBefore(b, in);
            v = load_of_select_tree(b, ptr);
            LLVMReplaceAllUsesWith(in, v);
            LLVMInstructionEraseFromParent(in);
        }
    }
    /* Sweep the now-dead pointer selects (innermost last). */
    do {
        changed = FALSE;
        for (bb = LLVMGetFirstBasicBlock(f); bb;
             bb = LLVMGetNextBasicBlock(bb)) {
            for (in = LLVMGetFirstInstruction(bb); in; in = next) {
                next = LLVMGetNextInstruction(in);
                if (LLVMIsASelectInst(in) && !LLVMGetFirstUse(in)
                    && LLVMGetTypeKind(LLVMTypeOf(in))
                       == LLVMPointerTypeKind) {
                    LLVMInstructionEraseFromParent(in);
                    changed = TRUE;
                }
            }
        }
    } while (changed);
    LLVMDisposeBuilder(b);
}

/* ------------------------------------------------------------------------- */
/*   Entry points (called from asm.c)                                        */
/* ------------------------------------------------------------------------- */

/* $LLVM=3: dump the routine's IR, tagged with the pipeline phase. */
static void dump_module(LLVMModuleRef m, const char *phase)
{
    char *ir;
    if (LLVM_CODEGEN < 3)
        return;
    if (!dump_file)
        dump_file = fopen("inform6-llvm-dump.ll", "w");
    if (!dump_file)
        return;
    ir = LLVMPrintModuleToString(m);
    fprintf(dump_file, "; ===== %s (%s)\n%s\n",
        llvm_current_routine_name(), phase, ir);
    LLVMDisposeMessage(ir);
}

/* Process the captured routine. Lift it to IR, optimize, and lower it
   back into the capture buffer; if any stage can't cope, the buffer is
   left holding the original instruction stream. Always returns FALSE:
   asm.c replays whichever stream the buffer holds through the classic
   encoder (so labels, branch shortening and backpatch markers are handled
   by the existing machinery either way). */
extern int llvm_pipeline_routine(void)
{
    LLVMModuleRef m;
    char *errmsg = NULL;

    /* Level 1: capture and replay only — prove the seam, change nothing. */
    if (LLVM_CODEGEN < 2)
        return FALSE;

    if (!llctx)
        llctx = LLVMContextCreate();

    m = lift_routine();
    if (!m) {
        no_routines_bailed++;
        if (LLVM_CODEGEN >= 3)
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
        mod = NULL;
        no_routines_bailed++;
        return FALSE;
    }
    LLVMDisposeMessage(errmsg);

    no_routines_lifted++;
    dump_module(m, "pre-opt");

    {   LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        LLVMErrorRef err = LLVMRunPasses(m, LLVM_PASS_PIPELINE, NULL, opts);
        LLVMDisposePassBuilderOptions(opts);
        if (err) {
            char *msg = LLVMGetErrorMessage(err);
            printf("! LLVM: pass pipeline failed on %s: %s\n",
                llvm_current_routine_name(), msg ? msg : "");
            LLVMDisposeErrorMessage(msg);
            LLVMDisposeModule(m);
            mod = NULL;
            no_routines_unlowered++;
            return FALSE;
        }
    }

    unmerge_pointer_selects(fn);

    dump_module(m, "post-opt");

    /* Debugging aid: I6_LLVM_LIMIT=<n> lowers only the first n lifted
       routines (the rest replay classically), for bisecting a bad one. */
    {   static int limit = -2;
        if (limit == -2) {
            const char *s = getenv("I6_LLVM_LIMIT");
            limit = s ? atoi(s) : -1;
        }
        if (limit >= 0 && no_routines_lifted > limit) {
            LLVMDisposeModule(m);
            mod = NULL;
            no_routines_unlowered++;
            return FALSE;
        }
        if (limit >= 0 && no_routines_lifted == limit)
            fprintf(stderr, "I6_LLVM_LIMIT: last lowered routine is #%d %s\n",
                no_routines_lifted, llvm_current_routine_name());
    }

    {   const char *why = NULL;
        int insts_in = 0, insts_out = 0;
        if (llvm_lower_routine(m, fn, &why, &insts_in, &insts_out)) {
            no_routines_lowered++;
            if (LLVM_CODEGEN >= 3)
                printf("LLVM: routine %s: %d instructions -> %d\n",
                    llvm_current_routine_name(), insts_in, insts_out);
        }
        else {
            no_routines_unlowered++;
            if (LLVM_CODEGEN >= 3)
                printf("! LLVM: could not lower %s: %s\n",
                    llvm_current_routine_name(), why ? why : "unknown");
        }
    }

    LLVMDisposeModule(m);
    mod = NULL;
    return FALSE;
}

extern void llvm_codegen_free(void)
{
    if (LLVM_CODEGEN && (no_routines_lifted || no_routines_bailed)) {
        printf("LLVM: optimized %d of %d captured routines "
            "(%d not lifted, %d not lowered); "
            "%d instructions -> %d\n",
            no_routines_lowered,
            no_routines_lifted + no_routines_bailed,
            no_routines_bailed, no_routines_unlowered,
            llvm_lower_insts_in, llvm_lower_insts_out);
    }
    no_routines_lifted = 0;
    no_routines_bailed = 0;
    no_routines_lowered = 0;
    no_routines_unlowered = 0;
    llvm_lower_insts_in = 0;
    llvm_lower_insts_out = 0;
    if (dump_file) {
        fclose(dump_file);
        dump_file = NULL;
    }
    if (llctx) {
        LLVMContextDispose(llctx);
        llctx = NULL;
    }
}
