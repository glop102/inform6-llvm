/* ------------------------------------------------------------------------- */
/*   "llvm_lower" : Lowering optimized LLVM IR back to Glulx instructions    */
/*                                                                           */
/*   Part of inform6-llvm. Not part of the upstream Inform 6 compiler.       */
/*                                                                           */
/*   The lifter (llvm_codegen.c) turns a captured routine into an IR         */
/*   function; after LLVM's passes have run, this module turns the IR back   */
/*   into Glulx instructions by rewriting the capture buffer in asm.c,       */
/*   which is then replayed through the classic encoder (so branch           */
/*   shortening and backpatch markers are handled by existing machinery).    */
/*                                                                           */
/*   Value representation (decided per SSA value, M4):                       */
/*     VK_STACK  - produced by one instruction, consumed by the next use in  */
/*                 LIFO order: rides the VM value stack ("sp" operands),     */
/*                 costing no slot and no copies;                            */
/*     VK_GLOBAL - a load of an i6 global none of whose uses can observe an  */
/*                 intervening write: uses read the global operand directly; */
/*     VK_SLOT   - lives in a Glulx local slot. Slots are reused: values     */
/*                 whose whole live range sits inside one basic block share  */
/*                 a pool of slots by linear-scan; values that cross blocks  */
/*                 (and phis) get dedicated slots.                           */
/*                                                                           */
/*   An interpreter's cost model is instructions dispatched, so the goal     */
/*   of all of this is simply fewer instructions (mostly: fewer copies).     */
/*                                                                           */
/*   Lowering runs in two phases:                                            */
/*     A. classify + validate + representation/slot assignment: no side      */
/*        effects on the compiler state, so an unsupported construct can     */
/*        still fall back to the classic replay;                             */
/*     B. emit: cannot fail; rewrites the capture buffer.                    */
/* ------------------------------------------------------------------------- */

#include "header.h"
#include "llvm_codegen.h"

#include <llvm-c/Core.h>

/* The most local slots a lowered routine may use. The locals format and
   operand encodings go well beyond this (two-byte frame offsets, multiple
   format pairs); 250 keeps frames within one format pair. */
#define MAX_LOWER_SLOTS 250

/* The most phi-copy stub blocks one terminator may need. */
#define MAX_STUBS 64

/* The most runtime reads one instruction's emission can make. */
#define MAX_READS 64

/* The most values simultaneously riding the VM stack between blocks'
   instructions (per basic block; overflow just stops fusing). */
#define MAX_PEND 16

/* ------------------------------------------------------------------------- */
/*   Lowering state                                                          */
/* ------------------------------------------------------------------------- */

typedef enum valkind_e {
    VK_SLOT,        /* value lives in a Glulx local slot                    */
    VK_STACK,       /* value rides the VM value stack (push at def, pop at  */
                    /*   its single use)                                    */
    VK_GLOBAL,      /* load of an i6 global whose uses read it directly     */
    VK_FUSED,       /* icmp folded into its single branch/select user       */
    VK_ALIAS,       /* same runtime representation as another value         */
    VK_SKIP         /* no general runtime representation (i6.sym calls,     */
                    /*   void calls, stores, narrow truncs, terminators...) */
} valkind;

typedef struct valinfo_s {
    LLVMValueRef v;
    valkind kind;
    int slot;               /* VK_SLOT */
    int scratch;            /* phi: parallel-copy staging; udiv/urem: temp  */
    int needs_scratch;      /* udiv/urem with 2 <= divisor < 2^31           */
    LLVMValueRef alias_to;  /* VK_ALIAS */
    LLVMValueRef sunk_into; /* select this arm computation is emitted in    */
    LLVMValueRef edge_fold; /* phi whose edge copy emits this computation   */
    int pos;                /* linear position in layout order              */
    int blk;                /* index of containing block                    */
    int last_use;           /* last position whose emission reads the value */
    int cross;              /* some read happens outside the def's block    */
} valinfo;

typedef struct blockinfo_s {
    LLVMBasicBlockRef bb;
    int label;              /* -1 for the entry block                       */
    int hazard;             /* phi copies on some edge need staging         */
    LLVMValueRef ret_inline;/* constant operand of a lone "ret c", or NULL  */
    int retconst;           /* 0/1 if ret_inline is that constant, else -1  */
    int emit_body;          /* FALSE: every reference is inlined, skip it   */
    int end_pos;            /* position of the terminator                   */
} blockinfo;

static valinfo   *vals;
static int        n_vals;
static blockinfo *blkinfo;
static int        n_blocks;

static LLVMValueRef cur_fn;
static int   n_params;
static int   next_slot;      /* next free local slot                        */
static int   lower_failed;
static const char *lower_fail_reason;
static int   last_emit_noreturn;  /* last emitted opcode had the Rf flag    */

static void lfail(const char *why)
{   if (!lower_failed) {
        lower_failed = TRUE;
        lower_fail_reason = why;
    }
}

/* ------------------------------------------------------------------------- */
/*   IR inspection helpers                                                   */
/* ------------------------------------------------------------------------- */

static int int_width(LLVMValueRef v)
{
    LLVMTypeRef t = LLVMTypeOf(v);
    if (LLVMGetTypeKind(t) != LLVMIntegerTypeKind) return 0;
    return (int)LLVMGetIntTypeWidth(t);
}

/* Name of the directly-called function, or NULL for indirect calls. */
static const char *called_fn_name(LLVMValueRef call)
{
    LLVMValueRef callee = LLVMGetCalledValue(call);
    size_t len;
    if (!LLVMIsAFunction(callee)) return NULL;
    return LLVMGetValueName2(callee, &len);
}

static int is_sym_call(LLVMValueRef v)
{
    const char *name;
    if (!LLVMIsACallInst(v)) return FALSE;
    name = called_fn_name(v);
    return name && strcmp(name, "i6.sym") == 0;
}

static int name_has_prefix(const char *name, const char *prefix)
{   return strncmp(name, prefix, strlen(prefix)) == 0;
}

static valinfo *lookup(LLVMValueRef v)
{
    int i;
    for (i = 0; i < n_vals; i++)
        if (vals[i].v == v) return &vals[i];
    return NULL;
}

/* Chase VK_ALIAS chains down to the underlying representation. */
static valinfo *underlying(LLVMValueRef v)
{
    valinfo *vi = lookup(v);
    while (vi && vi->kind == VK_ALIAS)
        vi = lookup(vi->alias_to);
    return vi;
}

/* The value whose slot or stack entry a read of v actually touches:
   aliases chased, everything without storage (constants, arguments,
   globals-read-direct, symbolic constants...) mapped to NULL. */
static LLVMValueRef rep(LLVMValueRef v)
{
    valinfo *vi;
    if (!v || !LLVMIsAInstruction(v)) return NULL;
    vi = underlying(v);
    if (!vi) return NULL;
    if (vi->kind == VK_SLOT || vi->kind == VK_STACK) return vi->v;
    return NULL;
}

static int param_index(LLVMValueRef v)
{
    int i;
    for (i = 0; i < n_params; i++)
        if (LLVMGetParam(cur_fn, i) == v) return i;
    return -1;
}

static blockinfo *block_info(LLVMBasicBlockRef bb)
{
    int i;
    for (i = 0; i < n_blocks; i++)
        if (blkinfo[i].bb == bb) return &blkinfo[i];
    return NULL;
}

static int phi_count(LLVMBasicBlockRef bb)
{
    LLVMValueRef in;
    int n = 0;
    for (in = LLVMGetFirstInstruction(bb); in && LLVMIsAPHINode(in);
         in = LLVMGetNextInstruction(in))
        n++;
    return n;
}

/* The value flowing into a phi along the edge from pred. */
static LLVMValueRef incoming_for(LLVMValueRef phi, LLVMBasicBlockRef pred)
{
    unsigned i, n = LLVMCountIncoming(phi);
    for (i = 0; i < n; i++)
        if (LLVMGetIncomingBlock(phi, i) == pred)
            return LLVMGetIncomingValue(phi, i);
    return NULL;
}

/* An icmp can fuse into its user (becoming a Glulx conditional branch or
   guarding a select) when it has exactly one use, by a conditional branch
   or select in the same basic block. Same-block matters: slots hold the
   values of the *most recent* execution of their definitions, so a compare
   can only be re-materialized where no back edge can have intervened. */
static int icmp_fusable(LLVMValueRef inst)
{
    LLVMUseRef use = LLVMGetFirstUse(inst);
    LLVMValueRef user;
    if (!use || LLVMGetNextUse(use)) return FALSE;
    user = LLVMGetUser(use);
    if (!LLVMIsAInstruction(user)) return FALSE;
    if (LLVMGetInstructionParent(user) != LLVMGetInstructionParent(inst))
        return FALSE;
    switch (LLVMGetInstructionOpcode(user)) {
    case LLVMBr:
        return LLVMIsConditional(user)
            && LLVMGetCondition(user) == inst;
    case LLVMSelect:
        return LLVMGetOperand(user, 0) == inst;
    default:
        return FALSE;
    }
}

