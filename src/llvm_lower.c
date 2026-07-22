/* ------------------------------------------------------------------------- */
/*   "llvm_lower" : Lowering optimized LLVM IR back to Glulx instructions    */
/*                                                                           */
/*   Part of inform6-llvm. Not part of the upstream Inform 6 compiler.       */
/*                                                                           */
/*   Direct code generation (llvm_codegen.c) builds each routine as IR;      */
/*   after LLVM's passes and legalization, this module translates the IR     */
/*   back into Glulx instructions by filling the output buffer in asm.c,     */
/*   which is then replayed through the classic encoder (so branch           */
/*   shortening and backpatch markers are handled by existing machinery).    */
/*                                                                           */
/*   This is a plain translator: all optimization happens at the IR level    */
/*   before the function arrives here. Every SSA value lives in a Glulx      */
/*   local slot; block-local values share a pool of slots by linear scan,    */
/*   values that cross blocks (and phis) get dedicated slots. The only      */
/*   shape decision made here is fusing a comparison into the conditional    */
/*   branch or select that is its single same-block consumer, because a     */
/*   fused compare-branch is the natural Glulx form of a conditional.        */
/*                                                                           */
/*   Lowering runs in two phases:                                            */
/*     A. classify + validate + slot assignment: no side effects on the      */
/*        compiler state, so an unsupported construct can still fall back    */
/*        to the classic replay;                                             */
/*     B. emit: cannot fail; rewrites the capture buffer.                    */
/* ------------------------------------------------------------------------- */

#include "header.h"
#include "llvm_codegen.h"

#include <llvm-c/Core.h>

/* The most local slots a lowered routine may use. The locals format and
   operand encodings go well beyond this (two-byte frame offsets, multiple
   format pairs); 250 keeps frames within one format pair. */
#define MAX_LOWER_SLOTS 250

/* ------------------------------------------------------------------------- */
/*   Lowering state                                                          */
/* ------------------------------------------------------------------------- */

typedef enum valkind_e {
    VK_SLOT,        /* value lives in a Glulx local slot                    */
    VK_FUSED,       /* icmp folded into its single branch/select user       */
    VK_ALIAS,       /* same runtime representation as another value         */
    VK_SKIP         /* no runtime representation (i6.sym calls, void        */
                    /*   calls, stores, narrow truncs, terminators...)      */
} valkind;

typedef struct valinfo_s {
    LLVMValueRef v;
    valkind kind;
    int slot;               /* VK_SLOT */
    int scratch;            /* phi: parallel-copy staging; udiv/urem and    */
                            /*   funnel shifts: a temporary                 */
    int needs_scratch;
    LLVMValueRef alias_to;  /* VK_ALIAS */
    int pos;                /* linear position in layout order              */
    int blk;                /* index of containing block                    */
    int last_use;           /* last position whose emission reads the value */
    int cross;              /* some read happens outside the def's block    */
} valinfo;