static int32 const_int_value(LLVMValueRef v)
{   return (int32)(uint32_t)LLVMConstIntGetZExtValue(v);
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 1: classify every instruction                             */
/* ------------------------------------------------------------------------- */

static void classify(LLVMValueRef in, valinfo *vi)
{
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);
    int w = int_width(in);

    vi->v = in;
    vi->kind = VK_SKIP;
    vi->slot = 0;
    vi->scratch = 0;
    vi->needs_scratch = FALSE;
    vi->alias_to = NULL;
    vi->sunk_into = NULL;
    vi->edge_fold = NULL;

    switch (opc) {
    case LLVMRet:
    case LLVMBr:
    case LLVMSwitch:
    case LLVMUnreachable:
        return;

    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
        if (w != 32) { lfail("narrow arithmetic"); return; }
        vi->kind = VK_SLOT;
        return;

    case LLVMAnd: case LLVMOr: case LLVMXor:
        /* i1 logic works on the 0/1 slot representation; i1 *arithmetic*
           (add = xor etc.) would not, but instcombine canonicalizes it
           away and the case above rejects it. */
        if (w != 32 && w != 1) { lfail("narrow logic op"); return; }
        vi->kind = VK_SLOT;
        return;

    case LLVMUDiv: case LLVMURem:
        /* Glulx has no unsigned division opcode; a constant divisor can
           be lowered with the halving trick (see emit_udiv_urem). */
        {   LLVMValueRef d = LLVMGetOperand(in, 1);
            uint32_t c;
            if (w != 32) { lfail("narrow unsigned division"); return; }
            if (!LLVMIsAConstantInt(d)) {
                lfail("unsigned division by non-constant");
                return;
            }
            c = (uint32_t)LLVMConstIntGetZExtValue(d);
            if (c == 0) { lfail("unsigned division by zero"); return; }
            vi->kind = VK_SLOT;
            if (c >= 2 && c < 0x80000000u) vi->needs_scratch = TRUE;
        }
        return;

    case LLVMICmp:
        {   int ow = int_width(LLVMGetOperand(in, 0));
            LLVMIntPredicate p = LLVMGetICmpPredicate(in);
            if (ow != 32) {
                /* i1 compares: the 0/1 representation matches unsigned
                   semantics but not signed (where true = -1). */
                if (ow != 1) { lfail("narrow compare"); return; }
                if (p == LLVMIntSLT || p == LLVMIntSLE
                    || p == LLVMIntSGT || p == LLVMIntSGE) {
                    lfail("signed compare of i1");
                    return;
                }
            }
            vi->kind = icmp_fusable(in) ? VK_FUSED : VK_SLOT;
        }
        return;

    case LLVMSelect:
        if (w != 32 && w != 1) { lfail("narrow select"); return; }
        vi->kind = VK_SLOT;
        return;

    case LLVMPHI:
        if (w != 32 && w != 1) { lfail("narrow phi"); return; }
        vi->kind = VK_SLOT;
        return;

    case LLVMFreeze:
        /* Our operands are always concrete at runtime; freeze is a no-op. */
        vi->kind = VK_ALIAS;
        vi->alias_to = LLVMGetOperand(in, 0);
        return;

    case LLVMZExt:
        {   int sw = int_width(LLVMGetOperand(in, 0));
            if (w != 32) { lfail("zext to narrow type"); return; }
            if (sw == 1) {
                /* The 0/1 slot already is the zext. */
                vi->kind = VK_ALIAS;
                vi->alias_to = LLVMGetOperand(in, 0);
            }
            else if (sw == 8 || sw == 16) {
                /* zext(trunc x) == x & mask */
                vi->kind = VK_SLOT;
            }
            else lfail("zext from unhandled width");
        }
        return;

    case LLVMSExt:
        {   int sw = int_width(LLVMGetOperand(in, 0));
            if (w != 32) { lfail("sext to narrow type"); return; }
            if (sw == 1 || sw == 8 || sw == 16) {
                vi->kind = VK_SLOT;
            }
            else lfail("sext from unhandled width");
        }
        return;

    case LLVMTrunc:
        if (int_width(LLVMGetOperand(in, 0)) != 32) {
            lfail("trunc from unhandled width");
            return;
        }
        if (w == 1) {
            /* Representable: x & 1 gives the 0/1 slot form. */
            vi->kind = VK_SLOT;
            return;
        }
        if (w == 8 || w == 16) {
            /* Only allowed as the inner half of sext/zext(trunc x)
               patterns; the extending user reads x directly. */
            LLVMUseRef use;
            for (use = LLVMGetFirstUse(in); use; use = LLVMGetNextUse(use)) {
                LLVMValueRef user = LLVMGetUser(use);
                LLVMOpcode uo;
                if (!LLVMIsAInstruction(user)) { lfail("trunc escapes"); return; }
                uo = LLVMGetInstructionOpcode(user);
                if ((uo != LLVMZExt && uo != LLVMSExt)
                    || int_width(user) != 32) {
                    lfail("narrow trunc with non-extend user");
                    return;
                }
            }
            return;  /* VK_SKIP */
        }
        lfail("trunc to unhandled width");
        return;

    case LLVMLoad:
    case LLVMStore:
        {   LLVMValueRef ptr = LLVMGetOperand(in, opc == LLVMLoad ? 0 : 1);
            const char *name;
            size_t len;
            if (!LLVMIsAGlobalVariable(ptr)) {
                lfail("memory access not to an i6 global");
                return;
            }
            name = LLVMGetValueName2(ptr, &len);
            if (!name || !name_has_prefix(name, "i6.g")) {
                lfail("memory access to unknown global");
                return;
            }
            if (opc == LLVMLoad)
                vi->kind = VK_SLOT;
        }
        return;

    case LLVMCall:
        {   const char *name = called_fn_name(in);
            if (!name) { lfail("indirect call"); return; }

            if (strcmp(name, "i6.sym") == 0)
                return;  /* becomes a marked constant operand at each use */

            if (name_has_prefix(name, "llvm.")) {
                if (name_has_prefix(name, "llvm.assume")
                    || name_has_prefix(name, "llvm.lifetime")
                    || name_has_prefix(name, "llvm.dbg")
                    || name_has_prefix(name, "llvm.donothing"))
                    return;
                if (name_has_prefix(name, "llvm.smax.i32")
                    || name_has_prefix(name, "llvm.smin.i32")
                    || name_has_prefix(name, "llvm.umax.i32")
                    || name_has_prefix(name, "llvm.umin.i32")
                    || name_has_prefix(name, "llvm.abs.i32")) {
                    vi->kind = VK_SLOT;
                    return;
                }
                if (name_has_prefix(name, "llvm.fshl.i32")
                    || name_has_prefix(name, "llvm.fshr.i32")) {
                    /* Funnel shift (rotate, when both inputs coincide):
                       constant counts lower as shift/shift/or. */
                    LLVMValueRef c = LLVMGetOperand(in, 2);
                    if (!LLVMIsAConstantInt(c)) {
                        lfail("funnel shift by non-constant");
                        return;
                    }
                    vi->kind = VK_SLOT;
                    if (((uint32_t)LLVMConstIntGetZExtValue(c) & 31) != 0)
                        vi->needs_scratch = TRUE;
                    return;
                }
                lfail("unhandled intrinsic");
                return;
            }

            if (strcmp(name, "i6.deref") == 0) {
                vi->kind = VK_SLOT;
                return;
            }
            if (strcmp(name, "i6.deref.store") == 0)
                return;

            if (name_has_prefix(name, "i6.")) {
                /* An opaque Glulx opcode. Validate the name and shape. */
                char base[64];
                size_t blen = strlen(name + 3);
                int has_stack = FALSE;
                int32 opnum;
                int flags, opct, n_src, nargs;

                if (blen >= sizeof(base)) { lfail("opcode name too long"); return; }
                strcpy(base, name + 3);
                if (blen > 2 && strcmp(base + blen - 2, ".s") == 0) {
                    has_stack = TRUE;
                    base[blen - 2] = 0;
                }
                opnum = glulx_opcode_by_name(base);
                if (opnum < 0) { lfail("unknown opcode call"); return; }
                flags = glulx_opcode_flags(opnum);
                opct  = glulx_opcode_operand_count(opnum);
                if (flags & OPFLAG_STORE2) { lfail("two-store opcode"); return; }
                if (flags & OPFLAG_BRANCH) { lfail("branch opcode call"); return; }
                n_src = opct - ((flags & OPFLAG_STORE) ? 1 : 0);
                nargs = (int)LLVMGetNumArgOperands(in);
                if (has_stack ? (nargs < n_src) : (nargs != n_src)) {
                    lfail("opcode call arg count mismatch");
                    return;
                }
                if ((flags & OPFLAG_STORE) && LLVMGetFirstUse(in))
                    vi->kind = VK_SLOT;
                return;  /* VK_SKIP if result unused or void */
            }

            lfail("call to unknown function");
        }
        return;

    case LLVMAlloca:
        lfail("alloca survived optimization");
        return;

    default:
        lfail("unhandled IR instruction");
        return;
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 2: validate operands                                      */
/* ------------------------------------------------------------------------- */

/* Can this value be turned into an assembly_operand at a use site? */
static int operand_ok(LLVMValueRef v)
{
    valinfo *vi;
    if (LLVMIsAConstantInt(v)) return int_width(v) <= 32;
    if (LLVMIsUndef(v) || LLVMIsPoison(v)) return TRUE;
    if (is_sym_call(v)) return TRUE;
    if (LLVMIsAArgument(v)) return param_index(v) >= 0;
    if (!LLVMIsAInstruction(v)) return FALSE;
    vi = underlying(v);
    return vi && vi->kind == VK_SLOT;
}

static void check_operand(LLVMValueRef v, const char *what)
{
    if (!operand_ok(v)) lfail(what);
}

/* i6.deref addresses must be encodable in a DEREFERENCE_OT operand:
   a plain constant or a symbolic (marked) one. */
static void check_deref_addr(LLVMValueRef v)
{
    if (LLVMIsAConstantInt(v) || LLVMIsUndef(v) || LLVMIsPoison(v)
        || is_sym_call(v))
        return;
    lfail("computed deref address");
}

static void validate(LLVMValueRef in)
{
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);
    unsigned i, n;

    switch (opc) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMUDiv: case LLVMURem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
    case LLVMICmp:
        check_operand(LLVMGetOperand(in, 0), "unlowerable operand");
        check_operand(LLVMGetOperand(in, 1), "unlowerable operand");
        return;

    case LLVMSelect:
        {   LLVMValueRef cond = LLVMGetOperand(in, 0);
            valinfo *ci = lookup(cond);
            if (!(ci && ci->kind == VK_FUSED)
                && !LLVMIsAConstantInt(cond)
                && !LLVMIsUndef(cond) && !LLVMIsPoison(cond))
                check_operand(cond, "unlowerable select condition");
            check_operand(LLVMGetOperand(in, 1), "unlowerable operand");
            check_operand(LLVMGetOperand(in, 2), "unlowerable operand");
        }
        return;

    case LLVMPHI:
        n = LLVMCountIncoming(in);
        for (i = 0; i < n; i++)
            check_operand(LLVMGetIncomingValue(in, i),
                "unlowerable phi input");
        return;

    case LLVMZExt:
    case LLVMSExt:
        {   LLVMValueRef src = LLVMGetOperand(in, 0);
            int sw = int_width(src);
            if (sw == 1)
                check_operand(src, "unlowerable extend source");
            else {
                /* 8/16-bit: source must be trunc-from-i32 whose input we
                   can read directly. */
                if (!LLVMIsAInstruction(src)
                    || LLVMGetInstructionOpcode(src) != LLVMTrunc) {
                    lfail("narrow extend not fed by trunc");
                    return;
                }
                check_operand(LLVMGetOperand(src, 0),
                    "unlowerable extend source");
            }
        }
        return;

    case LLVMTrunc:
        if (int_width(in) == 1)
            check_operand(LLVMGetOperand(in, 0), "unlowerable trunc source");
        return;

    case LLVMLoad:
        return;
    case LLVMStore:
        check_operand(LLVMGetOperand(in, 0), "unlowerable stored value");
        return;

    case LLVMCall:
        {   const char *name = called_fn_name(in);
            int nargs = (int)LLVMGetNumArgOperands(in);
            int j;
            if (!name) return;
            if (strcmp(name, "i6.sym") == 0)
                return;
            if (name_has_prefix(name, "llvm.assume")
                || name_has_prefix(name, "llvm.lifetime")
                || name_has_prefix(name, "llvm.dbg")
                || name_has_prefix(name, "llvm.donothing"))
                return;
            if (strcmp(name, "i6.deref") == 0) {
                check_deref_addr(LLVMGetOperand(in, 0));
                return;
            }
            if (strcmp(name, "i6.deref.store") == 0) {
                check_deref_addr(LLVMGetOperand(in, 0));
                check_operand(LLVMGetOperand(in, 1),
                    "unlowerable stored value");
                return;
            }
            if (name_has_prefix(name, "llvm.abs.")) {
                check_operand(LLVMGetOperand(in, 0), "unlowerable operand");
                return;
            }
            /* min/max intrinsics and i6.<opcode> calls: all integer args */
            for (j = 0; j < nargs; j++)
                check_operand(LLVMGetOperand(in, j), "unlowerable call arg");
        }
        return;

    case LLVMBr:
        if (LLVMIsConditional(in)) {
            LLVMValueRef cond = LLVMGetCondition(in);
            valinfo *ci = lookup(cond);
            if (!(ci && ci->kind == VK_FUSED)
                && !LLVMIsAConstantInt(cond)
                && !LLVMIsUndef(cond) && !LLVMIsPoison(cond))
                check_operand(cond, "unlowerable branch condition");
        }
        return;

    case LLVMSwitch:
        check_operand(LLVMGetOperand(in, 0), "unlowerable switch value");
        if (LLVMGetNumSuccessors(in) > MAX_STUBS)
            lfail("switch too large");
        return;

    case LLVMRet:
        if (LLVMGetNumOperands(in) != 1) { lfail("void return"); return; }
        check_operand(LLVMGetOperand(in, 0), "unlowerable return value");
        return;

    default:
        return;
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 3: what does each instruction's emission read at          */
/*   runtime? "pops" are reads that consume exactly once, unconditionally,   */
/*   in one emitted instruction — the sites where a VK_STACK value may be    */
/*   popped, in pop order. "other" reads disqualify a value from riding      */
/*   the stack (conditional, repeated, or order-sensitive reads).            */
/* ------------------------------------------------------------------------- */

typedef struct readset_s {
    LLVMValueRef pops[MAX_READS];  int n_pops;
    LLVMValueRef other[MAX_READS]; int n_other;
} readset;

static void rs_pop(readset *rs, LLVMValueRef v)
{
    LLVMValueRef r = rep(v);
    if (!r) return;
    if (rs->n_pops >= MAX_READS) { lfail("readset overflow"); return; }
    rs->pops[rs->n_pops++] = r;
}

static void rs_other(readset *rs, LLVMValueRef v)
{
    LLVMValueRef r = rep(v);
    if (!r) return;
    if (rs->n_other >= MAX_READS) { lfail("readset overflow"); return; }
    rs->other[rs->n_other++] = r;
}

/* Reads made by gen_cond_branch(cond): a fused icmp's operands, or the
   0/1 condition value itself. */
static void rs_cond(readset *rs, LLVMValueRef cond)
{
    valinfo *ci = lookup(cond);
    if (ci && ci->kind == VK_FUSED) {
        rs_pop(rs, LLVMGetOperand(cond, 0));
        rs_pop(rs, LLVMGetOperand(cond, 1));
    }
    else if (!LLVMIsAConstantInt(cond)
             && !LLVMIsUndef(cond) && !LLVMIsPoison(cond))
        rs_pop(rs, cond);
}

static void rs_arm_reads(readset *rs, LLVMValueRef op);

/* Phi-edge copies read the incoming values at the pred's terminator —
   or, for a folded incoming computation, its operands. */
static void rs_succ_phis(readset *rs, LLVMValueRef term)
{
    LLVMBasicBlockRef pred = LLVMGetInstructionParent(term);
    unsigned i, n = LLVMGetNumSuccessors(term);
    for (i = 0; i < n; i++) {
        LLVMBasicBlockRef s = LLVMGetSuccessor(term, i);
        LLVMValueRef phi;
        for (phi = LLVMGetFirstInstruction(s); phi && LLVMIsAPHINode(phi);
             phi = LLVMGetNextInstruction(phi)) {
            LLVMValueRef v = incoming_for(phi, pred);
            valinfo *di = (v && LLVMIsAInstruction(v)) ? lookup(v) : NULL;
            if (di && di->edge_fold == phi)
                rs_arm_reads(rs, v);
            else
                rs_other(rs, v);
        }
    }
}

static void emission_readset_core(LLVMValueRef in, readset *rs);

static void emission_readset(LLVMValueRef in, readset *rs)
{
    valinfo *vi = lookup(in);
    rs->n_pops = 0;
    rs->n_other = 0;
    if (vi && (vi->sunk_into || vi->edge_fold))
        return;   /* emitted (and read) inside its consumer(s) */
    emission_readset_core(in, rs);
}

/* A copy-then-conditional-overwrite select normally initializes with the
   true arm; when the true arm is an immediate and the false arm is a
   computed value the roles swap, betting that the immediate (a wraparound
   sentinel, a default) is the rare case, so the common path pays one copy
   and a branch instead of two copies and a branch. */
static int is_immediate_val(LLVMValueRef v)
{
    return LLVMIsAConstantInt(v) || LLVMIsUndef(v) || LLVMIsPoison(v)
        || is_sym_call(v);
}

static int select_swapped(LLVMValueRef sel)
{
    return is_immediate_val(LLVMGetOperand(sel, 1))
        && !is_immediate_val(LLVMGetOperand(sel, 2));
}

/* The select's operand at idx (1 = true arm, 2 = false arm), if it
   is a computation sunk into the select's own emission. */
static LLVMValueRef sunk_arm(LLVMValueRef sel, int idx)
{
    LLVMValueRef op = LLVMGetOperand(sel, idx);
    valinfo *vi = LLVMIsAInstruction(op) ? lookup(op) : NULL;
    return (vi && vi->sunk_into == sel) ? op : NULL;
}

/* Append everything a sunk arm's emission reads as conditional reads. */
static void rs_arm_reads(readset *rs, LLVMValueRef op)
{
    readset tmp;
    int k;
    emission_readset_core(op, &tmp);
    for (k = 0; k < tmp.n_pops; k++)
        if (rs->n_other < MAX_READS) rs->other[rs->n_other++] = tmp.pops[k];
        else lfail("readset overflow");
    for (k = 0; k < tmp.n_other; k++)
        if (rs->n_other < MAX_READS) rs->other[rs->n_other++] = tmp.other[k];
        else lfail("readset overflow");
}

static void emission_readset_core(LLVMValueRef in, readset *rs)
{
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);
    valinfo *vi = lookup(in);

    rs->n_pops = 0;
    rs->n_other = 0;

    switch (opc) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
        rs_pop(rs, LLVMGetOperand(in, 0));
        rs_pop(rs, LLVMGetOperand(in, 1));
        return;

    case LLVMUDiv: case LLVMURem:
        /* emit_udiv_urem reads the dividend several times */
        rs_other(rs, LLVMGetOperand(in, 0));
        rs_other(rs, LLVMGetOperand(in, 0));
        return;

    case LLVMICmp:
        if (vi && vi->kind == VK_FUSED)
            return;  /* read by the branch/select user */
        rs_pop(rs, LLVMGetOperand(in, 0));
        rs_pop(rs, LLVMGetOperand(in, 1));
        return;

    case LLVMSelect:
        {   LLVMValueRef cond = LLVMGetOperand(in, 0);
            LLVMValueRef tvs = sunk_arm(in, 1), fvs = sunk_arm(in, 2);
            if (LLVMIsAConstantInt(cond)) {
                rs_pop(rs, (const_int_value(cond) & 1)
                    ? LLVMGetOperand(in, 1) : LLVMGetOperand(in, 2));
                return;
            }
            if (LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
                rs_pop(rs, LLVMGetOperand(in, 2));
                return;
            }
            if (tvs || fvs) {
                /* Diamond emission: the branch reads the condition first;
                   both arms are then conditional. */
                rs_cond(rs, cond);
                if (tvs) rs_arm_reads(rs, tvs);
                else rs_other(rs, LLVMGetOperand(in, 1));
                if (fvs) rs_arm_reads(rs, fvs);
                else rs_other(rs, LLVMGetOperand(in, 2));
                return;
            }
            if (select_swapped(in)) {
                rs_pop(rs, LLVMGetOperand(in, 2));   /* fv first */
                rs_cond(rs, cond);
                rs_other(rs, LLVMGetOperand(in, 1));
                return;
            }
            rs_pop(rs, LLVMGetOperand(in, 1));   /* tv, unconditionally */
            rs_cond(rs, cond);
            rs_other(rs, LLVMGetOperand(in, 2)); /* fv, conditionally */
        }
        return;

    case LLVMPHI:
    case LLVMFreeze:
        return;

    case LLVMZExt:
        if (vi && vi->kind != VK_ALIAS)   /* zext(trunc x): reads x */
            rs_pop(rs, LLVMGetOperand(LLVMGetOperand(in, 0), 0));
        return;

    case LLVMSExt:
        {   LLVMValueRef src = LLVMGetOperand(in, 0);
            if (int_width(src) == 1)
                rs_pop(rs, src);
            else
                rs_pop(rs, LLVMGetOperand(src, 0));
        }
        return;

    case LLVMTrunc:
        if (int_width(in) == 1)
            rs_pop(rs, LLVMGetOperand(in, 0));
        return;  /* i8/i16: read by the extending users */

    case LLVMLoad:
        return;
    case LLVMStore:
        rs_pop(rs, LLVMGetOperand(in, 0));
        return;

    case LLVMRet:
        if (LLVMGetNumOperands(in) == 1)
            rs_pop(rs, LLVMGetOperand(in, 0));
        return;

    case LLVMBr:
        /* A branch whose successors coincide is emitted as a plain goto:
           the condition is never read at runtime. */
        if (LLVMIsConditional(in)
            && LLVMGetSuccessor(in, 0) != LLVMGetSuccessor(in, 1))
            rs_cond(rs, LLVMGetCondition(in));
        rs_succ_phis(rs, in);
        return;

    case LLVMSwitch:
        rs_other(rs, LLVMGetOperand(in, 0));  /* read per case */
        rs_succ_phis(rs, in);
        return;

    case LLVMUnreachable:
        return;

    case LLVMCall:
        {   const char *name = called_fn_name(in);
            int nargs, n_src, i;
            if (!name) return;
            if (strcmp(name, "i6.sym") == 0
                || name_has_prefix(name, "llvm.assume")
                || name_has_prefix(name, "llvm.lifetime")
                || name_has_prefix(name, "llvm.dbg")
                || name_has_prefix(name, "llvm.donothing")
                || strcmp(name, "i6.deref") == 0)
                return;
            if (name_has_prefix(name, "llvm.abs.")) {
                rs_other(rs, LLVMGetOperand(in, 0));
                rs_other(rs, LLVMGetOperand(in, 0));
                return;
            }
            if (name_has_prefix(name, "llvm.")) {  /* smax/smin/umax/umin */
                rs_other(rs, LLVMGetOperand(in, 0));
                rs_other(rs, LLVMGetOperand(in, 0));
                rs_other(rs, LLVMGetOperand(in, 1));
                rs_other(rs, LLVMGetOperand(in, 1));
                return;
            }
            if (strcmp(name, "i6.deref.store") == 0) {
                rs_pop(rs, LLVMGetOperand(in, 1));
                return;
            }
            /* opaque i6.<opcode>[.s] call */
            {   char base[64];
                size_t blen = strlen(name + 3);
                int has_stack = FALSE;
                int32 opnum;
                int flags, opct;
                strcpy(base, name + 3);
                if (blen > 2 && strcmp(base + blen - 2, ".s") == 0) {
                    has_stack = TRUE;
                    base[blen - 2] = 0;
                }
                opnum = glulx_opcode_by_name(base);
                flags = glulx_opcode_flags(opnum);
                opct  = glulx_opcode_operand_count(opnum);
                n_src = opct - ((flags & OPFLAG_STORE) ? 1 : 0);
                nargs = (int)LLVMGetNumArgOperands(in);
                if (has_stack) {
                    /* Arg pushes come before the instruction, so a fused
                       operand's pop would find an argument on top. */
                    for (i = 0; i < nargs; i++)
                        rs_other(rs, LLVMGetOperand(in, i));
                }
                else {
                    for (i = 0; i < n_src; i++)
                        rs_pop(rs, LLVMGetOperand(in, i));
                }
            }
        }
        return;

    default:
        /* Conservative: everything read, not fusable. */
        {   int i, n = LLVMGetNumOperands(in);
            for (i = 0; i < n; i++)
                rs_other(rs, LLVMGetOperand(in, i));
        }
        return;
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 4: direct global operands                                 */
/*                                                                           */
/*   A load of an i6 global whose every use happens before anything that     */
/*   could write the global (in the load's own block) doesn't need a copy    */
/*   into a slot: the uses read the GLOBALVAR_OT operand directly.           */
/* ------------------------------------------------------------------------- */

static int global_clobbers(LLVMValueRef in, LLVMValueRef g)
{
    switch (LLVMGetInstructionOpcode(in)) {
    case LLVMStore:
        return LLVMGetOperand(in, 1) == g;
    case LLVMCall:
        {   const char *name = called_fn_name(in);
            if (!name) return TRUE;
            if (strcmp(name, "i6.sym") == 0) return FALSE;
            if (strcmp(name, "i6.deref") == 0) return FALSE;
            if (name_has_prefix(name, "llvm.")) return FALSE;
            /* i6.deref.store and opaque opcodes (call, glk, astore...)
               may write anywhere in RAM, including the globals array. */
            return TRUE;
        }
    default:
        return FALSE;
    }
}

/* No write to g in positions (p1, p2) exclusive. */
static int global_safe_span(LLVMValueRef g, int p1, int p2)
{
    int q;
    for (q = p1 + 1; q < p2; q++)
        if (global_clobbers(vals[q].v, g)) return FALSE;
    return TRUE;
}

static void global_operand_pass(void)
{
    int p;
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        LLVMValueRef g;
        LLVMUseRef use;
        int ok = TRUE;

        if (vi->kind != VK_SLOT) continue;
        if (LLVMGetInstructionOpcode(vi->v) != LLVMLoad) continue;
        g = LLVMGetOperand(vi->v, 0);

        for (use = LLVMGetFirstUse(vi->v); use && ok;
             use = LLVMGetNextUse(use)) {
            LLVMValueRef user = LLVMGetUser(use);
            valinfo *ui;
            if (!LLVMIsAInstruction(user)) { ok = FALSE; break; }
            ui = lookup(user);
            if (!ui) { ok = FALSE; break; }

            if (LLVMIsAPHINode(user)) {
                /* Read by the edge copies at the end of each pred block
                   that feeds this value. */
                unsigned i, n = LLVMCountIncoming(user);
                for (i = 0; i < n && ok; i++) {
                    blockinfo *pb;
                    if (LLVMGetIncomingValue(user, i) != vi->v) continue;
                    pb = block_info(LLVMGetIncomingBlock(user, i));
                    if (!pb || pb != &blkinfo[vi->blk]) { ok = FALSE; break; }
                    ok = global_safe_span(g, vi->pos, pb->end_pos + 1);
                }
            }
            else if (ui->kind == VK_FUSED) {
                /* Read where the icmp's branch/select user emits. */
                LLVMValueRef reader = LLVMGetUser(LLVMGetFirstUse(user));
                valinfo *ri = reader ? lookup(reader) : NULL;
                if (!ri || ri->blk != vi->blk) ok = FALSE;
                else ok = global_safe_span(g, vi->pos, ri->pos);
            }
            else if (ui->kind == VK_ALIAS
                || (ui->kind == VK_SKIP
                    && LLVMGetInstructionOpcode(user) == LLVMTrunc)) {
                /* Forwarded reads; not worth chasing. */
                ok = FALSE;
            }
            else {
                if (ui->blk != vi->blk) ok = FALSE;
                else ok = global_safe_span(g, vi->pos, ui->pos);
            }
        }
        if (ok) vi->kind = VK_GLOBAL;
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 4b: select-arm sinking                                    */
/*                                                                           */
/*   instcombine/simplifycfg speculate cheap arm computations and fold       */
/*   branch diamonds into selects — right for hardware, wrong for an         */
/*   interpreter, where the speculated instruction is a full dispatch on     */
/*   every execution. A pure computation whose only use is one arm of a      */
/*   same-block select is "sunk": emitted inside the select's branch         */
/*   diamond, writing the select's slot directly, so it only runs on the     */
/*   path that needs it.                                                     */
/* ------------------------------------------------------------------------- */

/* Any instruction in (p1, p2) that could write memory an i6.deref might
   read (i.e. anything that writes RAM at all). */
static int memory_clobber_span(int p1, int p2)
{
    int q;
    for (q = p1 + 1; q < p2; q++) {
        LLVMValueRef in = vals[q].v;
        switch (LLVMGetInstructionOpcode(in)) {
        case LLVMStore:
            return TRUE;
        case LLVMCall:
            {   const char *name = called_fn_name(in);
                if (!name) return TRUE;
                if (strcmp(name, "i6.sym") == 0) continue;
                if (strcmp(name, "i6.deref") == 0) continue;
                if (name_has_prefix(name, "llvm.")) continue;
                return TRUE;
            }
        default:
            continue;
        }
    }
    return FALSE;
}

/* Sinking moves the arm's reads from its own position to the select's;
   reads of globals must still see the same values there. */
static int arm_globals_stable(LLVMValueRef op, int from, int to)
{
    unsigned i, n = LLVMGetNumOperands(op);
    for (i = 0; i < n; i++) {
        valinfo *ui = underlying(LLVMGetOperand(op, i));
        if (ui && ui->kind == VK_GLOBAL
            && !global_safe_span(LLVMGetOperand(ui->v, 0), from, to))
            return FALSE;
    }
    return TRUE;
}

/* Shape check: a pure computation emitted as one instruction, whose reads
   would still see the same values if re-evaluated at position to_pos. */
static int pure_op_movable(valinfo *vi, int to_pos)
{
    LLVMValueRef op = vi->v;
    switch (LLVMGetInstructionOpcode(op)) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
        return arm_globals_stable(op, vi->pos, to_pos);
    case LLVMTrunc:
        if (int_width(op) != 1) return FALSE;
        return arm_globals_stable(op, vi->pos, to_pos);
    case LLVMZExt:
    case LLVMSExt:
        {   LLVMValueRef src = LLVMGetOperand(op, 0);
            LLVMValueRef wide = (int_width(src) == 1)
                ? src : LLVMGetOperand(src, 0);
            valinfo *ui = underlying(wide);
            if (ui && ui->kind == VK_GLOBAL
                && !global_safe_span(LLVMGetOperand(ui->v, 0),
                       vi->pos, to_pos))
                return FALSE;
        }
        return TRUE;
    case LLVMLoad:
        return global_safe_span(LLVMGetOperand(op, 0), vi->pos, to_pos);
    case LLVMCall:
        {   const char *name = called_fn_name(op);
            if (!name || strcmp(name, "i6.deref") != 0) return FALSE;
            return !memory_clobber_span(vi->pos, to_pos);
        }
    default:
        return FALSE;   /* multi-emission or side-effecting */
    }
}

static int sinkable_arm(LLVMValueRef sel, valinfo *si, int idx)
{
    LLVMValueRef op = LLVMGetOperand(sel, idx);
    valinfo *vi;
    LLVMUseRef use;

    if (!LLVMIsAInstruction(op)) return FALSE;
    vi = lookup(op);
    if (!vi || vi->kind != VK_SLOT || vi->blk != si->blk) return FALSE;
    use = LLVMGetFirstUse(op);
    if (!use || LLVMGetNextUse(use)) return FALSE;
    return pure_op_movable(vi, si->pos);
}

static void sink_pass(void)
{
    int p;
    for (p = 0; p < n_vals; p++) {
        valinfo *si = &vals[p];
        LLVMValueRef cond;
        if (si->kind != VK_SLOT) continue;
        if (LLVMGetInstructionOpcode(si->v) != LLVMSelect) continue;
        cond = LLVMGetOperand(si->v, 0);
        if (LLVMIsAConstantInt(cond) || LLVMIsUndef(cond)
            || LLVMIsPoison(cond))
            continue;
        if (sinkable_arm(si->v, si, 1)) {
            valinfo *ai = lookup(LLVMGetOperand(si->v, 1));
            ai->sunk_into = si->v;
            ai->kind = VK_SKIP;
        }
        if (sinkable_arm(si->v, si, 2)) {
            valinfo *ai = lookup(LLVMGetOperand(si->v, 2));
            ai->sunk_into = si->v;
            ai->kind = VK_SKIP;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 4c: edge-copy folding                                     */
/*                                                                           */
/*   A pure computation whose only real consumer is a phi becomes the        */
/*   phi's edge copy itself ("add x 1 -> x" at the loop latch instead of     */
/*   "add x 1 -> t; ...; copy t -> x"). If its one other use is a select     */
/*   arm, the select gets a rematerialized clone (sunk, per pass 4b) —       */
/*   undoing GVN's merge of a loop increment with a wraparound select,       */
/*   which saves an instruction on hardware but costs one per iteration     */
/*   on an interpreter.                                                      */
/* ------------------------------------------------------------------------- */

static void edge_fold_pass(void)
{
    int p;
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        LLVMValueRef phi_use = NULL, sel_use = NULL;
        LLVMUseRef use;
        int ok = TRUE;

        if (vi->kind != VK_SLOT || LLVMIsAPHINode(vi->v)) continue;
        if (!pure_op_movable(vi, blkinfo[vi->blk].end_pos + 1)) continue;

        for (use = LLVMGetFirstUse(vi->v); use && ok;
             use = LLVMGetNextUse(use)) {
            LLVMValueRef user = LLVMGetUser(use);
            if (!LLVMIsAInstruction(user)) { ok = FALSE; break; }
            if (LLVMIsAPHINode(user)) {
                /* All entries carrying this value must come from our own
                   block (its operands are re-read on that edge). */
                unsigned i, n = LLVMCountIncoming(user);
                if (phi_use && phi_use != user) { ok = FALSE; break; }
                for (i = 0; i < n; i++) {
                    blockinfo *pb;
                    if (LLVMGetIncomingValue(user, i) != vi->v) continue;
                    pb = block_info(LLVMGetIncomingBlock(user, i));
                    if (!pb || pb != &blkinfo[vi->blk]) { ok = FALSE; break; }
                }
                phi_use = user;
            }
            else if (LLVMGetInstructionOpcode(user) == LLVMSelect
                     && !sel_use) {
                valinfo *si = lookup(user);
                LLVMValueRef cond = LLVMGetOperand(user, 0);
                if (!si || si->kind != VK_SLOT || si->blk != vi->blk
                    || LLVMGetOperand(user, 0) == vi->v
                    || LLVMIsAConstantInt(cond) || LLVMIsUndef(cond)
                    || LLVMIsPoison(cond))
                    ok = FALSE;
                else
                    sel_use = user;
            }
            else
                ok = FALSE;
        }
        if (!ok || !phi_use) continue;

        vi->edge_fold = phi_use;
        vi->kind = VK_SKIP;
        if (sel_use)
            vi->sunk_into = sel_use;   /* rematerialized clone in the arm */
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 5: liveness (block-local live ranges)                     */
/* ------------------------------------------------------------------------- */

static void liveness_pass(void)
{
    int p, k;
    readset rs;

    for (p = 0; p < n_vals; p++) {
        vals[p].last_use = vals[p].pos;
        vals[p].cross = FALSE;
    }
    for (p = 0; p < n_vals && !lower_failed; p++) {
        emission_readset(vals[p].v, &rs);
        for (k = 0; k < rs.n_pops + rs.n_other; k++) {
            LLVMValueRef r = (k < rs.n_pops)
                ? rs.pops[k] : rs.other[k - rs.n_pops];
            valinfo *vi = lookup(r);
            if (!vi) continue;
            if (p > vi->last_use) vi->last_use = p;
            if (vals[p].blk != vi->blk) vi->cross = TRUE;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 6: operand fusion (VK_STACK)                              */
/*                                                                           */
/*   A value written exactly once by a single emitted instruction and        */
/*   consumed exactly once, unconditionally, later in the same block can     */
/*   ride the VM value stack: its def stores to "sp" and its use pops it.    */
/*   A per-block simulation of the pending stack verifies that pushes and    */
/*   pops pair up in LIFO order; anything that doesn't falls back to a       */
/*   slot. Nothing else the lowerer emits touches the stack (opaque .s       */
/*   calls push and pop their own arguments in a balanced way above any      */
/*   pending values).                                                        */
/* ------------------------------------------------------------------------- */

/* Defs whose emission is one instruction whose final operand stores the
   result: safe to redirect into a push. */
static int stackable_def(valinfo *vi)
{
    if (vi->kind != VK_SLOT) return FALSE;
    switch (LLVMGetInstructionOpcode(vi->v)) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
    case LLVMZExt: case LLVMSExt: case LLVMTrunc:
    case LLVMLoad:
        return TRUE;
    case LLVMCall:
        {   const char *name = called_fn_name(vi->v);
            if (!name) return FALSE;
            if (strcmp(name, "i6.deref") == 0) return TRUE;
            if (name_has_prefix(name, "llvm.")) return FALSE; /* min/max/abs */
            if (name_has_prefix(name, "i6.")) return TRUE;    /* opaque store */
            return FALSE;
        }
    default:
        return FALSE;   /* icmp, select, phi, udiv/urem (multi-emission) */
    }
}

/* The instruction whose emission actually reads v, chasing single-use
   forwarders that emit nothing themselves (aliases, narrow truncs, fused
   icmps). NULL if v's use count anywhere along the chain isn't exactly 1. */
static LLVMValueRef effective_reader(LLVMValueRef v)
{
    int guard;
    for (guard = 0; guard < 16; guard++) {
        LLVMUseRef use = LLVMGetFirstUse(v);
        LLVMValueRef user;
        valinfo *ui;
        if (!use || LLVMGetNextUse(use)) return NULL;
        user = LLVMGetUser(use);
        if (!LLVMIsAInstruction(user)) return NULL;
        ui = lookup(user);
        if (!ui) return NULL;
        if (ui->kind == VK_ALIAS || ui->kind == VK_FUSED
            || (ui->kind == VK_SKIP
                && LLVMGetInstructionOpcode(user) == LLVMTrunc)) {
            v = user;
            continue;
        }
        return user;
    }
    return NULL;
}

static int fusion_candidate(valinfo *vi)
{
    LLVMValueRef reader;
    valinfo *ri;
    readset rs;
    int k, in_pops = 0, in_other = 0;

    if (!stackable_def(vi)) return FALSE;
    reader = effective_reader(vi->v);
    if (!reader || LLVMIsAPHINode(reader)) return FALSE;
    ri = lookup(reader);
    if (!ri || ri->blk != vi->blk) return FALSE;

    emission_readset(reader, &rs);
    for (k = 0; k < rs.n_pops; k++)
        if (rs.pops[k] == vi->v) in_pops++;
    for (k = 0; k < rs.n_other; k++)
        if (rs.other[k] == vi->v) in_other++;
    return in_pops == 1 && in_other == 0;
}

static void unfuse(LLVMValueRef v)
{
    valinfo *vi = lookup(v);
    if (vi) vi->kind = VK_SLOT;
}

static void fusion_pass(void)
{
    int bi;
    for (bi = 0; bi < n_blocks && !lower_failed; bi++) {
        LLVMValueRef pend[MAX_PEND];
        int n_pend = 0;
        int j;
        LLVMValueRef in;

        for (in = LLVMGetFirstInstruction(blkinfo[bi].bb); in;
             in = LLVMGetNextInstruction(in)) {
            readset rs;
            valinfo *vi;
            int k;

            emission_readset(in, &rs);
            for (k = 0; k < rs.n_pops; k++) {
                LLVMValueRef r = rs.pops[k];
                if (n_pend && pend[n_pend-1] == r) { n_pend--; continue; }
                for (j = n_pend - 1; j >= 0; j--) {
                    if (pend[j] != r) continue;
                    unfuse(r);   /* pop out of order: give it a slot */
                    memmove(&pend[j], &pend[j+1],
                        (n_pend-j-1) * sizeof(pend[0]));
                    n_pend--;
                    break;
                }
            }
            for (k = 0; k < rs.n_other; k++) {
                LLVMValueRef r = rs.other[k];
                for (j = n_pend - 1; j >= 0; j--) {
                    if (pend[j] != r) continue;
                    unfuse(r);   /* read some other way: give it a slot */
                    memmove(&pend[j], &pend[j+1],
                        (n_pend-j-1) * sizeof(pend[0]));
                    n_pend--;
                    break;
                }
            }

            vi = lookup(in);
            if (vi && n_pend < MAX_PEND && fusion_candidate(vi)) {
                vi->kind = VK_STACK;
                pend[n_pend++] = in;
            }
        }
        /* Anything still pending at block end can't ride the stack. */
        for (j = 0; j < n_pend; j++)
            unfuse(pend[j]);
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 7: block analysis and slot assignment                     */
/* ------------------------------------------------------------------------- */

/* Does the edge copy for phi (on the edge pred->succ) read some *other*
   phi of succ? For a folded computation that means its operands. */
static int edge_source_reads_phi(LLVMValueRef phi, LLVMBasicBlockRef pred,
    LLVMBasicBlockRef succ)
{
    LLVMValueRef v = incoming_for(phi, pred);
    valinfo *di = (v && LLVMIsAInstruction(v)) ? lookup(v) : NULL;
    if (di && di->edge_fold == phi) {
        readset tmp;
        int k;
        emission_readset_core(v, &tmp);
        for (k = 0; k < tmp.n_pops + tmp.n_other; k++) {
            LLVMValueRef r = (k < tmp.n_pops)
                ? tmp.pops[k] : tmp.other[k - tmp.n_pops];
            if (r != phi && LLVMIsAPHINode(r)
                && LLVMGetInstructionParent(r) == succ)
                return TRUE;
        }
        return FALSE;
    }
    {   LLVMValueRef r = rep(v);
        return r && r != phi && LLVMIsAPHINode(r)
            && LLVMGetInstructionParent(r) == succ;
    }
}

/* An edge's phi copies form a parallel assignment; they only need staging
   through scratch slots if some phi's copy reads another phi of the same
   block (whose slot a preceding copy might already have overwritten). */
static int edge_needs_staging(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ)
{
    LLVMValueRef phi;
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi)) {
        if (edge_source_reads_phi(phi, pred, succ))
            return TRUE;
    }
    return FALSE;
}

static void block_analysis(void)
{
    int i;
    for (i = 0; i < n_blocks; i++) {
        LLVMBasicBlockRef bb = blkinfo[i].bb;
        LLVMValueRef phi, in;

        blkinfo[i].hazard = FALSE;
        for (phi = LLVMGetFirstInstruction(bb); phi && LLVMIsAPHINode(phi);
             phi = LLVMGetNextInstruction(phi)) {
            unsigned k, n = LLVMCountIncoming(phi);
            for (k = 0; k < n; k++)
                if (edge_source_reads_phi(phi,
                        LLVMGetIncomingBlock(phi, k), bb))
                    blkinfo[i].hazard = TRUE;
        }

        /* Lone "ret <constant>" blocks: gotos inline the return; branches
           reach a ret 0 / ret 1 via the -3/-4 encodings. */
        blkinfo[i].ret_inline = NULL;
        blkinfo[i].retconst = -1;
        blkinfo[i].emit_body = TRUE;
        in = LLVMGetFirstInstruction(bb);
        if (in && in == LLVMGetBasicBlockTerminator(bb)
            && LLVMGetInstructionOpcode(in) == LLVMRet
            && LLVMGetNumOperands(in) == 1) {
            LLVMValueRef rv = LLVMGetOperand(in, 0);
            if (LLVMIsAConstantInt(rv) || LLVMIsUndef(rv) || LLVMIsPoison(rv)
                || is_sym_call(rv)) {
                blkinfo[i].ret_inline = rv;
                if (LLVMIsAConstantInt(rv)) {
                    int32 c = const_int_value(rv);
                    if (c == 0 || c == 1) blkinfo[i].retconst = c;
                }
                /* Every reference to a ret 0/1 block is inlined, so its
                   body (except the entry block's) is never reached. */
                if (i > 0 && blkinfo[i].retconst >= 0)
                    blkinfo[i].emit_body = FALSE;
            }
        }
    }
}

/* TRUE if some emission in positions [from, to] reads a value that lives
   in slot s. Values that share a slot through coalescing all match, so
   the check stays sound as slots accumulate tenants. */
static int slot_read_in_range(int from, int to, int s)
{
    int q, k;
    readset rs;
    for (q = from; q <= to; q++) {
        emission_readset(vals[q].v, &rs);
        for (k = 0; k < rs.n_pops + rs.n_other; k++) {
            LLVMValueRef r = (k < rs.n_pops)
                ? rs.pops[k] : rs.other[k - rs.n_pops];
            valinfo *ri = lookup(r);
            if (ri && ri->kind == VK_SLOT && ri->slot == s) return TRUE;
        }
    }
    return FALSE;
}

static int block_first_pos(int blk)
{   return blk == 0 ? 0 : blkinfo[blk-1].end_pos + 1;
}

/* Phi-incoming coalescing: a value whose only use is feeding a phi can be
   defined straight into the phi's slot, turning its edge copy into an
   elided self-copy — provided nothing that lives in that slot is still
   read while the new tenant occupies it.

   For an ordinary def the slot is occupied from the def to the end of its
   block (a multi-instruction emission may write its result before reading
   its own operands, so for those the def's own reads count too). For a
   phi feeding another phi, the slot is occupied from the block's entry
   (its predecessors' edge copies write it), so the whole block must be
   clean and no other phi of the block may read the slot through its own
   edge copies. */
static int coalesce_with_phi(valinfo *vi)
{
    LLVMUseRef use = LLVMGetFirstUse(vi->v);
    LLVMValueRef phi;
    valinfo *pi;
    int start;

    if (!use || LLVMGetNextUse(use)) return FALSE;
    phi = LLVMGetUser(use);
    if (!LLVMIsAPHINode(phi) || phi == vi->v) return FALSE;
    pi = lookup(phi);
    if (!pi || pi->kind != VK_SLOT || pi->slot == 0) return FALSE;

    if (LLVMIsAPHINode(vi->v)) {
        LLVMValueRef r;
        if (pi->blk == vi->blk) return FALSE;   /* parallel-copy tangle */
        for (r = LLVMGetFirstInstruction(blkinfo[vi->blk].bb);
             r && LLVMIsAPHINode(r); r = LLVMGetNextInstruction(r)) {
            unsigned k, n;
            valinfo *ri;
            if (r == vi->v) continue;
            ri = lookup(r);
            if (ri && ri->slot == pi->slot) return FALSE;
            n = LLVMCountIncoming(r);
            for (k = 0; k < n; k++) {
                valinfo *ii = underlying(LLVMGetIncomingValue(r, k));
                if (ii && ii->kind == VK_SLOT && ii->slot == pi->slot)
                    return FALSE;
            }
        }
        start = block_first_pos(vi->blk);
    }
    else
        start = stackable_def(vi) ? vi->pos + 1 : vi->pos;

    if (slot_read_in_range(start, blkinfo[vi->blk].end_pos, pi->slot))
        return FALSE;
    vi->slot = pi->slot;
    return TRUE;
}

static void assign_slots(void)
{
    int p, s;
    int pool_base, n_pool = 0;
    int pool_last[MAX_LOWER_SLOTS];

    next_slot = n_params + 1;

    /* Dedicated slots: phis (with staging where hazardous), values that
       cross block boundaries, and udiv/urem scratch temporaries. */
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        if (vi->kind != VK_SLOT) continue;
        if (LLVMIsAPHINode(vi->v)) {
            vi->slot = next_slot++;
            if (blkinfo[vi->blk].hazard) vi->scratch = next_slot++;
        }
        else if (vi->cross)
            vi->slot = next_slot++;
        if (vi->needs_scratch)
            vi->scratch = next_slot++;
    }

    /* Phi-to-phi coalescing: a merge phi whose only use feeds another
       phi (typically a loop latch) takes over that phi's slot, so the
       latch copy elides. */
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        if (vi->kind == VK_SLOT && LLVMIsAPHINode(vi->v))
            coalesce_with_phi(vi);
    }

    /* Block-local values: linear scan over layout order with slot reuse.
       Reuse requires the previous occupant's last use to lie strictly
       before the new def, so a multi-instruction emission that writes its
       result before reading its operands can never clobber an operand. */
    pool_base = next_slot;
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        if (vi->kind != VK_SLOT || vi->cross || LLVMIsAPHINode(vi->v))
            continue;
        if (coalesce_with_phi(vi))
            continue;
        for (s = 0; s < n_pool; s++)
            if (pool_last[s] < vi->pos) break;
        if (s == n_pool) {
            if (pool_base + n_pool > MAX_LOWER_SLOTS) {
                lfail("too many local slots");
                return;
            }
            n_pool++;
        }
        pool_last[s] = vi->last_use;
        vi->slot = pool_base + s;
    }

    next_slot += n_pool;
    if (next_slot - 1 > MAX_LOWER_SLOTS)
        lfail("too many local slots");
}

/* ------------------------------------------------------------------------- */
/*   Phase B: emission                                                       */
/* ------------------------------------------------------------------------- */

static assembly_operand mkop(int type, int32 value)
{
    assembly_operand o;
    o.type = type;
    o.value = value;
    o.marker = 0;
    o.symindex = -1;
    return o;
}

static assembly_operand slot_op(int slot)
{   return mkop(LOCALVAR_OT, slot);
}

static assembly_operand stack_op(void)
{   return mkop(LOCALVAR_OT, 0);
}

static assembly_operand const_op(int32 v)
{
    assembly_operand o = mkop(CONSTANT_OT, 0);
    set_constant_otv(&o, v);
    return o;
}

/* The global variable behind an i6.g<index> pointer, as an operand. */
static assembly_operand global_op(LLVMValueRef ptr)
{
    size_t len;
    const char *name = LLVMGetValueName2(ptr, &len);
    int idx = atoi(name + 4);
    return mkop(GLOBALVAR_OT, MAX_LOCAL_VARIABLES + idx);
}

/* Turn an SSA value into an assembly_operand at a use site. */
static assembly_operand resolve(LLVMValueRef v)
{
    valinfo *vi;
    int pi;

    if (LLVMIsAConstantInt(v)) {
        int w = int_width(v);
        int32 val = const_int_value(v);
        if (w == 1) val &= 1;
        return const_op(val);
    }
    if (LLVMIsUndef(v) || LLVMIsPoison(v))
        return const_op(0);
    if (is_sym_call(v)) {
        assembly_operand o = mkop(CONSTANT_OT,
            const_int_value(LLVMGetOperand(v, 1)));
        o.marker = const_int_value(LLVMGetOperand(v, 0));
        o.symindex = const_int_value(LLVMGetOperand(v, 2));
        return o;
    }
    pi = LLVMIsAArgument(v) ? param_index(v) : -1;
    if (pi >= 0)
        return slot_op(pi + 1);

    vi = underlying(v);
    if (!vi) {
        compiler_error("llvm_lower: operand with no representation");
        return const_op(0);
    }
    switch (vi->kind) {
    case VK_SLOT:   return slot_op(vi->slot);
    case VK_STACK:  return stack_op();
    case VK_GLOBAL: return global_op(LLVMGetOperand(vi->v, 0));
    default:
        compiler_error("llvm_lower: operand with no representation");
        return const_op(0);
    }
}

/* Where a def's emitted instruction writes its result. */
static assembly_operand dst_op(valinfo *vi)
{
    if (vi->kind == VK_STACK) return stack_op();
    return slot_op(vi->slot);
}

static void genop(int internal_number, int count,
    const assembly_operand *ops)
{
    assembly_instruction ai;
    int i;
    memset(&ai, 0, sizeof(ai));
    ai.internal_number = internal_number;
    ai.operand_count = count;
    ai.text = NULL;
    for (i = 0; i < count; i++)
        ai.operand[i] = ops[i];
    llvm_buffer_append_instruction(&ai);
    last_emit_noreturn =
        (glulx_opcode_flags(internal_number) & OPFLAG_RETURNS) != 0;
}

static void gen1(int op, assembly_operand a)
{   genop(op, 1, &a);
}
static void gen2(int op, assembly_operand a, assembly_operand b)
{   assembly_operand ops[2];
    ops[0] = a; ops[1] = b;
    genop(op, 2, ops);
}
static void gen3(int op, assembly_operand a, assembly_operand b,
    assembly_operand c)
{   assembly_operand ops[3];
    ops[0] = a; ops[1] = b; ops[2] = c;
    genop(op, 3, ops);
}

static assembly_operand branch_op(int label)
{
    assembly_operand o = mkop(CONSTANT_OT, label);
    o.marker = BRANCH_MV;
    return o;
}

static void gen_jump(int label)
{   gen1(jump_gc, branch_op(label));
}
static void gen_branch1(int op, assembly_operand a, int label)
{   gen2(op, a, branch_op(label));
}
static void gen_branch2(int op, assembly_operand a, assembly_operand b,
    int label)
{   gen3(op, a, b, branch_op(label));
}

static void gen_label(int n)
{
    llvm_buffer_append_label(n);
    last_emit_noreturn = FALSE;
}

static void gen_copy(assembly_operand src, assembly_operand dst)
{
    if (src.type == dst.type && src.value == dst.value && src.marker == 0)
        return;
    gen2(copy_gc, src, dst);
}

/* Map an icmp predicate to the Glulx conditional branch taken when the
   predicate holds. */
static int pred_branch_opcode(LLVMIntPredicate p)
{
    switch (p) {
    case LLVMIntEQ:  return jeq_gc;
    case LLVMIntNE:  return jne_gc;
    case LLVMIntSLT: return jlt_gc;
    case LLVMIntSLE: return jle_gc;
    case LLVMIntSGT: return jgt_gc;
    case LLVMIntSGE: return jge_gc;
    case LLVMIntULT: return jltu_gc;
    case LLVMIntULE: return jleu_gc;
    case LLVMIntUGT: return jgtu_gc;
    case LLVMIntUGE: return jgeu_gc;
    default:
        compiler_error("llvm_lower: unknown icmp predicate");
        return jeq_gc;
    }
}

static LLVMIntPredicate inverse_predicate(LLVMIntPredicate p)
{
    switch (p) {
    case LLVMIntEQ:  return LLVMIntNE;
    case LLVMIntNE:  return LLVMIntEQ;
    case LLVMIntSLT: return LLVMIntSGE;
    case LLVMIntSGE: return LLVMIntSLT;
    case LLVMIntSGT: return LLVMIntSLE;
    case LLVMIntSLE: return LLVMIntSGT;
    case LLVMIntULT: return LLVMIntUGE;
    case LLVMIntUGE: return LLVMIntULT;
    case LLVMIntUGT: return LLVMIntULE;
    case LLVMIntULE: return LLVMIntUGT;
    default:         return p;
    }
}

/* Emit "branch to <label> if <cond> is true" (or false, when inverted).
   cond is an i1 value: a fusable icmp becomes a native compare-branch,
   anything else is a 0/1 value tested with jnz/jz. Constants are the
   caller's business. */
static void gen_cond_branch(LLVMValueRef cond, int label, int invert)
{
    valinfo *ci = lookup(cond);
    if (ci && ci->kind == VK_FUSED) {
        LLVMIntPredicate p = LLVMGetICmpPredicate(cond);
        if (invert) p = inverse_predicate(p);
        gen_branch2(pred_branch_opcode(p),
            resolve(LLVMGetOperand(cond, 0)),
            resolve(LLVMGetOperand(cond, 1)),
            label);
    }
    else
        gen_branch1(invert ? jz_gc : jnz_gc, resolve(cond), label);
}

static void emit_valued_op(LLVMValueRef in, assembly_operand dst);

/* One phi's edge copy: a plain copy, or the folded computation itself. */
static void emit_edge_copy(LLVMValueRef phi, LLVMBasicBlockRef pred,
    assembly_operand dst)
{
    LLVMValueRef v = incoming_for(phi, pred);
    valinfo *di = (v && LLVMIsAInstruction(v)) ? lookup(v) : NULL;
    if (di && di->edge_fold == phi)
        emit_valued_op(v, dst);
    else
        gen_copy(resolve(v), dst);
}

/* Copies for the phis of succ along the edge from pred. Every phi gets
   its own slot; the copies are a parallel assignment, staged through
   scratch slots only when a source is another of succ's phis. */
static void emit_edge_copies(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ)
{
    LLVMValueRef phi;
    int n = phi_count(succ);

    if (n == 0) return;
    if (n == 1 || !edge_needs_staging(pred, succ)) {
        for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
             phi = LLVMGetNextInstruction(phi))
            emit_edge_copy(phi, pred, slot_op(lookup(phi)->slot));
        return;
    }
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi))
        emit_edge_copy(phi, pred, slot_op(lookup(phi)->scratch));
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi))
        gen_copy(slot_op(lookup(phi)->scratch),
            slot_op(lookup(phi)->slot));
}

/* Label to branch to for the edge pred->succ: the successor itself if the
   edge carries no phi copies (with lone ret 0/ret 1 successors reached
   through the -3/-4 "branch means return" encodings), else a fresh stub
   (recorded for flushing after the terminator) that performs the copies
   and jumps on. */
typedef struct edgestub_s {
    int label;
    LLVMBasicBlockRef pred, succ;
} edgestub;

static edgestub stubs[MAX_STUBS];
static int n_stubs;

static int edge_target_label(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ)
{
    blockinfo *sb = block_info(succ);
    if (phi_count(succ) == 0) {
        if (sb->retconst == 0) return -3;
        if (sb->retconst == 1) return -4;
        return sb->label;
    }
    if (n_stubs >= MAX_STUBS) {
        compiler_error("llvm_lower: stub overflow");
        return sb->label;
    }
    stubs[n_stubs].label = alloc_label();
    stubs[n_stubs].pred = pred;
    stubs[n_stubs].succ = succ;
    return stubs[n_stubs++].label;
}

static void flush_stubs(void)
{
    int i;
    for (i = 0; i < n_stubs; i++) {
        gen_label(stubs[i].label);
        emit_edge_copies(stubs[i].pred, stubs[i].succ);
        gen_jump(block_info(stubs[i].succ)->label);
    }
    n_stubs = 0;
}