typedef struct blockinfo_s {
    LLVMBasicBlockRef bb;
    int label;              /* -1 for the entry block                       */
    int hazard;             /* phi copies on some edge need staging         */
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

/* Static instruction counts across all lowered routines, printed with the
   LLVM stats line. */
int llvm_lower_insts_in;
int llvm_lower_insts_out;
static int n_emitted;        /* instructions emitted for this routine       */

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

/* The fused comparison represented by v, or NULL. */
static LLVMValueRef fused_icmp_for(LLVMValueRef v)
{
    valinfo *vi = LLVMIsAInstruction(v) ? underlying(v) : NULL;
    return vi && vi->kind == VK_FUSED && LLVMIsAICmpInst(vi->v)
        ? vi->v : NULL;
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

/* An icmp fuses into its user (becoming a Glulx conditional branch, or
   guarding a select) when it has exactly one use, in its own block, and
   that use is the user's condition. */
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

/* Parse a verbatim-form call name: "i6.custom.<code>.<flags>.<forms>"
   or "i6.raw.<name>.<forms>", optionally suffixed ".br". Returns FALSE
   if the name is not a verbatim call; lfails and returns TRUE with
   *opnum == -2 on a malformed one. For customs *opnum is -1 and the
   code and flags outputs carry the descriptor; for raw known opcodes
   *opnum is the internal number and they come from the table. */
static int parse_verbatim_name(const char *name, int32 *opnum, int32 *code,
    int *flags, char *forms, int *is_branch)
{
    char work[96], *part, *save;
    size_t len;
    int is_custom;

    *opnum = -2;
    if (name_has_prefix(name, "i6.custom."))
        is_custom = TRUE;
    else if (name_has_prefix(name, "i6.raw."))
        is_custom = FALSE;
    else
        return FALSE;
    len = strlen(name);
    if (len >= sizeof(work)) { lfail("verbatim name too long"); return TRUE; }
    strcpy(work, name + (is_custom ? 10 : 7));
    len = strlen(work);
    *is_branch = FALSE;
    if (len > 3 && strcmp(work + len - 3, ".br") == 0) {
        *is_branch = TRUE;
        work[len - 3] = 0;
    }
    part = strtok_r(work, ".", &save);
    if (!part) { lfail("malformed verbatim call"); return TRUE; }
    if (is_custom) {
        *code = atoi(part);
        part = strtok_r(NULL, ".", &save);
        if (!part) { lfail("malformed verbatim call"); return TRUE; }
        *flags = atoi(part);
        *opnum = -1;
    }
    else {
        *opnum = glulx_opcode_by_name(part);
        if (*opnum < 0) { lfail("unknown verbatim opcode"); return TRUE; }
        *code = glulx_opcode_code(*opnum);
        *flags = glulx_opcode_flags(*opnum);
    }
    part = strtok_r(NULL, ".", &save);
    if (part && strlen(part) > 8) { lfail("malformed verbatim call"); return TRUE; }
    strcpy(forms, part ? part : "");
    return TRUE;
}

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

            if (strcmp(name, "i6.stkpush") == 0) {
                /* A symbolic stack value spilled to the real stack. */
                if (LLVMGetNumArgOperands(in) != 1)
                    lfail("stkpush arg count");
                return;
            }
            if (strcmp(name, "i6.stkpop") == 0) {
                if (LLVMGetNumArgOperands(in) != 0)
                    lfail("stkpop arg count");
                if (LLVMGetFirstUse(in))
                    vi->kind = VK_SLOT;
                return;
            }
            if (strcmp(name, "i6.catchtok") == 0) {
                /* Emitted by its paired i6.catchflag as the catch's
                   store operand; a throw rewrites the slot in place. */
                if (LLVMGetNumArgOperands(in) != 0)
                    lfail("catchtok arg count");
                if (LLVMGetFirstUse(in))
                    vi->kind = VK_SLOT;
                return;
            }
            if (strcmp(name, "i6.catchflag") == 0) {
                if (LLVMGetNumArgOperands(in) != 1)
                    lfail("catchflag arg count");
                if (LLVMGetFirstUse(in))
                    vi->kind = VK_SLOT;
                return;
            }
            {   int32 vopnum, vcode;
                int vflags, vbr;
                char vforms[10];
                if (parse_verbatim_name(name, &vopnum, &vcode, &vflags,
                    vforms, &vbr)) {
                    int expect = 0, j;
                    if (vopnum == -2) return;  /* lfailed */
                    for (j = 0; vforms[j]; j++)
                        if (vforms[j] == 'v' || vforms[j] == 'g')
                            expect++;
                    if ((int)LLVMGetNumArgOperands(in) != expect) {
                        lfail("verbatim call arg count mismatch");
                        return;
                    }
                    if (LLVMGetFirstUse(in))
                        vi->kind = VK_SLOT;
                    return;
                }
            }
            if (name_has_prefix(name, "i6.")) {
                /* An opaque Glulx opcode. Validate the name and shape. */
                char base[64];
                size_t blen = strlen(name + 3);
                int has_stack = FALSE, is_branch = FALSE, is_ss2 = FALSE;
                int32 opnum;
                int flags, opct, n_src, nargs;

                if (blen >= sizeof(base)) { lfail("opcode name too long"); return; }
                strcpy(base, name + 3);
                if (blen > 4 && strcmp(base + blen - 4, ".ss2") == 0) {
                    is_ss2 = TRUE;
                    base[blen - 4] = 0;
                }
                else if (blen > 3 && strcmp(base + blen - 3, ".br") == 0) {
                    is_branch = TRUE;
                    base[blen - 3] = 0;
                }
                else if (blen > 2 && strcmp(base + blen - 2, ".s") == 0) {
                    has_stack = TRUE;
                    base[blen - 2] = 0;
                }
                opnum = glulx_opcode_by_name(base);
                if (opnum < 0) { lfail("unknown opcode call"); return; }
                flags = glulx_opcode_flags(opnum);
                opct  = glulx_opcode_operand_count(opnum);
                if (!is_ss2 != !(flags & OPFLAG_STORE2)) {
                    lfail("two-store opcode");
                    return;
                }
                if (!is_branch != !(flags & OPFLAG_BRANCH)) {
                    lfail("branch opcode call");
                    return;
                }
                n_src = opct - ((flags & OPFLAG_BRANCH) ? 1 : 0);
                if (is_ss2)
                    n_src -= 2;
                else if (flags & OPFLAG_STORE)
                    n_src -= 1;
                nargs = (int)LLVMGetNumArgOperands(in);
                if (has_stack ? (nargs < n_src) : (nargs != n_src)) {
                    lfail("opcode call arg count mismatch");
                    return;
                }
                if (!is_ss2 && (flags & (OPFLAG_STORE | OPFLAG_BRANCH))
                    && LLVMGetFirstUse(in))
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
    if (LLVMIsAGlobalVariable(v)) {
        /* A verbatim-form call passing an i6 global by reference. */
        size_t len;
        const char *name = LLVMGetValueName2(v, &len);
        return name && name_has_prefix(name, "i6.g");
    }
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
            if (!fused_icmp_for(cond)
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
            if (!fused_icmp_for(cond)
                && !LLVMIsAConstantInt(cond)
                && !LLVMIsUndef(cond) && !LLVMIsPoison(cond))
                check_operand(cond, "unlowerable branch condition");
        }
        return;

    case LLVMSwitch:
        check_operand(LLVMGetOperand(in, 0), "unlowerable switch value");
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
/*   Phase A, pass 3: liveness                                               */
/*                                                                           */
/*   For each value, the last position whose emission reads it (for pool     */
/*   slot reuse) and whether any read happens outside its block (which       */
/*   forces a dedicated slot). A fused comparison's operands are read at     */
/*   the consumer that emits the compare-branch; reads through aliases and   */
/*   narrow truncs land on the underlying value; a phi reads its incoming    */
/*   values at the end of each predecessor block.                            */
/* ------------------------------------------------------------------------- */

static void note_read(LLVMValueRef v, int pos, int blk)
{
    valinfo *vi;
    if (!v || !LLVMIsAInstruction(v)) return;
    vi = underlying(v);
    if (!vi) return;
    if (vi->kind == VK_FUSED) {
        note_read(LLVMGetOperand(vi->v, 0), pos, blk);
        note_read(LLVMGetOperand(vi->v, 1), pos, blk);
        return;
    }
    if (vi->kind == VK_SKIP
        && LLVMGetInstructionOpcode(vi->v) == LLVMTrunc) {
        /* Narrow trunc: its extending users read the trunc's input. */
        note_read(LLVMGetOperand(vi->v, 0), pos, blk);
        return;
    }
    if (pos > vi->last_use) vi->last_use = pos;
    if (blk != vi->blk) vi->cross = TRUE;
}

static void liveness_pass(void)
{
    int p;
    unsigned i, n;

    for (p = 0; p < n_vals; p++) {
        vals[p].last_use = vals[p].pos;
        vals[p].cross = FALSE;
    }
    for (p = 0; p < n_vals; p++) {
        LLVMValueRef in = vals[p].v;
        if (LLVMIsAPHINode(in)) {
            n = LLVMCountIncoming(in);
            for (i = 0; i < n; i++) {
                blockinfo *pb = block_info(LLVMGetIncomingBlock(in, i));
                if (pb)
                    note_read(LLVMGetIncomingValue(in, i),
                        pb->end_pos, (int)(pb - blkinfo));
            }
            continue;
        }
        n = LLVMGetNumOperands(in);
        for (i = 0; i < n; i++)
            note_read(LLVMGetOperand(in, i), vals[p].pos, vals[p].blk);
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 4: block analysis and slot assignment                     */
/* ------------------------------------------------------------------------- */

/* Does the edge copy for phi (on the edge from pred) read some *other*
   phi of the same block? */
static int edge_source_reads_phi(LLVMValueRef phi, LLVMBasicBlockRef pred,
    LLVMBasicBlockRef succ)
{
    valinfo *ri = underlying(incoming_for(phi, pred));
    return ri && ri->v != phi && LLVMIsAPHINode(ri->v)
        && LLVMGetInstructionParent(ri->v) == succ;
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
        LLVMValueRef phi;

        blkinfo[i].hazard = FALSE;
        for (phi = LLVMGetFirstInstruction(bb); phi && LLVMIsAPHINode(phi);
             phi = LLVMGetNextInstruction(phi)) {
            unsigned k, n = LLVMCountIncoming(phi);
            for (k = 0; k < n; k++)
                if (edge_source_reads_phi(phi,
                        LLVMGetIncomingBlock(phi, k), bb))
                    blkinfo[i].hazard = TRUE;
        }
    }
}

static void assign_slots(void)
{
    int p, s;
    int pool_base, n_pool = 0;
    int pool_last[MAX_LOWER_SLOTS];

    next_slot = n_params + 1;

    /* Dedicated slots: phis (with staging where hazardous), values that
       cross block boundaries, and multi-instruction scratch temporaries. */
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        if (vi->kind == VK_SLOT) {
            if (LLVMIsAPHINode(vi->v)) {
                vi->slot = next_slot++;
                if (blkinfo[vi->blk].hazard) vi->scratch = next_slot++;
            }
            else if (vi->cross)
                vi->slot = next_slot++;
        }
        if (vi->needs_scratch)
            vi->scratch = next_slot++;
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
    if (LLVMIsAGlobalVariable(v))
        return global_op(v);
    if (is_sym_call(v)) {
        int32 marker   = const_int_value(LLVMGetOperand(v, 0));
        int32 value    = const_int_value(LLVMGetOperand(v, 1));
        int32 symindex = const_int_value(LLVMGetOperand(v, 2));
        assembly_operand o;
        /* An internal-routine address (IROUTINE_MV) is deferred through the
           routine symbol's own value rather than baked here, so end-of-pass
           address assignment can set it after this operand is emitted:
           emit SYMBOL_MV carrying the symbol index, exactly as the front
           end does for replaced routines. Final backpatch reads
           symbols[symindex].value and applies its IROUTINE_MV marker.
           Veneer refs (VROUTINE_MV) resolve from veneer_routine_address[]
           at end of pass, and anything without a symbol stays as-is. */
        if (marker == IROUTINE_MV && symindex >= 0) {
            o = mkop(CONSTANT_OT, symindex);
            o.marker = SYMBOL_MV;
        } else {
            o = mkop(CONSTANT_OT, value);
            o.marker = marker;
        }
        o.symindex = symindex;
        return o;
    }
    pi = LLVMIsAArgument(v) ? param_index(v) : -1;
    if (pi >= 0)
        return slot_op(pi + 1);

    vi = underlying(v);
    if (!vi || vi->kind != VK_SLOT) {
        compiler_error("llvm_lower: operand with no representation");
        return const_op(0);
    }
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
    n_emitted++;
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
   cond is an i1 value: a fused icmp becomes a native compare-branch,
   anything else is a 0/1 value tested with jnz/jz. Constants are the
   caller's business. */
static void gen_cond_branch(LLVMValueRef cond, int label, int invert)
{
    LLVMValueRef cmp = fused_icmp_for(cond);
    if (cmp) {
        LLVMIntPredicate p = LLVMGetICmpPredicate(cmp);
        if (invert) p = inverse_predicate(p);
        gen_branch2(pred_branch_opcode(p),
            resolve(LLVMGetOperand(cmp, 0)),
            resolve(LLVMGetOperand(cmp, 1)),
            label);
    }
    else
        gen_branch1(invert ? jz_gc : jnz_gc, resolve(cond), label);
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
            gen_copy(resolve(incoming_for(phi, pred)),
                slot_op(lookup(phi)->slot));
        return;
    }
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi))
        gen_copy(resolve(incoming_for(phi, pred)),
            slot_op(lookup(phi)->scratch));
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi))
        gen_copy(slot_op(lookup(phi)->scratch),
            slot_op(lookup(phi)->slot));
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

static void emit_select(LLVMValueRef in, valinfo *vi)
{
    LLVMValueRef cond = LLVMGetOperand(in, 0);
    assembly_operand dst = slot_op(vi->slot);
    int done;

    if (LLVMIsAConstantInt(cond)) {
        gen_copy(resolve((const_int_value(cond) & 1)
                ? LLVMGetOperand(in, 1) : LLVMGetOperand(in, 2)),
            dst);
        return;
    }
    if (LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
        gen_copy(resolve(LLVMGetOperand(in, 2)), dst);
        return;
    }
    done = alloc_label();
    gen_copy(resolve(LLVMGetOperand(in, 1)), dst);
    gen_cond_branch(cond, done, FALSE);
    gen_copy(resolve(LLVMGetOperand(in, 2)), dst);
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

/* Reconstruct an opaque i6.<opcode>[.s|.br] call as the original Glulx
   instruction, pushing any stack-passed arguments first. A ".br" call
   is a branch opcode materialized as its taken/not-taken 0/1 answer:
   the opcode branches over the value construction. */
static void emit_opaque_call(LLVMValueRef in, valinfo *vi, const char *name)
{
    char base[64];
    size_t blen = strlen(name + 3);
    int has_stack = FALSE, is_branch = FALSE, is_ss2 = FALSE;
    int32 opnum;
    int flags, opct, n_src, nargs, i;
    assembly_operand ops[8];

    strcpy(base, name + 3);
    if (blen > 4 && strcmp(base + blen - 4, ".ss2") == 0) {
        is_ss2 = TRUE;
        base[blen - 4] = 0;
    }
    else if (blen > 3 && strcmp(base + blen - 3, ".br") == 0) {
        is_branch = TRUE;
        base[blen - 3] = 0;
    }
    else if (blen > 2 && strcmp(base + blen - 2, ".s") == 0) {
        has_stack = TRUE;
        base[blen - 2] = 0;
    }
    opnum = glulx_opcode_by_name(base);
    flags = glulx_opcode_flags(opnum);
    opct  = glulx_opcode_operand_count(opnum);
    n_src = opct - ((flags & OPFLAG_BRANCH) ? 1 : 0);
    if (is_ss2)
        n_src -= 2;
    else if (flags & OPFLAG_STORE)
        n_src -= 1;
    nargs = (int)LLVMGetNumArgOperands(in);

    /* Stack args: listed in runtime pop order (arg n_src is the top of
       the stack), so push them in reverse to rebuild it. */
    if (has_stack)
        for (i = nargs - 1; i >= n_src; i--)
            gen2(copy_gc, resolve(LLVMGetOperand(in, i)), stack_pointer);

    for (i = 0; i < n_src; i++)
        ops[i] = resolve(LLVMGetOperand(in, i));
    if (is_ss2) {
        /* Both stores push: first store below, second on top. */
        ops[n_src] = stack_op();
        ops[n_src + 1] = stack_op();
        genop(opnum, opct, ops);
        return;
    }
    if (is_branch) {
        int taken = alloc_label();
        ops[n_src] = branch_op(taken);
        genop(opnum, opct, ops);
        if (vi->kind == VK_SLOT) {
            int done = alloc_label();
            gen_copy(const_op(0), slot_op(vi->slot));
            gen_jump(done);
            gen_label(taken);
            gen_copy(const_op(1), slot_op(vi->slot));
            gen_label(done);
        }
        else
            gen_label(taken);   /* both outcomes converge at once */
        return;
    }
    if (flags & OPFLAG_STORE)
        ops[n_src] = (vi->kind == VK_SLOT)
            ? slot_op(vi->slot)
            : mkop(ZEROCONSTANT_OT, 0);   /* discard */
    genop(opnum, opct, ops);
}

static void emit_instruction(LLVMValueRef in)
{
    valinfo *vi = lookup(in);
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
        if (vi->kind == VK_SLOT) {
            /* zext(trunc x): mask x down. */
            LLVMValueRef trunc = LLVMGetOperand(in, 0);
            int sw = int_width(trunc);
            gen3(bitand_gc, resolve(LLVMGetOperand(trunc, 0)),
                const_op(sw == 8 ? 0xff : 0xffff), slot_op(vi->slot));
        }
        return;  /* i1: alias */

    case LLVMSExt:
        {   LLVMValueRef src = LLVMGetOperand(in, 0);
            int sw = int_width(src);
            if (sw == 1)
                /* 0/1 -> 0/-1 */
                gen2(neg_gc, resolve(src), slot_op(vi->slot));
            else
                /* sext(trunc x): native sign-extend of x's low bits. */
                gen2(sw == 8 ? sexb_gc : sexs_gc,
                    resolve(LLVMGetOperand(src, 0)), slot_op(vi->slot));
        }
        return;

    case LLVMTrunc:
        if (int_width(in) == 1)
            gen3(bitand_gc, resolve(LLVMGetOperand(in, 0)), const_op(1),
                slot_op(vi->slot));
        return;  /* i8/i16: handled by extending users */

    case LLVMLoad:
        gen_copy(global_op(LLVMGetOperand(in, 0)), slot_op(vi->slot));
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
                gen_copy(deref_op(LLVMGetOperand(in, 0)),
                    slot_op(vi->slot));
                return;
            }
            if (strcmp(name, "i6.deref.store") == 0) {
                gen_copy(resolve(LLVMGetOperand(in, 1)),
                    deref_op(LLVMGetOperand(in, 0)));
                return;
            }
            if (strcmp(name, "i6.stkpush") == 0) {
                gen_copy(resolve(LLVMGetOperand(in, 0)), stack_op());
                return;
            }
            if (strcmp(name, "i6.stkpop") == 0) {
                gen_copy(stack_op(), (vi->kind == VK_SLOT)
                    ? slot_op(vi->slot) : zero_operand);
                return;
            }
            {   int32 vopnum, vcode;
                int vflags, vbr;
                char vforms[10];
                if (parse_verbatim_name(name, &vopnum, &vcode, &vflags,
                    vforms, &vbr)) {
                    assembly_operand ops[9];
                    int count, argi = 0, j;
                    count = (int)strlen(vforms) + (vbr ? 1 : 0);
                    for (j = 0; vforms[j]; j++) {
                        switch (vforms[j]) {
                        case 'v':
                            ops[j] = resolve(LLVMGetOperand(in, argi++));
                            break;
                        case 'g':
                            ops[j] = global_op(LLVMGetOperand(in, argi++));
                            break;
                        case 'p':
                            ops[j] = stack_op();
                            break;
                        case 'r':
                            ops[j] = (vi->kind == VK_SLOT)
                                ? slot_op(vi->slot)
                                : mkop(ZEROCONSTANT_OT, 0);
                            break;
                        }
                    }
                    if (vopnum == -1)
                        glulx_set_custom_opcode(vcode, vflags, count);
                    if (vbr) {
                        int taken = alloc_label();
                        ops[count - 1] = branch_op(taken);
                        genop(vopnum, count, ops);
                        if (vi->kind == VK_SLOT) {
                            int done = alloc_label();
                            gen_copy(const_op(0), slot_op(vi->slot));
                            gen_jump(done);
                            gen_label(taken);
                            gen_copy(const_op(1), slot_op(vi->slot));
                            gen_label(done);
                        }
                        else
                            gen_label(taken);
                    }
                    else
                        genop(vopnum, count, ops);
                    return;
                }
            }
            if (strcmp(name, "i6.catchtok") == 0)
                return;  /* the paired i6.catchflag emits the catch */
            if (strcmp(name, "i6.catchflag") == 0) {
                /* catch <token slot> ?taken, then build the 0/1 taken
                   flag; the throw-resume path falls through the catch
                   with the token slot rewritten to the thrown value. */
                assembly_operand ops[2];
                valinfo *tok = underlying(LLVMGetOperand(in, 0));
                int taken = alloc_label();
                if (!tok || tok->kind != VK_SLOT) {
                    lfail("catchflag without token slot");
                    return;
                }
                ops[0] = slot_op(tok->slot);
                ops[1] = branch_op(taken);
                genop(catch_gc, 2, ops);
                if (vi->kind == VK_SLOT) {
                    int done = alloc_label();
                    gen_copy(const_op(0), slot_op(vi->slot));
                    gen_jump(done);
                    gen_label(taken);
                    gen_copy(const_op(1), slot_op(vi->slot));
                    gen_label(done);
                }
                else
                    gen_label(taken);
                return;
            }
            emit_opaque_call(in, vi, name);
        }
        return;

    default:
        compiler_error("llvm_lower: emit of unhandled instruction");
        return;
    }
    gen3(op, resolve(LLVMGetOperand(in, 0)), resolve(LLVMGetOperand(in, 1)),
        slot_op(vi->slot));
}