/* An unconditional transfer to succ: a lone "ret <constant>" successor is
   inlined; otherwise edge copies, then a jump unless succ is the next
   emitted block in layout order. */
static void emit_goto(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ,
    LLVMBasicBlockRef next_bb)
{
    blockinfo *sb = block_info(succ);
    if (sb->ret_inline && phi_count(succ) == 0) {
        gen1(return_gc, resolve(sb->ret_inline));
        return;
    }
    emit_edge_copies(pred, succ);
    if (succ != next_bb)
        gen_jump(sb->label);
}

/* ---- individual instruction emission ------------------------------------ */

static void emit_icmp(LLVMValueRef in, valinfo *vi)
{
    int done = alloc_label();
    gen_copy(const_op(1), slot_op(vi->slot));
    gen_branch2(pred_branch_opcode(LLVMGetICmpPredicate(in)),
        resolve(LLVMGetOperand(in, 0)), resolve(LLVMGetOperand(in, 1)),
        done);
    gen_copy(const_op(0), slot_op(vi->slot));
    gen_label(done);
}

static assembly_operand deref_op(LLVMValueRef addr);

/* Emit a single-instruction computation with an explicit destination
   (used both for ordinary defs and for arms sunk into a select). */
static void emit_valued_op(LLVMValueRef in, assembly_operand dst)
{
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);
    int op;

    switch (opc) {
    case LLVMAdd:  op = add_gc;     break;
    case LLVMSub:  op = sub_gc;     break;
    case LLVMMul:  op = mul_gc;     break;
    case LLVMSDiv: op = div_gc;     break;
    case LLVMSRem: op = mod_gc;     break;
    case LLVMAnd:  op = bitand_gc;  break;
    case LLVMOr:   op = bitor_gc;   break;
    case LLVMXor:  op = bitxor_gc;  break;
    case LLVMShl:  op = shiftl_gc;  break;
    case LLVMLShr: op = ushiftr_gc; break;
    case LLVMAShr: op = sshiftr_gc; break;

    case LLVMZExt:
        {   /* zext(trunc x): mask x down. */
            LLVMValueRef trunc = LLVMGetOperand(in, 0);
            int sw = int_width(trunc);
            gen3(bitand_gc, resolve(LLVMGetOperand(trunc, 0)),
                const_op(sw == 8 ? 0xff : 0xffff), dst);
        }
        return;
    case LLVMSExt:
        {   LLVMValueRef src = LLVMGetOperand(in, 0);
            int sw = int_width(src);
            if (sw == 1)
                /* 0/1 -> 0/-1 */
                gen2(neg_gc, resolve(src), dst);
            else
                /* sext(trunc x): native sign-extend of x's low bits. */
                gen2(sw == 8 ? sexb_gc : sexs_gc,
                    resolve(LLVMGetOperand(src, 0)), dst);
        }
        return;
    case LLVMTrunc:
        gen3(bitand_gc, resolve(LLVMGetOperand(in, 0)), const_op(1), dst);
        return;
    case LLVMLoad:
        gen_copy(global_op(LLVMGetOperand(in, 0)), dst);
        return;
    case LLVMCall:   /* i6.deref */
        gen_copy(deref_op(LLVMGetOperand(in, 0)), dst);
        return;

    default:
        compiler_error("llvm_lower: bad valued op");
        return;
    }
    gen3(op, resolve(LLVMGetOperand(in, 0)), resolve(LLVMGetOperand(in, 1)),
        dst);
}

static void emit_select(LLVMValueRef in, valinfo *vi)
{
    LLVMValueRef cond = LLVMGetOperand(in, 0);
    LLVMValueRef tvs, fvs;
    int done;

    if (LLVMIsAConstantInt(cond)) {
        gen_copy(resolve((const_int_value(cond) & 1)
                ? LLVMGetOperand(in, 1) : LLVMGetOperand(in, 2)),
            slot_op(vi->slot));
        return;
    }
    if (LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
        gen_copy(resolve(LLVMGetOperand(in, 2)), slot_op(vi->slot));
        return;
    }
    tvs = sunk_arm(in, 1);
    fvs = sunk_arm(in, 2);
    if (tvs || fvs) {
        /* Branch diamond: each arm's computation runs only on its path. */
        int arm = alloc_label();
        done = alloc_label();
        if (fvs) {
            gen_cond_branch(cond, arm, TRUE);   /* to fv when false */
            if (tvs) emit_valued_op(tvs, slot_op(vi->slot));
            else gen_copy(resolve(LLVMGetOperand(in, 1)), slot_op(vi->slot));
            gen_jump(done);
            gen_label(arm);
            emit_valued_op(fvs, slot_op(vi->slot));
        }
        else {
            gen_cond_branch(cond, arm, FALSE);  /* to tv when true */
            gen_copy(resolve(LLVMGetOperand(in, 2)), slot_op(vi->slot));
            gen_jump(done);
            gen_label(arm);
            emit_valued_op(tvs, slot_op(vi->slot));
        }
        gen_label(done);
        return;
    }
    done = alloc_label();
    if (select_swapped(in)) {
        gen_copy(resolve(LLVMGetOperand(in, 2)), slot_op(vi->slot));
        gen_cond_branch(cond, done, TRUE);
        gen_copy(resolve(LLVMGetOperand(in, 1)), slot_op(vi->slot));
    }
    else {
        gen_copy(resolve(LLVMGetOperand(in, 1)), slot_op(vi->slot));
        gen_cond_branch(cond, done, FALSE);
        gen_copy(resolve(LLVMGetOperand(in, 2)), slot_op(vi->slot));
    }
    gen_label(done);
}