/* ---- terminator emission ------------------------------------------------ */

/* Emit the phi copies for an edge and transfer control to succ, falling
   through when succ is the next block in layout and nothing (such as a
   pending stub) sits in between. */
static void emit_goto_edge(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ,
    LLVMBasicBlockRef next_bb, int force_jump)
{
    emit_edge_copies(pred, succ);
    if (force_jump || succ != next_bb)
        gen_jump(block_info(succ)->label);
}

static void emit_terminator(LLVMValueRef term, LLVMBasicBlockRef bb,
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
            emit_goto_edge(bb, LLVMGetSuccessor(term, 0), next_bb, FALSE);
            return;
        }
        {   LLVMValueRef cond = LLVMGetCondition(term);
            LLVMBasicBlockRef then_bb = LLVMGetSuccessor(term, 0);
            LLVMBasicBlockRef else_bb = LLVMGetSuccessor(term, 1);
            LLVMBasicBlockRef btarget, gtarget;
            int invert, stub, blabel;

            if (then_bb == else_bb || LLVMIsAConstantInt(cond)
                || LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
                LLVMBasicBlockRef chosen = then_bb;
                if (then_bb != else_bb
                    && LLVMIsAConstantInt(cond)
                    && !(const_int_value(cond) & 1))
                    chosen = else_bb;
                if (then_bb != else_bb
                    && (LLVMIsUndef(cond) || LLVMIsPoison(cond)))
                    chosen = else_bb;
                emit_goto_edge(bb, chosen, next_bb, FALSE);
                return;
            }
            /* Branch on the true edge; fall through / jump on the false
               edge — inverted when the true edge is the natural
               fallthrough. A branch target with phis needs a stub for
               its copies. */
            btarget = then_bb; gtarget = else_bb; invert = FALSE;
            if (then_bb == next_bb && else_bb != next_bb) {
                btarget = else_bb; gtarget = then_bb; invert = TRUE;
            }
            stub = phi_count(btarget) != 0;
            blabel = stub ? alloc_label() : block_info(btarget)->label;
            gen_cond_branch(cond, blabel, invert);
            emit_goto_edge(bb, gtarget, next_bb, stub);
            if (stub) {
                gen_label(blabel);
                emit_edge_copies(bb, btarget);
                gen_jump(block_info(btarget)->label);
            }
        }
        return;

    case LLVMSwitch:
        {   assembly_operand cond = resolve(LLVMGetOperand(term, 0));
            int nops = LLVMGetNumOperands(term);
            int n_cases = (nops - 2) / 2;
            int *stub_label = NULL;
            int c, n_stubs = 0;

            if (n_cases > 0) {
                stub_label = my_calloc(sizeof(int), n_cases,
                    "llvm lower switch stubs");
            }
            for (c = 0; c < n_cases; c++) {
                LLVMBasicBlockRef dest = LLVMGetSuccessor(term, c + 1);
                int target;
                stub_label[c] = -1;
                if (phi_count(dest) != 0) {
                    stub_label[c] = alloc_label();
                    target = stub_label[c];
                    n_stubs++;
                }
                else
                    target = block_info(dest)->label;
                gen_branch2(jeq_gc, cond,
                    resolve(LLVMGetOperand(term, 2 + 2 * c)), target);
            }
            emit_goto_edge(bb, LLVMGetSuccessor(term, 0), next_bb,
                n_stubs > 0);
            for (c = 0; c < n_cases; c++) {
                if (stub_label[c] < 0) continue;
                gen_label(stub_label[c]);
                emit_edge_copies(bb, LLVMGetSuccessor(term, c + 1));
                gen_jump(block_info(LLVMGetSuccessor(term, c + 1))->label);
            }
            if (stub_label)
                my_free(&stub_label, "llvm lower switch stubs");
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
    const char **fail_reason, int *insts_in, int *insts_out)
{
    LLVMBasicBlockRef bb;
    LLVMValueRef in;
    int i, total_slots, n_insts;

    (void)m;
    cur_fn = fn;
    lower_failed = FALSE;
    lower_fail_reason = NULL;
    *insts_in = 0;
    *insts_out = 0;
    last_emit_noreturn = FALSE;
    n_params = (int)LLVMCountParams(fn);
    next_slot = n_params + 1;

    if (n_params != no_locals) {
        /* Direct codegen defines params = locals; disagreement means a bug. */
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
    if (!lower_failed) liveness_pass();
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

    llvm_lower_insts_in += llvm_shadow_instruction_count;
    *insts_in = llvm_shadow_instruction_count;
    n_emitted = 0;

    llvm_buffer_reset();
    for (i = 1; i < n_blocks; i++)
        blkinfo[i].label = alloc_label();

    for (i = 0; i < n_blocks; i++) {
        LLVMBasicBlockRef next_bb =
            (i + 1 < n_blocks) ? blkinfo[i + 1].bb : NULL;
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

    llvm_lower_insts_out += n_emitted;
    *insts_out = n_emitted;

    my_free(&vals, "llvm lower values");
    my_free(&blkinfo, "llvm lower blocks");
    return TRUE;
}