/* Unsigned division/remainder by a constant. Glulx only has signed
   division, but for 0 <= a,b < 2^31 it agrees with unsigned, so halve
   first: q0 = ((x >>u 1) / c) << 1 is the true quotient or one less,
   and one unsigned compare of r0 = x - q0*c against c corrects it. */
static void emit_udiv_urem(LLVMValueRef in, valinfo *vi)
{
    int is_div = (LLVMGetInstructionOpcode(in) == LLVMUDiv);
    assembly_operand x = resolve(LLVMGetOperand(in, 0));
    uint32_t c = (uint32_t)LLVMConstIntGetZExtValue(LLVMGetOperand(in, 1));
    assembly_operand cop = const_op((int32)c);
    assembly_operand D = slot_op(vi->slot);
    int done;

    if (c == 1) {
        gen_copy(is_div ? x : const_op(0), D);
        return;
    }
    if (c >= 0x80000000u) {
        /* The quotient is 0 or 1; one unsigned compare decides. */
        done = alloc_label();
        if (is_div) {
            gen_copy(const_op(1), D);
            gen_branch2(jgeu_gc, x, cop, done);
            gen_copy(const_op(0), D);
        }
        else {
            gen_copy(x, D);
            gen_branch2(jltu_gc, x, cop, done);
            gen3(sub_gc, x, cop, D);
        }
        gen_label(done);
        return;
    }
    {   assembly_operand T = slot_op(vi->scratch);
        done = alloc_label();
        if (is_div) {
            gen3(ushiftr_gc, x, const_op(1), D);
            gen3(div_gc, D, cop, D);
            gen3(shiftl_gc, D, const_op(1), D);
            gen3(mul_gc, D, cop, T);
            gen3(sub_gc, x, T, T);
            gen_branch2(jltu_gc, T, cop, done);
            gen3(add_gc, D, const_op(1), D);
        }
        else {
            gen3(ushiftr_gc, x, const_op(1), T);
            gen3(div_gc, T, cop, T);
            gen3(shiftl_gc, T, const_op(1), T);
            gen3(mul_gc, T, cop, T);
            gen3(sub_gc, x, T, D);
            gen_branch2(jltu_gc, D, cop, done);
            gen3(sub_gc, D, cop, D);
        }
        gen_label(done);
    }
}

/* DEREFERENCE_OT operand from an i6.deref address argument. */
static assembly_operand deref_op(LLVMValueRef addr)
{
    assembly_operand o;
    if (is_sym_call(addr)) {
        o = mkop(DEREFERENCE_OT, const_int_value(LLVMGetOperand(addr, 1)));
        o.marker = const_int_value(LLVMGetOperand(addr, 0));
        o.symindex = const_int_value(LLVMGetOperand(addr, 2));
        return o;
    }
    if (LLVMIsUndef(addr) || LLVMIsPoison(addr))
        return mkop(DEREFERENCE_OT, 0);
    return mkop(DEREFERENCE_OT, const_int_value(addr));
}

static void emit_minmax(LLVMValueRef in, valinfo *vi, const char *name)
{
    assembly_operand a = resolve(LLVMGetOperand(in, 0));
    assembly_operand b = resolve(LLVMGetOperand(in, 1));
    int done = alloc_label();
    int keep_a_op;

    if (name_has_prefix(name, "llvm.smax.")) keep_a_op = jge_gc;
    else if (name_has_prefix(name, "llvm.smin.")) keep_a_op = jle_gc;
    else if (name_has_prefix(name, "llvm.umax.")) keep_a_op = jgeu_gc;
    else keep_a_op = jleu_gc;

    gen_copy(a, slot_op(vi->slot));
    gen_branch2(keep_a_op, a, b, done);
    gen_copy(b, slot_op(vi->slot));
    gen_label(done);
}

/* Funnel shift with a constant count: fshl(a,b,c) is the top word of
   a:b shifted left by c, fshr the bottom word shifted right. */
static void emit_fsh(LLVMValueRef in, valinfo *vi, int left)
{
    assembly_operand a = resolve(LLVMGetOperand(in, 0));
    assembly_operand b = resolve(LLVMGetOperand(in, 1));
    int32 c = (int32)((uint32_t)LLVMConstIntGetZExtValue(
        LLVMGetOperand(in, 2)) & 31);
    assembly_operand D = slot_op(vi->slot);

    if (c == 0) {
        gen_copy(left ? a : b, D);
        return;
    }
    {   assembly_operand T = slot_op(vi->scratch);
        if (left) {
            gen3(shiftl_gc, a, const_op(c), D);
            gen3(ushiftr_gc, b, const_op(32 - c), T);
        }
        else {
            gen3(ushiftr_gc, b, const_op(c), D);
            gen3(shiftl_gc, a, const_op(32 - c), T);
        }
        gen3(bitor_gc, D, T, D);
    }
}

static void emit_abs(LLVMValueRef in, valinfo *vi)
{
    assembly_operand x = resolve(LLVMGetOperand(in, 0));
    int done = alloc_label();
    gen_copy(x, slot_op(vi->slot));
    gen_branch2(jge_gc, x, const_op(0), done);
    gen2(neg_gc, x, slot_op(vi->slot));
    gen_label(done);
}

/* Reconstruct an opaque i6.<opcode>[.s] call as the original Glulx
   instruction, pushing any stack-passed arguments first. */
static void emit_opaque_call(LLVMValueRef in, valinfo *vi, const char *name)
{
    char base[64];
    size_t blen = strlen(name + 3);
    int has_stack = FALSE;
    int32 opnum;
    int flags, opct, n_src, nargs, i;
    assembly_operand ops[8];

    strcpy(base, name + 3);
    if (blen > 2 && strcmp(base + blen - 2, ".s") == 0) {
        has_stack = TRUE;
        base[blen - 2] = 0;
    }
    opnum = glulx_opcode_by_name(base);
    flags = glulx_opcode_flags(opnum);
    opct  = glulx_opcode_operand_count(opnum);
    n_src = opct - ((flags & OPFLAG_STORE) ? 1 : 0);
    nargs = (int)LLVMGetNumArgOperands(in);

    /* Stack args: the lifter popped them in runtime order (arg n_src was
       the top of the stack), so push them in reverse to rebuild it. */
    if (has_stack)
        for (i = nargs - 1; i >= n_src; i--)
            gen2(copy_gc, resolve(LLVMGetOperand(in, i)), stack_pointer);

    for (i = 0; i < n_src; i++)
        ops[i] = resolve(LLVMGetOperand(in, i));
    if (flags & OPFLAG_STORE)
        ops[n_src] = (vi->kind == VK_SLOT || vi->kind == VK_STACK)
            ? dst_op(vi)
            : mkop(ZEROCONSTANT_OT, 0);   /* discard */
    genop(opnum, opct, ops);
}

static void emit_instruction(LLVMValueRef in)
{
    valinfo *vi = lookup(in);
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);

    if (vi && (vi->sunk_into || vi->edge_fold))
        return;   /* emitted inside its consumer(s) */

    switch (opc) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
        emit_valued_op(in, dst_op(vi));
        return;

    case LLVMUDiv: case LLVMURem:
        emit_udiv_urem(in, vi);
        return;

    case LLVMICmp:
        if (vi->kind == VK_SLOT)
            emit_icmp(in, vi);
        return;  /* VK_FUSED: emitted by the user */

    case LLVMSelect:
        emit_select(in, vi);
        return;

    case LLVMPHI:
        return;  /* filled by edge copies */

    case LLVMFreeze:
        return;  /* alias */

    case LLVMZExt:
        if (vi->kind == VK_SLOT || vi->kind == VK_STACK) {
            /* zext(trunc x): mask x down. */
            LLVMValueRef trunc = LLVMGetOperand(in, 0);
            int sw = int_width(trunc);
            gen3(bitand_gc, resolve(LLVMGetOperand(trunc, 0)),
                const_op(sw == 8 ? 0xff : 0xffff), dst_op(vi));
        }
        return;  /* i1: alias */

    case LLVMSExt:
        {   LLVMValueRef src = LLVMGetOperand(in, 0);
            int sw = int_width(src);
            if (sw == 1)
                /* 0/1 -> 0/-1 */
                gen2(neg_gc, resolve(src), dst_op(vi));
            else
                /* sext(trunc x): native sign-extend of x's low bits. */
                gen2(sw == 8 ? sexb_gc : sexs_gc,
                    resolve(LLVMGetOperand(src, 0)), dst_op(vi));
        }
        return;

    case LLVMTrunc:
        if (int_width(in) == 1)
            gen3(bitand_gc, resolve(LLVMGetOperand(in, 0)), const_op(1),
                dst_op(vi));
        return;  /* i8/i16: handled by extending users */

    case LLVMLoad:
        if (vi->kind == VK_GLOBAL)
            return;  /* uses read the global operand directly */
        gen_copy(global_op(LLVMGetOperand(in, 0)), dst_op(vi));
        return;
    case LLVMStore:
        gen_copy(resolve(LLVMGetOperand(in, 0)),
            global_op(LLVMGetOperand(in, 1)));
        return;

    case LLVMCall:
        {   const char *name = called_fn_name(in);
            if (strcmp(name, "i6.sym") == 0)
                return;
            if (name_has_prefix(name, "llvm.assume")
                || name_has_prefix(name, "llvm.lifetime")
                || name_has_prefix(name, "llvm.dbg")
                || name_has_prefix(name, "llvm.donothing"))
                return;
            if (name_has_prefix(name, "llvm.abs.")) {
                emit_abs(in, vi);
                return;
            }
            if (name_has_prefix(name, "llvm.fshl.")
                || name_has_prefix(name, "llvm.fshr.")) {
                emit_fsh(in, vi, name_has_prefix(name, "llvm.fshl."));
                return;
            }
            if (name_has_prefix(name, "llvm.")) {
                emit_minmax(in, vi, name);
                return;
            }
            if (strcmp(name, "i6.deref") == 0) {
                gen_copy(deref_op(LLVMGetOperand(in, 0)), dst_op(vi));
                return;
            }
            if (strcmp(name, "i6.deref.store") == 0) {
                gen_copy(resolve(LLVMGetOperand(in, 1)),
                    deref_op(LLVMGetOperand(in, 0)));
                return;
            }
            emit_opaque_call(in, vi, name);
        }
        return;

    default:
        compiler_error("llvm_lower: emit of unhandled instruction");
        return;
    }
}

static void emit_terminator(LLVMValueRef term, LLVMBasicBlockRef cur,
    LLVMBasicBlockRef next_bb)
{
    switch (LLVMGetInstructionOpcode(term)) {

    case LLVMRet:
        gen1(return_gc, resolve(LLVMGetOperand(term, 0)));
        return;

    case LLVMUnreachable:
        /* Normally preceded by a noreturn opcode (quit, restart, a tail
           call...); if not, make the impossible path fail loudly instead
           of running off the routine's end. */
        if (!last_emit_noreturn)
            genop(quit_gc, 0, NULL);
        return;

    case LLVMBr:
        if (!LLVMIsConditional(term)) {
            emit_goto(cur, LLVMGetSuccessor(term, 0), next_bb);
            flush_stubs();
            return;
        }
        {   LLVMValueRef cond = LLVMGetCondition(term);
            LLVMBasicBlockRef then_bb = LLVMGetSuccessor(term, 0);
            LLVMBasicBlockRef else_bb = LLVMGetSuccessor(term, 1);

            if (then_bb == else_bb || LLVMIsAConstantInt(cond)
                || LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
                LLVMBasicBlockRef succ = then_bb;
                if (then_bb != else_bb
                    && !(LLVMIsAConstantInt(cond)
                         && (const_int_value(cond) & 1)))
                    succ = else_bb;
                emit_goto(cur, succ, next_bb);
                flush_stubs();
                return;
            }
            if (then_bb == next_bb) {
                /* Fall through into the then block instead of jumping
                   around an else-jump. */
                gen_cond_branch(cond, edge_target_label(cur, else_bb), TRUE);
                emit_goto(cur, then_bb, n_stubs ? NULL : next_bb);
            }
            else {
                gen_cond_branch(cond, edge_target_label(cur, then_bb), FALSE);
                /* If a stub is pending it is emitted right below, in the
                   fallthrough path — so the else edge must jump explicitly. */
                emit_goto(cur, else_bb, n_stubs ? NULL : next_bb);
            }
            flush_stubs();
        }
        return;

    case LLVMSwitch:
        {   assembly_operand cond = resolve(LLVMGetOperand(term, 0));
            LLVMBasicBlockRef defbb = LLVMGetSwitchDefaultDest(term);
            int nops = LLVMGetNumOperands(term);
            int c, ft = -1;

            /* If some case's target is the fallthrough block (and no edge
               needs phi copies), test that case last, inverted: it falls
               through instead of jumping. */
            if (phi_count(defbb) == 0) {
                for (c = 2; c < nops; c += 2) {
                    LLVMBasicBlockRef dest =
                        LLVMValueAsBasicBlock(LLVMGetOperand(term, c + 1));
                    if (phi_count(dest) != 0) { ft = -1; break; }
                    if (dest == next_bb && ft < 0) ft = c;
                }
            }
            for (c = 2; c < nops; c += 2) {
                LLVMBasicBlockRef dest =
                    LLVMValueAsBasicBlock(LLVMGetOperand(term, c + 1));
                if (c == ft) continue;
                gen_branch2(jeq_gc, cond,
                    resolve(LLVMGetOperand(term, c)),
                    edge_target_label(cur, dest));
            }
            if (ft >= 0) {
                gen_branch2(jne_gc, cond,
                    resolve(LLVMGetOperand(term, ft)),
                    edge_target_label(cur, defbb));
                return;   /* falls through into next_bb; no stubs pending */
            }
            emit_goto(cur, defbb, n_stubs ? NULL : next_bb);
            flush_stubs();
        }
        return;

    default:
        compiler_error("llvm_lower: unhandled terminator");
        return;
    }
}

/* ------------------------------------------------------------------------- */
/*   Driver                                                                  */
/* ------------------------------------------------------------------------- */

extern int llvm_lower_routine(LLVMModuleRef m, LLVMValueRef fn,
    const char **fail_reason)
{
    LLVMBasicBlockRef bb;
    LLVMValueRef in;
    int i, total_slots, n_insts;

    (void)m;
    cur_fn = fn;
    lower_failed = FALSE;
    lower_fail_reason = NULL;
    last_emit_noreturn = FALSE;
    n_stubs = 0;
    n_params = (int)LLVMCountParams(fn);
    next_slot = n_params + 1;

    if (n_params != no_locals) {
        /* The lifter defines params = locals; disagreement means a bug. */
        compiler_error("llvm_lower: param count mismatch");
        *fail_reason = "param count mismatch";
        return FALSE;
    }

    /* Count and table up blocks and instructions. */
    n_blocks = 0;
    n_insts = 0;
    for (bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        n_blocks++;
        for (in = LLVMGetFirstInstruction(bb); in;
             in = LLVMGetNextInstruction(in))
            n_insts++;
    }
    if (n_blocks == 0) {
        *fail_reason = "empty function";
        return FALSE;
    }

    vals = my_calloc(sizeof(valinfo), n_insts ? n_insts : 1,
        "llvm lower values");
    blkinfo = my_calloc(sizeof(blockinfo), n_blocks, "llvm lower blocks");
    n_vals = 0;

    i = 0;
    for (bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        blkinfo[i].bb = bb;
        blkinfo[i].label = -1;
        i++;
    }

    /* Phase A. */
    {   int bi = 0;
        for (bb = LLVMGetFirstBasicBlock(fn); bb && !lower_failed;
             bb = LLVMGetNextBasicBlock(bb), bi++) {
            for (in = LLVMGetFirstInstruction(bb); in && !lower_failed;
                 in = LLVMGetNextInstruction(in)) {
                classify(in, &vals[n_vals]);
                vals[n_vals].pos = n_vals;
                vals[n_vals].blk = bi;
                blkinfo[bi].end_pos = n_vals;
                n_vals++;
            }
        }
    }
    if (!lower_failed) {
        for (bb = LLVMGetFirstBasicBlock(fn); bb && !lower_failed;
             bb = LLVMGetNextBasicBlock(bb))
            for (in = LLVMGetFirstInstruction(bb); in && !lower_failed;
                 in = LLVMGetNextInstruction(in))
                validate(in);
    }
    if (!lower_failed) global_operand_pass();
    if (!lower_failed) sink_pass();
    if (!lower_failed) edge_fold_pass();
    if (!lower_failed) liveness_pass();
    if (!lower_failed) fusion_pass();
    if (!lower_failed) block_analysis();
    if (!lower_failed) assign_slots();

    if (lower_failed) {
        *fail_reason = lower_fail_reason;
        my_free(&vals, "llvm lower values");
        my_free(&blkinfo, "llvm lower blocks");
        return FALSE;
    }

    /* Phase B. Patch the header first: if the holding area isn't in the
       expected state this fails without side effects and we fall back. */
    total_slots = next_slot - 1;
    if (total_slots < no_locals) total_slots = no_locals;
    if (!llvm_patch_routine_locals(total_slots)) {
        *fail_reason = "routine header not patchable";
        my_free(&vals, "llvm lower values");
        my_free(&blkinfo, "llvm lower blocks");
        return FALSE;
    }

    llvm_buffer_reset();
    for (i = 1; i < n_blocks; i++)
        blkinfo[i].label = alloc_label();

    for (i = 0; i < n_blocks; i++) {
        LLVMBasicBlockRef next_bb = NULL;
        int j;
        if (!blkinfo[i].emit_body)
            continue;
        for (j = i + 1; j < n_blocks; j++)
            if (blkinfo[j].emit_body) { next_bb = blkinfo[j].bb; break; }
        bb = blkinfo[i].bb;
        if (i > 0)
            gen_label(blkinfo[i].label);
        for (in = LLVMGetFirstInstruction(bb); in;
             in = LLVMGetNextInstruction(in)) {
            if (in == LLVMGetBasicBlockTerminator(bb))
                emit_terminator(in, bb, next_bb);
            else
                emit_instruction(in);
        }
    }

    my_free(&vals, "llvm lower values");
    my_free(&blkinfo, "llvm lower blocks");
    return TRUE;
}
