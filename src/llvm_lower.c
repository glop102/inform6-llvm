/* ------------------------------------------------------------------------- */
/*   "llvm_lower" : Lowering optimized LLVM IR back to Glulx instructions    */
/*                                                                           */
/*   Part of inform6-llvm. Not part of the upstream Inform 6 compiler.       */
/*                                                                           */
/*   Direct code generation (llvm_codegen.c) builds each routine as IR;       */
/*   after LLVM's passes, this module turns the IR back into Glulx            */
/*   instructions by filling the output buffer in asm.c,                     */
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
    LLVMValueRef fold_store;/* store whose target this def writes directly  */
    int pos;                /* linear position in layout order              */
    int blk;                /* index of containing block                    */
    int last_use;           /* last position whose emission reads the value */
    int cross;              /* some read happens outside the def's block    */
} valinfo;

typedef struct blockinfo_s {
    LLVMBasicBlockRef bb;
    int label;              /* -1 for the entry block                       */
    int hazard;             /* phi copies on some edge need staging         */
    LLVMValueRef ret_inline;/* operand of a lone "ret v" (a constant or a   */
                            /*   value readable at every pred), or NULL     */
    int retconst;           /* 0/1 if ret_inline is that constant, else -1  */
    LLVMValueRef ret_phi;   /* phi of a lone "phi; ret phi" block: every    */
                            /*   edge returns its own incoming directly     */
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

/* Statistics: static instruction counts across all lowered routines, as a
   crude "do no harm" measure (printed with the LLVM stats line). */
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

/* TRUE if this call names an i6.<opcode> whose opcode never writes RAM,
   globals, or locals (see llvm_opcode_no_ram_write in llvm_codegen.c). */
static int call_writes_no_ram(const char *name)
{
    char base[64];
    size_t blen;
    if (!name_has_prefix(name, "i6.")) return FALSE;
    blen = strlen(name + 3);
    if (blen >= sizeof(base)) return FALSE;
    strcpy(base, name + 3);
    if (blen > 2 && strcmp(base + blen - 2, ".s") == 0)
        base[blen - 2] = 0;
    return llvm_opcode_no_ram_write(base);
}

/* TRUE if this instruction is an opaque i6.<opcode>.s call (stack-passed
   arguments). *n_src_out gets the count of regular source operands;
   *nargs_out the total argument count. Extras occupy indices
   [n_src, nargs), listed in runtime pop order (first argument first). */
static int stack_call_shape(LLVMValueRef in, int *n_src_out, int *nargs_out)
{
    const char *name;
    char base[64];
    size_t blen;
    int32 opnum;
    int flags;
    if (!LLVMIsACallInst(in)) return FALSE;
    name = called_fn_name(in);
    if (!name || !name_has_prefix(name, "i6.")) return FALSE;
    blen = strlen(name + 3);
    if (blen <= 2 || blen >= sizeof(base)) return FALSE;
    strcpy(base, name + 3);
    if (strcmp(base + blen - 2, ".s") != 0) return FALSE;
    base[blen - 2] = 0;
    opnum = glulx_opcode_by_name(base);
    if (opnum < 0) return FALSE;
    flags = glulx_opcode_flags(opnum);
    *n_src_out = glulx_opcode_operand_count(opnum)
        - ((flags & OPFLAG_STORE) ? 1 : 0);
    *nargs_out = (int)LLVMGetNumArgOperands(in);
    return TRUE;
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

/* The fused comparison represented by v, looking through no-op aliases such
   as freeze. */
static LLVMValueRef fused_icmp_for(LLVMValueRef v)
{
    valinfo *vi = LLVMIsAInstruction(v) ? underlying(v) : NULL;
    return vi && vi->kind == VK_FUSED && LLVMIsAICmpInst(vi->v)
        ? vi->v : NULL;
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

/* Has this store's value def taken over the store's emission (writing the
   store's target directly)? validx is the store's value operand index. */
static int store_is_folded(LLVMValueRef st, int validx)
{
    LLVMValueRef v = LLVMGetOperand(st, validx);
    valinfo *di = (v && LLVMIsAInstruction(v)) ? underlying(v) : NULL;
    return di && di->fold_store == st;
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

/* Is this phi the whole value of a lone "phi; ret phi" block? Such
   blocks are never emitted: every edge to them becomes a return of its
   own incoming value, so the phi has no slot and no edge copies. */
static int is_ret_phi(LLVMValueRef phi)
{
    LLVMBasicBlockRef bb;
    LLVMValueRef term;
    if (!LLVMIsAPHINode(phi)) return FALSE;
    bb = LLVMGetInstructionParent(phi);
    if (LLVMGetFirstInstruction(bb) != phi) return FALSE;
    term = LLVMGetNextInstruction(phi);
    if (!term || term != LLVMGetBasicBlockTerminator(bb)) return FALSE;
    if (LLVMGetInstructionOpcode(term) != LLVMRet) return FALSE;
    return LLVMGetNumOperands(term) == 1 && LLVMGetOperand(term, 0) == phi;
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

/* Values which cannot change while a routine runs. Comparisons of these may
   be re-emitted at every branch consumer instead of materializing a 0/1 slot. */
static int immutable_compare_operand(LLVMValueRef v)
{
    return LLVMIsAConstantInt(v) || LLVMIsUndef(v) || LLVMIsPoison(v)
        || LLVMIsAArgument(v) || is_sym_call(v);
}

/* An icmp can fuse into its user (becoming a Glulx conditional branch or
   guarding a select) when it has exactly one same-block use. A multi-use
   comparison can also be rematerialized when both operands are immutable
   and every use is a branch/select condition. The latter replaces the
   materialization plus N condition tests with the same N direct compares,
   so it is non-worse on every path. */
static int icmp_fusable(LLVMValueRef inst)
{
    LLVMUseRef use = LLVMGetFirstUse(inst);
    LLVMValueRef user;
    if (!use) return FALSE;
    if (LLVMGetNextUse(use)) {
        if (!immutable_compare_operand(LLVMGetOperand(inst, 0))
            || !immutable_compare_operand(LLVMGetOperand(inst, 1)))
            return FALSE;
        for (; use; use = LLVMGetNextUse(use)) {
            user = LLVMGetUser(use);
            if (!LLVMIsAInstruction(user)) return FALSE;
            if (LLVMGetInstructionOpcode(user) == LLVMBr) {
                if (!LLVMIsConditional(user)
                    || LLVMGetCondition(user) != inst) return FALSE;
            }
            else if (LLVMGetInstructionOpcode(user) == LLVMSelect) {
                if (LLVMGetOperand(user, 0) != inst) return FALSE;
            }
            else return FALSE;
        }
        return TRUE;
    }
    user = LLVMGetUser(use);
    if (!LLVMIsAInstruction(user)) return FALSE;
    if (LLVMGetInstructionOpcode(user) == LLVMFreeze) {
        LLVMUseRef fuse = LLVMGetFirstUse(user);
        if (!fuse || LLVMGetNextUse(fuse)) return FALSE;
        user = LLVMGetUser(fuse);
        if (!LLVMIsAInstruction(user)) return FALSE;
    }
    if (LLVMGetInstructionParent(user) != LLVMGetInstructionParent(inst))
        return FALSE;
    switch (LLVMGetInstructionOpcode(user)) {
    case LLVMBr:
        return LLVMIsConditional(user)
            && LLVMGetCondition(user) == inst;
    case LLVMSelect:
        return LLVMGetOperand(user, 0) == inst;
    case LLVMZExt:
        /* ret (zext icmp), all in one block: lowers as a compare-branch
           to the -4 "return 1" target followed by "return 0" (2 emitted
           instructions instead of the 0/1 materialization's 5). */
        {   LLVMUseRef zuse = LLVMGetFirstUse(user);
            LLVMValueRef zuser;
            if (int_width(user) != 32) return FALSE;
            if (!zuse || LLVMGetNextUse(zuse)) return FALSE;
            zuser = LLVMGetUser(zuse);
            if (!LLVMIsAInstruction(zuser)) return FALSE;
            if (LLVMGetInstructionOpcode(zuser) != LLVMRet) return FALSE;
            return LLVMGetInstructionParent(zuser)
                == LLVMGetInstructionParent(inst);
        }
    default:
        return FALSE;
    }
}

/* The fused icmp behind "ret (zext icmp)", or NULL. */
static LLVMValueRef ret_fused_icmp(LLVMValueRef term)
{
    LLVMValueRef rv, c;
    valinfo *ci;
    if (LLVMGetNumOperands(term) != 1) return NULL;
    rv = LLVMGetOperand(term, 0);
    if (!LLVMIsAInstruction(rv)
        || LLVMGetInstructionOpcode(rv) != LLVMZExt) return NULL;
    c = LLVMGetOperand(rv, 0);
    if (!LLVMIsAInstruction(c) || !LLVMIsAICmpInst(c)) return NULL;
    ci = lookup(c);
    return (ci && ci->kind == VK_FUSED) ? c : NULL;
}

/* ---- i1 connective trees ------------------------------------------------ */

#define TREE_NONE 0
#define TREE_AND  1
#define TREE_OR   2
#define TREE_NOT  3

/* Recognize the i1 connective shapes instcombine/simplifycfg build when
   they fold source-level short-circuit branches: plain and/or, the
   poison-safe "logical" select forms select(c,x,false) == c && x and
   select(c,true,x) == c || x, and xor-with-true negation. Children come
   back in evaluation (short-circuit) order. */
static int tree_node_shape(LLVMValueRef v, LLVMValueRef *a, LLVMValueRef *b)
{
    LLVMOpcode o;
    if (!LLVMIsAInstruction(v) || int_width(v) != 1) return TREE_NONE;
    o = LLVMGetInstructionOpcode(v);
    if (o == LLVMAnd || o == LLVMOr) {
        *a = LLVMGetOperand(v, 0);
        *b = LLVMGetOperand(v, 1);
        return o == LLVMAnd ? TREE_AND : TREE_OR;
    }
    if (o == LLVMXor) {
        LLVMValueRef x = LLVMGetOperand(v, 1);
        if (LLVMIsAConstantInt(x)
            && (LLVMConstIntGetZExtValue(x) & 1) == 1) {
            *a = LLVMGetOperand(v, 0);
            *b = NULL;
            return TREE_NOT;
        }
        return TREE_NONE;
    }
    if (o == LLVMSelect) {
        LLVMValueRef tv = LLVMGetOperand(v, 1);
        LLVMValueRef fv = LLVMGetOperand(v, 2);
        if (LLVMIsAConstantInt(fv)
            && (LLVMConstIntGetZExtValue(fv) & 1) == 0) {
            *a = LLVMGetOperand(v, 0); *b = tv;
            return TREE_AND;
        }
        if (LLVMIsAConstantInt(tv)
            && (LLVMConstIntGetZExtValue(tv) & 1) == 1) {
            *a = LLVMGetOperand(v, 0); *b = fv;
            return TREE_OR;
        }
        return TREE_NONE;
    }
    return TREE_NONE;
}

/* A connective marked (by branch_tree_pass) as emitted inside the given
   conditional branch, as a chain of short-circuit compare-branches. */
static int is_marked_tree_node(LLVMValueRef v, LLVMValueRef term)
{
    LLVMValueRef a, b;
    valinfo *vi = LLVMIsAInstruction(v) ? lookup(v) : NULL;
    return vi && vi->kind == VK_SKIP && vi->sunk_into == term
        && tree_node_shape(v, &a, &b) != TREE_NONE;
}

/* The select fused into this return (marked by ret_select_pass with
   sunk_into = the ret), or NULL. "ret (select c, K1, K2)" emits as a
   conditional branch between two returns — via the -3/-4 encodings when
   an arm is 0 or 1 — instead of the select's copy/branch/copy diamond
   plus a return. */
static LLVMValueRef ret_fused_select(LLVMValueRef term)
{
    LLVMValueRef rv;
    valinfo *si;
    if (LLVMGetNumOperands(term) != 1) return NULL;
    rv = LLVMGetOperand(term, 0);
    if (!LLVMIsAInstruction(rv)
        || LLVMGetInstructionOpcode(rv) != LLVMSelect) return NULL;
    si = lookup(rv);
    return (si && si->sunk_into == term) ? rv : NULL;
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
    vi->fold_store = NULL;

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

/* Every condition throughout a fused return select chain must be
   emittable as a conditional branch: a fused compare or a readable
   value. Immediate arms need no storage; chained selects recurse. */
static void validate_ret_select(LLVMValueRef sel, LLVMValueRef term)
{
    LLVMValueRef cond = LLVMGetOperand(sel, 0);
    int i;
    if (!is_marked_tree_node(cond, sel) && !fused_icmp_for(cond))
        check_operand(cond, "unlowerable return condition");
    for (i = 1; i <= 2; i++) {
        LLVMValueRef a = LLVMGetOperand(sel, i);
        valinfo *ai = LLVMIsAInstruction(a) ? lookup(a) : NULL;
        if (ai && ai->kind == VK_SKIP && ai->sunk_into == term)
            validate_ret_select(a, term);
    }
}

static void validate(LLVMValueRef in)
{
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);
    unsigned i, n;

    /* Nodes already decided (before validation) to emit inside their
       consumer — ret-selects and connective trees (whose consumer may
       itself be a select) — are read through their leaves, which are
       validated on their own. */
    {   valinfo *vi = lookup(in);
        if (vi && vi->kind == VK_SKIP && vi->sunk_into
            && (LLVMIsATerminatorInst(vi->sunk_into)
                || is_marked_tree_node(in, vi->sunk_into)))
            return;
    }

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
            LLVMValueRef fc = fused_icmp_for(cond);
            if (!is_marked_tree_node(cond, in)
                && !fc
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
            if (sw == 1) {
                valinfo *si = lookup(src);
                /* A lone ret(zext icmp) is emitted directly by the return
                   terminator and needs no readable comparison slot. */
                if (!(si && si->kind == VK_FUSED))
                    check_operand(src, "unlowerable extend source");
            }
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
            LLVMValueRef fc = fused_icmp_for(cond);
            if (is_marked_tree_node(cond, in))
                return;   /* leaves validated by their own cases */
            if (!fc
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
        if (ret_fused_icmp(in)) {
            LLVMValueRef c = ret_fused_icmp(in);
            check_operand(LLVMGetOperand(c, 0), "unlowerable operand");
            check_operand(LLVMGetOperand(c, 1), "unlowerable operand");
            return;
        }
        {   LLVMValueRef sel = ret_fused_select(in);
            if (sel) {
                validate_ret_select(sel, in);
                return;
            }
        }
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
    LLVMValueRef cmp = fused_icmp_for(cond);
    if (cmp) {
        rs_pop(rs, LLVMGetOperand(cmp, 0));
        rs_pop(rs, LLVMGetOperand(cmp, 1));
    }
    else if (!LLVMIsAConstantInt(cond)
             && !LLVMIsUndef(cond) && !LLVMIsPoison(cond))
        rs_pop(rs, cond);
}

static void rs_tree_cond(readset *rs, LLVMValueRef v, LLVMValueRef term,
    int *first);

/* Condition reads for a consumer that may carry a marked connective
   tree (a conditional branch or a select). */
static void rs_cond_tree_aware(readset *rs, LLVMValueRef cond,
    LLVMValueRef consumer)
{
    if (is_marked_tree_node(cond, consumer)) {
        int first = TRUE;
        rs_tree_cond(rs, cond, consumer, &first);
    }
    else
        rs_cond(rs, cond);
}

/* Reads made by a marked connective tree's emission at its branch: the
   first-evaluated leaf reads unconditionally (a genuine pop site), every
   later leaf only on some paths. */
static void rs_tree_cond(readset *rs, LLVMValueRef v, LLVMValueRef term,
    int *first)
{
    LLVMValueRef a, b;
    if (is_marked_tree_node(v, term)) {
        int k = tree_node_shape(v, &a, &b);
        rs_tree_cond(rs, a, term, first);
        if (k != TREE_NOT)
            rs_tree_cond(rs, b, term, first);
        return;
    }
    {   valinfo *vi = LLVMIsAInstruction(v) ? lookup(v) : NULL;
        if (vi && vi->kind == VK_FUSED && LLVMIsAICmpInst(v)) {
            if (*first) {
                rs_pop(rs, LLVMGetOperand(v, 0));
                rs_pop(rs, LLVMGetOperand(v, 1));
            }
            else {
                rs_other(rs, LLVMGetOperand(v, 0));
                rs_other(rs, LLVMGetOperand(v, 1));
            }
        }
        else if (*first)
            rs_pop(rs, v);
        else
            rs_other(rs, v);
        *first = FALSE;
    }
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

/* Reads of a fused return select chain, in emission order: the root
   condition first, then the chained arm's reads. Conditions read as
   pops — each executed pop sequence is a prefix of this order, and a
   return discards whatever an untaken chain suffix left pending. */
static void rs_ret_select(readset *rs, LLVMValueRef term, LLVMValueRef sel)
{
    int i;
    rs_cond_tree_aware(rs, LLVMGetOperand(sel, 0), sel);
    for (i = 1; i <= 2; i++) {
        LLVMValueRef a = LLVMGetOperand(sel, i);
        valinfo *ai = LLVMIsAInstruction(a) ? lookup(a) : NULL;
        if (ai && ai->kind == VK_SKIP && ai->sunk_into == term)
            rs_ret_select(rs, term, a);
        else
            rs_other(rs, a);
    }
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
            if (tvs || fvs || (vi && vi->fold_store)) {
                /* Diamond emission: the branch reads the condition first;
                   both arms are then conditional. */
                rs_cond_tree_aware(rs, cond, in);
                if (tvs) rs_arm_reads(rs, tvs);
                else rs_other(rs, LLVMGetOperand(in, 1));
                if (fvs) rs_arm_reads(rs, fvs);
                else rs_other(rs, LLVMGetOperand(in, 2));
                return;
            }
            if (select_swapped(in)) {
                rs_pop(rs, LLVMGetOperand(in, 2));   /* fv first */
                rs_cond_tree_aware(rs, cond, in);
                rs_other(rs, LLVMGetOperand(in, 1));
                return;
            }
            rs_pop(rs, LLVMGetOperand(in, 1));   /* tv, unconditionally */
            rs_cond_tree_aware(rs, cond, in);
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
        if (!store_is_folded(in, 0))
            rs_pop(rs, LLVMGetOperand(in, 0));
        return;

    case LLVMRet:
        {   LLVMValueRef fc = ret_fused_icmp(in);
            LLVMValueRef sel = ret_fused_select(in);
            if (fc) {
                rs_pop(rs, LLVMGetOperand(fc, 0));
                rs_pop(rs, LLVMGetOperand(fc, 1));
            }
            else if (sel)
                rs_ret_select(rs, in, sel);
            else if (LLVMGetNumOperands(in) == 1)
                rs_pop(rs, LLVMGetOperand(in, 0));
        }
        return;

    case LLVMBr:
        /* A branch whose successors coincide is emitted as a plain goto:
           the condition is never read at runtime. */
        if (LLVMIsConditional(in)
            && LLVMGetSuccessor(in, 0) != LLVMGetSuccessor(in, 1)) {
            LLVMValueRef cond = LLVMGetCondition(in);
            if (is_marked_tree_node(cond, in)) {
                int first = TRUE;
                rs_tree_cond(rs, cond, in, &first);
            }
            else
                rs_cond(rs, cond);
        }
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
                if (!store_is_folded(in, 1))
                    rs_pop(rs, LLVMGetOperand(in, 1));
                return;
            }
            if (strcmp(name, "i6.stkpush") == 0) {
                rs_pop(rs, LLVMGetOperand(in, 0));
                return;
            }
            if (strcmp(name, "i6.stkpop") == 0)
                return;
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
            if (call_writes_no_ram(name)) return FALSE;
            /* i6.deref.store and other opaque opcodes (call, glk,
               astore...) may write anywhere in RAM, including the
               globals array. */
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

/* Write to g anywhere in positions [p1, p2] inclusive. */
static int global_clobber_in_range(LLVMValueRef g, int p1, int p2)
{
    int q;
    for (q = p1; q <= p2; q++)
        if (global_clobbers(vals[q].v, g)) return TRUE;
    return FALSE;
}

static int block_first_pos(int blk);
static void mark_reachable(int blk, unsigned char *seen);

/* dirty_in[b]: some execution path from the load reaches b's entry
   having written g since the load last executed. Re-entering the load's
   own block kills the taint: a fresh execution of the load observes the
   written value, so direct reads agree with it again. Only blocks
   reachable from the load can carry taint toward its uses. */
static void global_dirty_dataflow(LLVMValueRef g, int loadblk, int loadpos,
    unsigned char *dirty_in, unsigned char *reach)
{
    int changed = TRUE, b;
    unsigned i, n;
    memset(dirty_in, 0, n_blocks);
    memset(reach, 0, n_blocks);
    mark_reachable(loadblk, reach);
    while (changed) {
        changed = FALSE;
        for (b = 0; b < n_blocks; b++) {
            int out;
            LLVMValueRef term;
            if (!reach[b]) continue;
            if (b == loadblk)
                out = global_clobber_in_range(g, loadpos + 1,
                    blkinfo[b].end_pos);
            else
                out = dirty_in[b]
                    || global_clobber_in_range(g, block_first_pos(b),
                        blkinfo[b].end_pos);
            if (!out) continue;
            term = LLVMGetBasicBlockTerminator(blkinfo[b].bb);
            if (!term) continue;
            n = LLVMGetNumSuccessors(term);
            for (i = 0; i < n; i++) {
                blockinfo *sb = block_info(LLVMGetSuccessor(term, i));
                if (sb && !dirty_in[sb - blkinfo]) {
                    dirty_in[sb - blkinfo] = 1;
                    changed = TRUE;
                }
            }
        }
    }
}

/* Where a value's emission actually happens: values sunk into a
   consumer (select arms, ret-fused selects, connective trees) emit —
   and read their operands — at that consumer's position. */
static valinfo *emission_site(valinfo *vi)
{
    while (vi->kind == VK_SKIP && vi->sunk_into) {
        valinfo *ci = lookup(vi->sunk_into);
        if (!ci) break;
        vi = ci;
    }
    return vi;
}

/* A read of the load's value emitted at position usepos (in useblk)
   observes the global's current value; that equals the loaded value
   when no path from the load's execution to the read passes a write. */
static int global_read_safe(LLVMValueRef g, int loadblk, int loadpos,
    const unsigned char *dirty_in, int useblk, int usepos)
{
    if (useblk == loadblk && usepos > loadpos)
        return global_safe_span(g, loadpos, usepos);
    if (dirty_in[useblk]) return FALSE;
    return !global_clobber_in_range(g, block_first_pos(useblk), usepos - 1);
}

static void global_operand_pass(void)
{
    int p;
    unsigned char *dirty_in = my_calloc(1, n_blocks, "llvm lower dirty");
    unsigned char *reach = my_calloc(1, n_blocks, "llvm lower dirty reach");

    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        LLVMValueRef g;
        LLVMUseRef use;
        int ok = TRUE;

        if (vi->kind != VK_SLOT) continue;
        if (LLVMGetInstructionOpcode(vi->v) != LLVMLoad) continue;
        g = LLVMGetOperand(vi->v, 0);
        global_dirty_dataflow(g, vi->blk, vi->pos, dirty_in, reach);

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
                    if (!pb) { ok = FALSE; break; }
                    ok = global_read_safe(g, vi->blk, vi->pos, dirty_in,
                        (int)(pb - blkinfo), pb->end_pos + 1);
                }
            }
            else if (ui->kind == VK_FUSED) {
                /* Read where the icmp's branch/select user emits. */
                LLVMValueRef reader = LLVMGetUser(LLVMGetFirstUse(user));
                valinfo *ri = reader ? lookup(reader) : NULL;
                if (!ri) ok = FALSE;
                else {
                    ri = emission_site(ri);
                    ok = global_read_safe(g, vi->blk, vi->pos, dirty_in,
                        ri->blk, ri->pos);
                }
            }
            else if (ui->kind == VK_ALIAS
                || (ui->kind == VK_SKIP
                    && LLVMGetInstructionOpcode(user) == LLVMTrunc)) {
                /* Forwarded reads; not worth chasing. */
                ok = FALSE;
            }
            else {
                valinfo *ei = emission_site(ui);
                if (ei->edge_fold)
                    ok = FALSE;   /* reads at its phi edges; not chased */
                else
                    ok = global_read_safe(g, vi->blk, vi->pos, dirty_in,
                        ei->blk, ei->pos);
            }
        }
        if (ok) vi->kind = VK_GLOBAL;
    }
    my_free(&dirty_in, "llvm lower dirty");
    my_free(&reach, "llvm lower dirty reach");
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
                if (call_writes_no_ram(name)) continue;
                return TRUE;
            }
        default:
            continue;
        }
    }
    return FALSE;
}

/* The global a read of this value resolves to, or NULL: VK_GLOBAL loads
   and defs folded into a global store (their reads use the store's
   target operand). */
static LLVMValueRef read_resolves_to_global(valinfo *ui)
{
    if (!ui) return NULL;
    if (ui->kind == VK_GLOBAL) return LLVMGetOperand(ui->v, 0);
    if (ui->kind == VK_SKIP && ui->fold_store
        && LLVMGetInstructionOpcode(ui->fold_store) == LLVMStore)
        return LLVMGetOperand(ui->fold_store, 1);
    return NULL;
}

/* Sinking moves the arm's reads from its own position to the select's;
   reads of globals must still see the same values there. */
static int arm_globals_stable(LLVMValueRef op, int from, int to)
{
    unsigned i, n = LLVMGetNumOperands(op);
    for (i = 0; i < n; i++) {
        LLVMValueRef g = read_resolves_to_global(
            underlying(LLVMGetOperand(op, i)));
        if (g && !global_safe_span(g, from, to))
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
            LLVMValueRef g = read_resolves_to_global(underlying(wide));
            if (g && !global_safe_span(g, vi->pos, to_pos))
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

/* A loop recurrence can be rematerialized on a later backedge without
   re-reading arbitrary state: the phi has one dedicated slot, and Glulx reads
   arithmetic sources before writing the destination. */
static int self_recurrence_for_phi(valinfo *vi, LLVMValueRef phi)
{
    LLVMValueRef a, b;
    LLVMOpcode op = LLVMGetInstructionOpcode(vi->v);
    if (op != LLVMAdd && op != LLVMSub) return FALSE;
    a = LLVMGetOperand(vi->v, 0);
    b = LLVMGetOperand(vi->v, 1);
    if (op == LLVMSub)
        return a == phi && LLVMIsAConstantInt(b);
    return (a == phi && LLVMIsAConstantInt(b))
        || (b == phi && LLVMIsAConstantInt(a));
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
                   block (its operands are re-read on that edge). Ret-phis
                   have no edge copies to fold into: their edges resolve
                   the incoming as a return operand, which needs a slot. */
                unsigned i, n = LLVMCountIncoming(user);
                if (is_ret_phi(user)) { ok = FALSE; break; }
                if (phi_use && phi_use != user) { ok = FALSE; break; }
                for (i = 0; i < n; i++) {
                    blockinfo *pb;
                    if (LLVMGetIncomingValue(user, i) != vi->v) continue;
                    pb = block_info(LLVMGetIncomingBlock(user, i));
                    if (!pb || (pb != &blkinfo[vi->blk]
                            && !self_recurrence_for_phi(vi, user))) {
                        ok = FALSE;
                        break;
                    }
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
/*   Phase A, pass 4d: store folding                                         */
/*                                                                           */
/*   A single-emission def whose only consumer is a store can write the      */
/*   store's target (a global or deref operand) directly, instead of         */
/*   passing through the stack or a slot and paying a copy: "add a b Glob"   */
/*   where classic codegen also emits one instruction. Glulx evaluates       */
/*   source operands before the store operand, so the def may read its own   */
/*   target ("add Glob 1 Glob"). Sound only when nothing emitted between     */
/*   the def and the store could observe the target's early write, so we     */
/*   require the window between them to emit nothing at all.                 */
/*                                                                           */
/*   The multi-use variant folds a def stored to a global and then reused:   */
/*   the later reads use the GLOBALVAR operand while the global stays        */
/*   unclobbered, matching classic's "assign to global, read it back".       */
/* ------------------------------------------------------------------------- */

static int stackable_def(valinfo *vi);
static LLVMValueRef effective_reader(LLVMValueRef v);

/* TRUE if this instruction's emission dispatches no runtime instructions
   at its own position (any reads it causes happen at its consumers). */
static int emits_nothing(valinfo *vi)
{
    switch (vi->kind) {
    case VK_ALIAS: case VK_FUSED: case VK_GLOBAL:
        return TRUE;
    case VK_SKIP:
        {   LLVMOpcode opc = LLVMGetInstructionOpcode(vi->v);
            if (opc == LLVMTrunc) return TRUE;  /* narrow trunc */
            if (opc == LLVMCall) {
                const char *name = called_fn_name(vi->v);
                return name && (strcmp(name, "i6.sym") == 0
                    || name_has_prefix(name, "llvm.assume")
                    || name_has_prefix(name, "llvm.lifetime")
                    || name_has_prefix(name, "llvm.dbg")
                    || name_has_prefix(name, "llvm.donothing"));
            }
        }
        return FALSE;
    default:
        return FALSE;   /* VK_SLOT / VK_STACK emit */
    }
}

/* TRUE if this instruction's emission cannot invalidate anything a
   later-moved read might see: it writes no RAM, no globals, and nothing
   but its own result slot. Used by the passes that move reads later
   (ret-selects, branch trees); pool slot reuse cannot alias the moved
   reads' slots because their last_use extends to the consumer. */
static int window_safe_for_reads(valinfo *vi)
{
    if (emits_nothing(vi)) return TRUE;
    /* A def folded into a store writes a global, not just its slot. The
       callers all run before store_fold_pass today; this guards against
       a future reordering. */
    if (vi->fold_store) return FALSE;
    switch (LLVMGetInstructionOpcode(vi->v)) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMUDiv: case LLVMURem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
    case LLVMICmp: case LLVMSelect:
    case LLVMZExt: case LLVMSExt: case LLVMTrunc:
    case LLVMFreeze: case LLVMLoad:
        return TRUE;
    case LLVMCall:
        {   const char *name = called_fn_name(vi->v);
            if (!name) return FALSE;
            if (strcmp(name, "i6.deref") == 0) return TRUE;
            if (name_has_prefix(name, "llvm.")) return TRUE;
            return call_writes_no_ram(name);
        }
    default:
        return FALSE;   /* stores, opaque calls, terminators */
    }
}

/* Multi-use variant of the store fold: one use is a store to an i6
   global immediately after the def (nothing emits between, the same
   early-write rule as the single-use fold), and every other use reads
   the value later in the block with no intervening write to that
   global. The def then writes the global directly and the other uses
   read the GLOBALVAR operand; both the copy and the def's slot
   disappear. This is the shape classic codegen produces for
   "glob = expr; ... use glob ..." ("add sp K Glob; div Glob C sp"). */
static void multi_store_fold(valinfo *vi)
{
    LLVMValueRef st = NULL, g;
    valinfo *si = NULL;
    LLVMUseRef use;
    int q;

    /* Candidate: the earliest store of this value to a global. */
    for (use = LLVMGetFirstUse(vi->v); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        valinfo *ui;
        if (!LLVMIsAInstruction(user)) return;
        if (LLVMGetInstructionOpcode(user) != LLVMStore) continue;
        ui = lookup(user);
        if (!ui) return;
        if (!si || ui->pos < si->pos) { st = user; si = ui; }
    }
    if (!st || si->blk != vi->blk) return;
    g = LLVMGetOperand(st, 1);
    if (!LLVMIsAGlobalVariable(g)) return;

    for (q = vi->pos + 1; q < si->pos; q++)
        if (!emits_nothing(&vals[q])) return;

    /* Every other use must read strictly after the store, in the same
       block, with the global unclobbered from the store to the read.
       Reads absorbed elsewhere (aliases, sunk selects, tree nodes) are
       not worth chasing; fused icmps read at their consumer. */
    for (use = LLVMGetFirstUse(vi->v); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        valinfo *ui;
        if (user == st) continue;
        if (!LLVMIsAInstruction(user) || LLVMIsAPHINode(user)) return;
        ui = lookup(user);
        if (!ui) return;
        if (ui->kind == VK_FUSED) {
            LLVMValueRef c = LLVMGetUser(LLVMGetFirstUse(user));
            ui = c ? lookup(c) : NULL;
            if (!ui) return;
        }
        else if (ui->kind == VK_ALIAS
            || (ui->kind == VK_SKIP
                && LLVMGetInstructionOpcode(user) != LLVMRet))
            return;
        if (ui->blk != vi->blk) return;
        if (ui->pos <= si->pos) return;
        if (!global_safe_span(g, si->pos, ui->pos)) return;
    }
    vi->fold_store = st;
    vi->kind = VK_SKIP;
}

static void store_fold_pass(void)
{
    int p, q;
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        LLVMValueRef reader;
        valinfo *si;
        int validx;

        if (vi->kind != VK_SLOT) continue;
        reader = effective_reader(vi->v);
        /* A select is multi-instruction, but can still write a sole store
           directly. Its emitter branches before writing either arm, so the
           destination cannot clobber a condition or arm read. */
        if (LLVMGetInstructionOpcode(vi->v) == LLVMSelect) {
            if (!reader) continue;
            if (LLVMGetInstructionOpcode(reader) == LLVMStore)
                validx = 0;
            else if (LLVMIsACallInst(reader)) {
                const char *name = called_fn_name(reader);
                if (!name || strcmp(name, "i6.deref.store") != 0) continue;
                validx = 1;
            }
            else continue;
            if (underlying(LLVMGetOperand(reader, validx)) != vi) continue;
            si = lookup(reader);
            if (!si || si->blk != vi->blk) continue;
            for (q = vi->pos + 1; q < si->pos; q++)
                if (!emits_nothing(&vals[q])) break;
            if (q < si->pos) continue;
            vi->fold_store = reader;
            continue;
        }
        if (!stackable_def(vi)) continue;
        if (!reader) { multi_store_fold(vi); continue; }
        if (LLVMGetInstructionOpcode(reader) == LLVMStore)
            validx = 0;
        else if (LLVMIsACallInst(reader)) {
            const char *name = called_fn_name(reader);
            if (!name || strcmp(name, "i6.deref.store") != 0) continue;
            validx = 1;
        }
        else continue;
        if (underlying(LLVMGetOperand(reader, validx)) != vi) continue;
        si = lookup(reader);
        if (!si || si->blk != vi->blk) continue;
        for (q = vi->pos + 1; q < si->pos; q++)
            if (!emits_nothing(&vals[q])) break;
        if (q < si->pos) continue;
        vi->fold_store = reader;
        vi->kind = VK_SKIP;
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 1b: returns of selects                                    */
/*                                                                           */
/*   "ret (select c, K1, K2)" with immediate arms: the select's diamond      */
/*   (copy/branch/copy, 3-5 instructions) plus the return collapse into a    */
/*   conditional branch between two returns (2-3). One arm may instead be    */
/*   another qualifying select, which chains a further conditional return:   */
/*   if-conversion turns branchy boolean routines (Unsigned__Compare) into   */
/*   exactly this nest, and the chain restores the branch form. Chains       */
/*   only, never a select in both arms: a stack-riding condition operand     */
/*   of the second arm would be popped out of LIFO order on the path that    */
/*   takes the first arm's chain, whereas a chain only ever abandons a       */
/*   pending suffix, which the return discards. The selects are marked      */
/*   sunk into the ret; the decision is memoized here because it depends     */
/*   on a window check whose answer must not drift as later passes           */
/*   reclassify values. Runs right after classify, before validation.        */
/* ------------------------------------------------------------------------- */

static int is_immediate_val(LLVMValueRef v);
static int ret_arm_qualifies(LLVMValueRef v, int blk, int *minpos, int depth);

#define RET_SELECT_MAX_DEPTH 4

static int ret_select_qualifies(LLVMValueRef rv, int blk, int *minpos,
    int depth)
{
    LLVMValueRef cond;
    LLVMUseRef use;
    valinfo *si = lookup(rv);
    if (!si || si->kind != VK_SLOT || si->blk != blk) return FALSE;
    if (int_width(rv) != 32) return FALSE;
    use = LLVMGetFirstUse(rv);
    if (!use || LLVMGetNextUse(use)) return FALSE;
    cond = LLVMGetOperand(rv, 0);
    if (LLVMIsAConstantInt(cond) || LLVMIsUndef(cond)
        || LLVMIsPoison(cond)) return FALSE;
    if (!is_immediate_val(LLVMGetOperand(rv, 1))
        && !is_immediate_val(LLVMGetOperand(rv, 2))) return FALSE;
    if (!ret_arm_qualifies(LLVMGetOperand(rv, 1), blk, minpos, depth)
        || !ret_arm_qualifies(LLVMGetOperand(rv, 2), blk, minpos, depth))
        return FALSE;
    if (si->pos < *minpos) *minpos = si->pos;
    return TRUE;
}

static int ret_arm_qualifies(LLVMValueRef v, int blk, int *minpos, int depth)
{
    if (is_immediate_val(v)) return TRUE;
    if (depth >= RET_SELECT_MAX_DEPTH) return FALSE;
    if (!LLVMIsAInstruction(v)
        || LLVMGetInstructionOpcode(v) != LLVMSelect) return FALSE;
    return ret_select_qualifies(v, blk, minpos, depth + 1);
}

static void mark_ret_select(LLVMValueRef term, LLVMValueRef rv)
{
    valinfo *si = lookup(rv);
    int i;
    si->kind = VK_SKIP;
    si->sunk_into = term;
    for (i = 1; i <= 2; i++) {
        LLVMValueRef a = LLVMGetOperand(rv, i);
        if (!is_immediate_val(a)) mark_ret_select(term, a);
    }
}

static void ret_select_pass(void)
{
    int p, q, minpos;
    for (p = 0; p < n_vals; p++) {
        LLVMValueRef term = vals[p].v, rv;

        if (LLVMGetInstructionOpcode(term) != LLVMRet) continue;
        if (LLVMGetNumOperands(term) != 1) continue;
        rv = LLVMGetOperand(term, 0);
        if (!LLVMIsAInstruction(rv)
            || LLVMGetInstructionOpcode(rv) != LLVMSelect) continue;
        minpos = n_vals;
        if (!ret_select_qualifies(rv, vals[p].blk, &minpos, 0)) continue;
        /* Every chained condition's and arm's reads move from their
           select's position to the ret's; anything in between that
           writes could clobber them. */
        for (q = minpos + 1; q < p; q++)
            if (!window_safe_for_reads(&vals[q])) break;
        if (q < p) continue;
        mark_ret_select(term, rv);
    }
}

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 1c: branch condition trees                                */
/*                                                                           */
/*   instcombine folds source-level short-circuit branches ("if (a && b)")  */
/*   into i1 connectives feeding one conditional branch — right for          */
/*   hardware, expensive for an interpreter, where each materialized 0/1     */
/*   compare costs a diamond. A single-use connective tree whose only        */
/*   consumer is a same-block conditional branch is marked sunk into that    */
/*   branch and re-expanded at emission into chained compare-branches;       */
/*   its single-use same-block compare leaves fuse into those branches.      */
/*   Decided and memoized here, before validation, like ret-selects.         */
/* ------------------------------------------------------------------------- */

#define MAX_TREE 16

typedef struct treebuf_s {
    LLVMValueRef nodes[MAX_TREE]; int n_nodes;
    LLVMValueRef fuse[MAX_TREE];  int n_fuse;
    int min_pos;
} treebuf;

static int tree_leaf_ok(LLVMValueRef v, LLVMBasicBlockRef bb, treebuf *tb)
{
    if (LLVMIsAInstruction(v) && LLVMIsAICmpInst(v)
        && LLVMGetInstructionParent(v) == bb) {
        LLVMUseRef use = LLVMGetFirstUse(v);
        valinfo *vi = lookup(v);
        if (vi && (vi->kind == VK_SLOT || vi->kind == VK_FUSED)
            && use && !LLVMGetNextUse(use)) {
            /* VK_FUSED here means "fused into its select user", which is
               itself a node of this tree; it needs no re-marking. */
            if (vi->kind == VK_SLOT) {
                if (tb->n_fuse >= MAX_TREE) return FALSE;
                tb->fuse[tb->n_fuse++] = v;
            }
            if (vi->pos < tb->min_pos) tb->min_pos = vi->pos;
            return TRUE;
        }
    }
    return operand_ok(v);   /* any readable 0/1 value */
}

static int tree_collect(LLVMValueRef v, LLVMBasicBlockRef bb, treebuf *tb,
    int depth)
{
    LLVMValueRef a, b;
    valinfo *vi;
    LLVMUseRef use;
    int shape;

    if (depth > 6) return FALSE;
    shape = tree_node_shape(v, &a, &b);
    if (shape == TREE_NONE) return FALSE;
    vi = lookup(v);
    if (!vi || vi->kind != VK_SLOT) return FALSE;
    if (LLVMGetInstructionParent(v) != bb) return FALSE;
    use = LLVMGetFirstUse(v);
    if (!use || LLVMGetNextUse(use)) return FALSE;
    if (tb->n_nodes >= MAX_TREE) return FALSE;

    if (!tree_collect(a, bb, tb, depth + 1) && !tree_leaf_ok(a, bb, tb))
        return FALSE;
    if (shape != TREE_NOT
        && !tree_collect(b, bb, tb, depth + 1) && !tree_leaf_ok(b, bb, tb))
        return FALSE;
    tb->nodes[tb->n_nodes++] = v;
    if (vi->pos < tb->min_pos) tb->min_pos = vi->pos;
    return TRUE;
}

static int in_treebuf(treebuf *tb, LLVMValueRef v)
{
    int i;
    for (i = 0; i < tb->n_nodes; i++) if (tb->nodes[i] == v) return TRUE;
    for (i = 0; i < tb->n_fuse; i++) if (tb->fuse[i] == v) return TRUE;
    return FALSE;
}

static void branch_tree_pass(void)
{
    int p, q, i;
    /* Walk backward so an outer consumer claims a nested connective
       select (as a tree node) before that select could claim its own
       condition as a separate tree. */
    for (p = n_vals - 1; p >= 0; p--) {
        LLVMValueRef term = vals[p].v, cond;
        LLVMOpcode opc = LLVMGetInstructionOpcode(term);
        treebuf tb;

        /* Consumers whose emission branches on a condition: conditional
           branches, and selects (their diamond re-branches on it). */
        if (opc == LLVMBr) {
            if (!LLVMIsConditional(term)) continue;
            if (LLVMGetSuccessor(term, 0) == LLVMGetSuccessor(term, 1))
                continue;
            cond = LLVMGetCondition(term);
        }
        else if (opc == LLVMSelect) {
            /* Ordinary selects, and selects already fused into a return
               (their chain re-branches on the condition at the ret). */
            valinfo *si = lookup(term);
            if (!si) continue;
            if (si->kind != VK_SLOT
                && !(si->kind == VK_SKIP && si->sunk_into
                     && LLVMIsATerminatorInst(si->sunk_into))) continue;
            cond = LLVMGetOperand(term, 0);
        }
        else
            continue;
        tb.n_nodes = 0; tb.n_fuse = 0; tb.min_pos = p;
        if (!tree_collect(cond, LLVMGetInstructionParent(term), &tb, 0))
            continue;
        /* The leaves' reads move to the consumer; anything in between
           that writes could clobber what they read. */
        for (q = tb.min_pos + 1; q < p; q++)
            if (!in_treebuf(&tb, vals[q].v)
                && !window_safe_for_reads(&vals[q]))
                break;
        if (q < p) continue;
        for (i = 0; i < tb.n_nodes; i++) {
            valinfo *ni = lookup(tb.nodes[i]);
            ni->kind = VK_SKIP;
            ni->sunk_into = term;
        }
        for (i = 0; i < tb.n_fuse; i++)
            lookup(tb.fuse[i])->kind = VK_FUSED;
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
/*   slot. Opaque .s calls push and pop their explicit arguments in a        */
/*   balanced way above any pending values; additionally, pending values     */
/*   matching a contiguous deepest tail of a .s call's argument push order   */
/*   stay on the stack and skip their explicit pushes entirely.              */
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
                && LLVMGetInstructionOpcode(user) == LLVMTrunc)
            || (ui->kind == VK_SKIP && ui->sunk_into
                && (LLVMIsATerminatorInst(ui->sunk_into)
                    || is_marked_tree_node(user, ui->sunk_into)))) {
            /* The last case: ret-selects and connective trees, whose
               reads happen at the consumer that absorbs them. */
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
    int n_src, nargs;

    if (!stackable_def(vi)) return FALSE;
    reader = effective_reader(vi->v);
    if (!reader || LLVMIsAPHINode(reader)) return FALSE;
    ri = lookup(reader);
    if (!ri || ri->blk != vi->blk) return FALSE;

    if (stack_call_shape(reader, &n_src, &nargs)) {
        /* A .s call's stack arguments are genuine pops, but only a
           contiguous deepest tail of the push order can stay pending;
           fusion_pass decides that with its stack simulation and
           unfuses the rest. Regular operands read after the pushes,
           so feeding one from the stack would pop an argument. */
        int hits = 0;
        for (k = 0; k < n_src; k++)
            if (rep(LLVMGetOperand(reader, k)) == vi->v) return FALSE;
        for (k = n_src; k < nargs; k++)
            if (rep(LLVMGetOperand(reader, k)) == vi->v) hits++;
        return hits == 1;
    }

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
            int n_src, nargs;

            if (stack_call_shape(in, &n_src, &nargs)) {
                /* Explicit argument pushes go deepest-first (last argument
                   first), so pending values matching a contiguous tail of
                   that push order — the top of the pending stack reading
                   extras[j], extras[j+1], ... from the top down — are
                   already in position and their pushes are elided.
                   Everything else the call reads must leave the stack:
                   explicit pushes would land on top of it. */
                int L, t, max_match = nargs - n_src;
                if (max_match > n_pend) max_match = n_pend;
                for (L = max_match; L > 0; L--) {
                    for (t = 0; t < L; t++)
                        if (pend[n_pend - 1 - t]
                            != rep(LLVMGetOperand(in, nargs - L + t)))
                            break;
                    if (t == L) break;
                }
                n_pend -= L;
                for (k = 0; k < nargs - L; k++) {
                    LLVMValueRef r = rep(LLVMGetOperand(in, k));
                    for (j = n_pend - 1; j >= 0; j--) {
                        if (pend[j] != r) continue;
                        unfuse(r);
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
                continue;
            }

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

            /* Spill traffic (stkpush/stkpop) moves the REAL stack top,
               which the pending model cannot see: anything still fused
               across it would be pushed above the spilled values or
               popped in their place. Unfuse everything; a stkpop's own
               result may then fuse below - it is already on top. */
            if (LLVMIsACallInst(in)) {
                const char *cname = called_fn_name(in);
                if (cname && (strcmp(cname, "i6.stkpush") == 0
                    || strcmp(cname, "i6.stkpop") == 0)) {
                    for (j = 0; j < n_pend; j++)
                        unfuse(pend[j]);
                    n_pend = 0;
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

        /* Lone "ret v" blocks: gotos inline the return when v is a
           constant or a value readable at every predecessor (a dedicated
           cross-block slot or a parameter slot — both are written by v's
           def, which dominates every edge into this block, and neither
           is ever reused). Branches reach a ret 0 / ret 1 via the -3/-4
           encodings. */
        blkinfo[i].ret_inline = NULL;
        blkinfo[i].retconst = -1;
        blkinfo[i].ret_phi = NULL;
        blkinfo[i].emit_body = TRUE;
        in = LLVMGetFirstInstruction(bb);
        if (in && in == LLVMGetBasicBlockTerminator(bb)
            && LLVMGetInstructionOpcode(in) == LLVMRet
            && LLVMGetNumOperands(in) == 1) {
            LLVMValueRef rv = LLVMGetOperand(in, 0);
            int inlinable = FALSE;
            if (LLVMIsAConstantInt(rv) || LLVMIsUndef(rv) || LLVMIsPoison(rv)
                || is_sym_call(rv))
                inlinable = TRUE;
            else if (LLVMIsAArgument(rv))
                inlinable = (param_index(rv) >= 0);
            else if (LLVMIsAInstruction(rv)) {
                valinfo *ri = underlying(rv);
                inlinable = (ri && ri->kind == VK_SLOT);
            }
            if (inlinable) {
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
        else if (in && is_ret_phi(in)) {
            /* Every edge returns its incoming value directly; nothing
               ever jumps to the block itself. */
            blkinfo[i].ret_phi = in;
            if (i > 0)
                blkinfo[i].emit_body = FALSE;
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
            /* Parameters have no valinfo but occupy their own slots,
               which phi coalescing may now share. */
            if (r && LLVMIsAArgument(r) && param_index(r) + 1 == s)
                return TRUE;
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
                LLVMValueRef iv = LLVMGetIncomingValue(r, k);
                valinfo *ii = underlying(iv);
                if (ii && ii->kind == VK_SLOT && ii->slot == pi->slot)
                    return FALSE;
                if (LLVMIsAArgument(iv)
                    && param_index(iv) + 1 == pi->slot) return FALSE;
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

/* Mark every block reachable from blk along CFG edges, including blk
   itself. */
static void mark_reachable(int blk, unsigned char *seen)
{
    LLVMValueRef term;
    unsigned i, n;
    if (seen[blk]) return;
    seen[blk] = 1;
    term = LLVMGetBasicBlockTerminator(blkinfo[blk].bb);
    if (!term) return;
    n = LLVMGetNumSuccessors(term);
    for (i = 0; i < n; i++) {
        blockinfo *sb = block_info(LLVMGetSuccessor(term, i));
        if (sb) mark_reachable((int)(sb - blkinfo), seen);
    }
}

/* A phi one of whose incomings is a parameter can live in the
   parameter's own slot when the parameter is dead once the phi's block
   is entered: the entry edge copy becomes an elided self-copy and every
   other edge updates the parameter slot in place, exactly as classic
   code mutates the source local. Parameter slots are never written by
   lowered code and never pooled, so the overwrite is unobservable
   provided no read of the parameter can execute at or after an edge
   into the phi's block: no non-phi read in a block reachable from the
   phi's block, no phi read on an edge leaving a reachable block, and no
   other phi of the same block reading the parameter (its edge copy
   would race ours). */
static int phi_param_slot(LLVMValueRef phi, unsigned char *seen)
{
    LLVMBasicBlockRef pblk = LLVMGetInstructionParent(phi);
    blockinfo *pb = block_info(pblk);
    unsigned i, n = LLVMCountIncoming(phi);

    if (!pb) return 0;
    memset(seen, 0, n_blocks);
    mark_reachable((int)(pb - blkinfo), seen);

    for (i = 0; i < n; i++) {
        LLVMValueRef v = LLVMGetIncomingValue(phi, i);
        LLVMUseRef use;
        int pi;
        if (!LLVMIsAArgument(v)) continue;
        pi = param_index(v);
        if (pi < 0) continue;
        for (use = LLVMGetFirstUse(v); use; use = LLVMGetNextUse(use)) {
            LLVMValueRef user = LLVMGetUser(use);
            blockinfo *ub;
            if (!LLVMIsAInstruction(user)) break;
            if (LLVMIsAPHINode(user)) {
                unsigned k, kn = LLVMCountIncoming(user);
                if (user != phi
                    && LLVMGetInstructionParent(user) == pblk) break;
                for (k = 0; k < kn; k++) {
                    if (LLVMGetIncomingValue(user, k) != v) continue;
                    ub = block_info(LLVMGetIncomingBlock(user, k));
                    if (!ub || seen[ub - blkinfo]) break;
                }
                if (k < kn) break;
                continue;
            }
            ub = block_info(LLVMGetInstructionParent(user));
            if (!ub || seen[ub - blkinfo]) break;
        }
        if (use) continue;   /* a reader could observe the overwrite */
        return pi + 1;
    }
    return 0;
}

static void assign_slots(void)
{
    int p, s;
    int pool_base, n_pool = 0;
    int pool_last[MAX_LOWER_SLOTS];
    unsigned char *seen = my_calloc(1, n_blocks, "llvm lower reach");
    unsigned char param_taken[MAX_LOWER_SLOTS] = { 0 };

    next_slot = n_params + 1;

    /* Dedicated slots: phis (with staging where hazardous), values that
       cross block boundaries, and udiv/urem scratch temporaries. */
    for (p = 0; p < n_vals; p++) {
        valinfo *vi = &vals[p];
        if (vi->kind != VK_SLOT) continue;
        if (LLVMIsAPHINode(vi->v)) {
            int pslot;
            if (is_ret_phi(vi->v))
                continue;   /* per-edge returns: never copied into or read */
            pslot = phi_param_slot(vi->v, seen);
            /* One adopting phi per parameter slot: a second tenant's
               edge writes could clobber the first while it is live. */
            if (pslot && param_taken[pslot]) pslot = 0;
            if (pslot) param_taken[pslot] = 1;
            vi->slot = pslot ? pslot : next_slot++;
            if (blkinfo[vi->blk].hazard) vi->scratch = next_slot++;
        }
        else if (vi->cross)
            vi->slot = next_slot++;
        if (vi->needs_scratch)
            vi->scratch = next_slot++;
    }
    my_free(&seen, "llvm lower reach");

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

static assembly_operand store_target_op(LLVMValueRef st);

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
        int32 marker   = const_int_value(LLVMGetOperand(v, 0));
        int32 value    = const_int_value(LLVMGetOperand(v, 1));
        int32 symindex = const_int_value(LLVMGetOperand(v, 2));
        assembly_operand o;
        /* An internal-routine address (IROUTINE_MV) is deferred through the
           routine symbol's own value rather than baked here, so end-of-pass
           address assignment can set it after this operand is emitted. Emit
           SYMBOL_MV carrying the symbol index, exactly as the front end does
           for replaced routines (expressp.c): final backpatch reads
           symbols[symindex].value and applies its IROUTINE_MV marker, giving
           the identical final address. Veneer routine refs (VROUTINE_MV)
           already resolve from veneer_routine_address[] at end-of-pass, and
           anything without a symbol index stays as-is. */
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
    if (!vi) {
        compiler_error("llvm_lower: operand with no representation");
        return const_op(0);
    }
    switch (vi->kind) {
    case VK_SLOT:   return slot_op(vi->slot);
    case VK_STACK:  return stack_op();
    case VK_GLOBAL: return global_op(LLVMGetOperand(vi->v, 0));
    case VK_SKIP:
        /* A def folded into a global store: reads use the global. */
        if (vi->fold_store
            && LLVMGetInstructionOpcode(vi->fold_store) == LLVMStore)
            return store_target_op(vi->fold_store);
        /* fall through */
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
   cond is an i1 value: a fusable icmp becomes a native compare-branch,
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

/* Emit a marked connective tree as chained short-circuit branches:
   control transfers to label when the tree's value equals !invert, and
   falls through otherwise. Leaves emit through gen_cond_branch. */
static void emit_tree_branch(LLVMValueRef cond, int label, int invert,
    LLVMValueRef term)
{
    LLVMValueRef a, b;
    if (is_marked_tree_node(cond, term)) {
        int k = tree_node_shape(cond, &a, &b);
        if (k == TREE_NOT)
            emit_tree_branch(a, label, !invert, term);
        else if ((k == TREE_AND) != (invert != 0)) {
            /* AND taken-if-true / OR taken-if-false: the first leg can
               short-circuit past the branch. */
            int skip = alloc_label();
            emit_tree_branch(a, skip, k == TREE_AND, term);
            emit_tree_branch(b, label, invert, term);
            gen_label(skip);
        }
        else {
            /* AND taken-if-false / OR taken-if-true: either leg decides. */
            emit_tree_branch(a, label, invert, term);
            emit_tree_branch(b, label, invert, term);
        }
        return;
    }
    gen_cond_branch(cond, label, invert);
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

typedef enum edgekind_e {
    EDGE_UNUSED,
    EDGE_DIRECT,
    EDGE_FALLTHROUGH,
    EDGE_PHI_STUB,
    EDGE_INLINE_RETURN
} edgekind;

typedef struct edgeplan_s {
    LLVMBasicBlockRef succ;
    edgekind kind;
    int copy_phis;
    int target_label;
    LLVMValueRef return_value;
    int stub_label;
    int queue_order;
} edgeplan;

typedef enum controlkind_e {
    CONTROL_OTHER,
    CONTROL_GOTO,
    CONTROL_COND,
    CONTROL_SWITCH
} controlkind;

typedef struct emitblock_s {
    int bi;
    LLVMValueRef term;
    edgeplan *edges;
    int n_edges;
    controlkind control;
    int goto_edge;
    int branch_edge;
    int branch_invert;
    int switch_fallthrough;
    int n_queued;
} emitblock;

typedef struct emitplan_s {
    emitblock *blocks;
    edgeplan *edges;
    int n_blocks;
} emitplan;

/* How an edge into a return block can be emitted: with Glulx's
   return-0/return-1 branch encodings, as an inline "return v", or it is
   not a return edge at all. */
enum { RETEDGE_NONE, RETEDGE_BRANCH, RETEDGE_VALUE };
static int edge_return_kind(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ)
{
    blockinfo *sb = block_info(succ);
    LLVMValueRef v;
    if (sb->ret_phi)
        v = incoming_for(sb->ret_phi, pred);
    else if (sb->ret_inline && phi_count(succ) == 0)
        v = sb->ret_inline;
    else
        return RETEDGE_NONE;
    if (v && LLVMIsAConstantInt(v)) {
        int32 c = const_int_value(v);
        if (c == 0 || c == 1) return RETEDGE_BRANCH;
    }
    return RETEDGE_VALUE;
}

/* Plan an edge used as a conditional or switch target. Phi copies require a
   stub; return constants of 0 or 1 use Glulx's special branch-return labels
   whether the return block is a lone constant return or a merged ret-phi. */
static void plan_target_edge(edgeplan *ep, LLVMBasicBlockRef pred)
{
    blockinfo *sb = block_info(ep->succ);
    if (sb->ret_phi) {
        LLVMValueRef v = incoming_for(sb->ret_phi, pred);
        if (v && LLVMIsAConstantInt(v)) {
            int32 c = const_int_value(v);
            if (c == 0 || c == 1) {
                ep->kind = EDGE_DIRECT;
                ep->target_label = (c == 0) ? -3 : -4;
                return;
            }
        }
    }
    if (phi_count(ep->succ) != 0) {
        ep->kind = EDGE_PHI_STUB;
        ep->target_label = sb->label;
        if (sb->ret_phi)
            ep->return_value = incoming_for(sb->ret_phi, pred);
        return;
    }
    ep->kind = EDGE_DIRECT;
    if (sb->retconst == 0) ep->target_label = -3;
    else if (sb->retconst == 1) ep->target_label = -4;
    else ep->target_label = sb->label;
}

/* Plan an edge emitted as the non-targeted side of a terminator. */
static void plan_goto_edge(edgeplan *ep, LLVMBasicBlockRef pred,
    LLVMBasicBlockRef next_bb, int force_jump)
{
    blockinfo *sb = block_info(ep->succ);
    if (sb->ret_phi) {
        ep->kind = EDGE_INLINE_RETURN;
        ep->return_value = incoming_for(sb->ret_phi, pred);
        return;
    }
    if (sb->ret_inline && phi_count(ep->succ) == 0) {
        ep->kind = EDGE_INLINE_RETURN;
        ep->return_value = sb->ret_inline;
        return;
    }
    ep->copy_phis = phi_count(ep->succ) != 0;
    if (!force_jump && ep->succ == next_bb)
        ep->kind = EDGE_FALLTHROUGH;
    else {
        ep->kind = EDGE_DIRECT;
        ep->target_label = sb->label;
    }
}

/* Stub labels stay lazy so labels allocated by instruction selection retain
   their existing numbers relative to control-flow labels. */
static int planned_target_label(emitblock *bp, int edge)
{
    edgeplan *ep = &bp->edges[edge];
    if (ep->kind == EDGE_DIRECT) return ep->target_label;
    if (ep->kind == EDGE_PHI_STUB) {
        if (ep->queue_order < 0) {
            ep->stub_label = alloc_label();
            ep->queue_order = bp->n_queued++;
        }
        return ep->stub_label;
    }
    compiler_error("llvm_lower: planned edge is not a branch target");
    return ep->target_label;
}

static void emit_planned_edge(edgeplan *ep, LLVMBasicBlockRef pred)
{
    switch (ep->kind) {
    case EDGE_INLINE_RETURN:
        gen1(return_gc, resolve(ep->return_value));
        return;
    case EDGE_FALLTHROUGH:
        if (ep->copy_phis) emit_edge_copies(pred, ep->succ);
        return;
    case EDGE_DIRECT:
        if (ep->copy_phis) emit_edge_copies(pred, ep->succ);
        gen_jump(ep->target_label);
        return;
    default:
        compiler_error("llvm_lower: planned edge cannot be emitted as goto");
        return;
    }
}

static void flush_planned_stubs(emitblock *bp, LLVMBasicBlockRef pred)
{
    int order, i;
    for (order = 0; order < bp->n_queued; order++) {
        for (i = 0; i < bp->n_edges; i++) {
            edgeplan *ep = &bp->edges[i];
            if (ep->queue_order != order) continue;
            gen_label(ep->stub_label);
            if (ep->return_value)
                gen1(return_gc, resolve(ep->return_value));
            else {
                emit_edge_copies(pred, ep->succ);
                gen_jump(ep->target_label);
            }
            break;
        }
    }
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

/* The operand a folded store writes: its global or deref target. */
static assembly_operand store_target_op(LLVMValueRef st)
{
    if (LLVMGetInstructionOpcode(st) == LLVMStore)
        return global_op(LLVMGetOperand(st, 1));
    return deref_op(LLVMGetOperand(st, 0));   /* i6.deref.store */
}

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
    assembly_operand dst = vi->fold_store
        ? store_target_op(vi->fold_store) : slot_op(vi->slot);
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
    tvs = sunk_arm(in, 1);
    fvs = sunk_arm(in, 2);
    if (tvs || fvs || vi->fold_store) {
        /* Branch diamond: each arm's computation runs only on its path. */
        int arm = alloc_label();
        done = alloc_label();
        if (fvs) {
            emit_tree_branch(cond, arm, TRUE, in);   /* to fv when false */
            if (tvs) emit_valued_op(tvs, dst);
            else gen_copy(resolve(LLVMGetOperand(in, 1)), dst);
            gen_jump(done);
            gen_label(arm);
            emit_valued_op(fvs, dst);
        }
        else {
            emit_tree_branch(cond, arm, FALSE, in);  /* to tv when true */
            gen_copy(resolve(LLVMGetOperand(in, 2)), dst);
            gen_jump(done);
            gen_label(arm);
            if (tvs) emit_valued_op(tvs, dst);
            else gen_copy(resolve(LLVMGetOperand(in, 1)), dst);
        }
        gen_label(done);
        return;
    }
    done = alloc_label();
    if (select_swapped(in)) {
        gen_copy(resolve(LLVMGetOperand(in, 2)), dst);
        emit_tree_branch(cond, done, TRUE, in);
        gen_copy(resolve(LLVMGetOperand(in, 1)), dst);
    }
    else {
        gen_copy(resolve(LLVMGetOperand(in, 1)), dst);
        emit_tree_branch(cond, done, FALSE, in);
        gen_copy(resolve(LLVMGetOperand(in, 2)), dst);
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

    /* Stack args: listed in runtime pop order (arg n_src is the top of
       the stack), so push them in reverse to rebuild it. Fused (VK_STACK)
       arguments were pushed by their defs and are already in position. */
    if (has_stack)
        for (i = nargs - 1; i >= n_src; i--) {
            LLVMValueRef ext = LLVMGetOperand(in, i);
            LLVMValueRef r = rep(ext);
            valinfo *ei = r ? lookup(r) : NULL;
            if (ei && ei->kind == VK_STACK)
                continue;
            gen2(copy_gc, resolve(ext), stack_pointer);
        }

    for (i = 0; i < n_src; i++)
        ops[i] = resolve(LLVMGetOperand(in, i));
    if (flags & OPFLAG_STORE)
        ops[n_src] = vi->fold_store ? store_target_op(vi->fold_store)
            : (vi->kind == VK_SLOT || vi->kind == VK_STACK)
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

    if (vi && vi->fold_store) {
        /* The def writes its store's target directly; opaque opcode
           calls handle the destination in emit_opaque_call instead. */
        const char *name = (opc == LLVMCall) ? called_fn_name(in) : NULL;
        if (opc != LLVMSelect
            && (!name || strcmp(name, "i6.deref") == 0)) {
            emit_valued_op(in, store_target_op(vi->fold_store));
            return;
        }
    }

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
        if (store_is_folded(in, 0))
            return;   /* the value's def wrote the global itself */
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
                if (store_is_folded(in, 1))
                    return;   /* the value's def wrote the target itself */
                gen_copy(resolve(LLVMGetOperand(in, 1)),
                    deref_op(LLVMGetOperand(in, 0)));
                return;
            }
            if (strcmp(name, "i6.stkpush") == 0) {
                gen_copy(resolve(LLVMGetOperand(in, 0)), stack_op());
                return;
            }
            if (strcmp(name, "i6.stkpop") == 0) {
                /* A stack-fused result is already on top: the consumer
                   pops it directly and no instruction is emitted. */
                assembly_operand pop_dst;
                if (vi->kind == VK_STACK)
                    return;
                if (vi->fold_store)
                    pop_dst = store_target_op(vi->fold_store);
                else if (vi->kind == VK_SLOT)
                    pop_dst = dst_op(vi);
                else
                    pop_dst = zero_operand;
                gen_copy(stack_op(), pop_dst);
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

static void plan_terminator(emitblock *bp, LLVMBasicBlockRef next_bb)
{
    LLVMValueRef term = bp->term;
    int i;

    bp->control = CONTROL_OTHER;
    bp->goto_edge = -1;
    bp->branch_edge = -1;
    bp->switch_fallthrough = -1;
    for (i = 0; i < bp->n_edges; i++) {
        bp->edges[i].succ = LLVMGetSuccessor(term, i);
        bp->edges[i].kind = EDGE_UNUSED;
        bp->edges[i].stub_label = -1;
        bp->edges[i].queue_order = -1;
    }

    if (LLVMGetInstructionOpcode(term) == LLVMBr) {
        LLVMBasicBlockRef cur = LLVMGetInstructionParent(term);
        if (!LLVMIsConditional(term)) {
            bp->control = CONTROL_GOTO;
            bp->goto_edge = 0;
            plan_goto_edge(&bp->edges[0], cur, next_bb, FALSE);
            return;
        }
        {   LLVMValueRef cond = LLVMGetCondition(term);
            LLVMBasicBlockRef then_bb = bp->edges[0].succ;
            LLVMBasicBlockRef else_bb = bp->edges[1].succ;
            if (then_bb == else_bb || LLVMIsAConstantInt(cond)
                || LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
                int selected = 0;
                if (then_bb != else_bb
                    && !(LLVMIsAConstantInt(cond)
                         && (const_int_value(cond) & 1)))
                    selected = 1;
                bp->control = CONTROL_GOTO;
                bp->goto_edge = selected;
                plan_goto_edge(&bp->edges[selected], cur, next_bb, FALSE);
                return;
            }
            bp->control = CONTROL_COND;
            {   int then_ret = edge_return_kind(cur, then_bb);
                int else_ret = edge_return_kind(cur, else_bb);
                /* Return edges pick their cheapest role: a 0/1 return
                   is one instruction as the branch (the rfalse/rtrue
                   encodings), any other return is one instruction as
                   the goto (an inline "return v"); otherwise prefer the
                   natural fallthrough. */
                if (then_ret == RETEDGE_BRANCH) {
                    bp->branch_edge = 0;
                    bp->goto_edge = 1;
                    bp->branch_invert = FALSE;
                }
                else if (else_ret == RETEDGE_BRANCH) {
                    bp->branch_edge = 1;
                    bp->goto_edge = 0;
                    bp->branch_invert = TRUE;
                }
                else if (then_ret == RETEDGE_VALUE
                         && else_ret == RETEDGE_NONE) {
                    bp->branch_edge = 1;
                    bp->goto_edge = 0;
                    bp->branch_invert = TRUE;
                }
                else if (else_ret == RETEDGE_VALUE
                         && then_ret == RETEDGE_NONE) {
                    bp->branch_edge = 0;
                    bp->goto_edge = 1;
                    bp->branch_invert = FALSE;
                }
                /* If exactly one ordinary edge needs phi copies, emit those
                   copies on the inline/goto side and branch directly to the
                   copy-free side. Targeting the phi edge would require a
                   stub and force an extra jump on the other path. */
                else if (then_ret == RETEDGE_NONE && else_ret == RETEDGE_NONE
                         && phi_count(then_bb) != 0
                         && phi_count(else_bb) == 0) {
                    bp->branch_edge = 1;
                    bp->goto_edge = 0;
                    bp->branch_invert = TRUE;
                }
                else if (then_ret == RETEDGE_NONE && else_ret == RETEDGE_NONE
                         && phi_count(then_bb) == 0
                         && phi_count(else_bb) != 0) {
                    bp->branch_edge = 0;
                    bp->goto_edge = 1;
                    bp->branch_invert = FALSE;
                }
                else if (then_bb == next_bb) {
                    bp->branch_edge = 1;
                    bp->goto_edge = 0;
                    bp->branch_invert = TRUE;
                }
                else {
                    bp->branch_edge = 0;
                    bp->goto_edge = 1;
                    bp->branch_invert = FALSE;
                }
            }
            plan_target_edge(&bp->edges[bp->branch_edge], cur);
            plan_goto_edge(&bp->edges[bp->goto_edge], cur, next_bb,
                bp->edges[bp->branch_edge].kind == EDGE_PHI_STUB);
        }
        return;
    }

    if (LLVMGetInstructionOpcode(term) == LLVMSwitch) {
        LLVMBasicBlockRef cur = LLVMGetInstructionParent(term);
        LLVMBasicBlockRef defbb = bp->edges[0].succ;
        int nops = LLVMGetNumOperands(term);
        int c, ft = -1, force_jump = FALSE;
        bp->control = CONTROL_SWITCH;

        if (phi_count(defbb) == 0) {
            for (c = 2; c < nops; c += 2) {
                LLVMBasicBlockRef dest = bp->edges[c / 2].succ;
                if (phi_count(dest) != 0) { ft = -1; break; }
                /* Moving the last eligible case cannot delay an earlier
                   case merely to create the fallthrough. */
                if (dest == next_bb) ft = c / 2;
            }
        }
        for (c = 2; c < nops; c += 2) {
            int edge = c / 2;
            if (edge == ft) continue;
            plan_target_edge(&bp->edges[edge], cur);
            if (bp->edges[edge].kind == EDGE_PHI_STUB)
                force_jump = TRUE;
        }
        if (ft >= 0) {
            bp->switch_fallthrough = ft;
            bp->edges[ft].kind = EDGE_FALLTHROUGH;
            plan_target_edge(&bp->edges[0], cur);
        }
        else {
            bp->goto_edge = 0;
            plan_goto_edge(&bp->edges[0], cur, next_bb, force_jump);
        }
    }
}

/* A fused return select chain: branch to the 0/1 return encodings when
   an immediate arm allows it, otherwise return one arm past a label,
   and chain into any arm that is itself a select sunk into this ret. */
static void emit_ret_value(LLVMValueRef term, LLVMValueRef v);

static void emit_ret_select(LLVMValueRef term, LLVMValueRef sel)
{
    LLVMValueRef cond = LLVMGetOperand(sel, 0);
    LLVMValueRef tv = LLVMGetOperand(sel, 1);
    LLVMValueRef fv = LLVMGetOperand(sel, 2);
    int32 tc = LLVMIsAConstantInt(tv) ? const_int_value(tv) : -1;
    int32 fc2 = LLVMIsAConstantInt(fv) ? const_int_value(fv) : -1;
    if (LLVMIsAConstantInt(tv) && (tc == 0 || tc == 1)) {
        emit_tree_branch(cond, tc ? -4 : -3, FALSE, sel);
        emit_ret_value(term, fv);
    }
    else if (LLVMIsAConstantInt(fv) && (fc2 == 0 || fc2 == 1)) {
        emit_tree_branch(cond, fc2 ? -4 : -3, TRUE, sel);
        emit_ret_value(term, tv);
    }
    else {
        int l = alloc_label();
        emit_tree_branch(cond, l, TRUE, sel);   /* false arm at l */
        emit_ret_value(term, tv);
        gen_label(l);
        emit_ret_value(term, fv);
    }
}

static void emit_ret_value(LLVMValueRef term, LLVMValueRef v)
{
    valinfo *vi = LLVMIsAInstruction(v) ? lookup(v) : NULL;
    if (vi && vi->kind == VK_SKIP && vi->sunk_into == term)
        emit_ret_select(term, v);
    else
        gen1(return_gc, resolve(v));
}

static void emit_terminator(LLVMValueRef term, LLVMBasicBlockRef cur,
    emitblock *bp)
{
    switch (LLVMGetInstructionOpcode(term)) {

    case LLVMRet:
        {   LLVMValueRef fc = ret_fused_icmp(term);
            LLVMValueRef sel = ret_fused_select(term);
            if (fc) {
                gen_branch2(pred_branch_opcode(LLVMGetICmpPredicate(fc)),
                    resolve(LLVMGetOperand(fc, 0)),
                    resolve(LLVMGetOperand(fc, 1)),
                    -4);   /* "branch means return 1" */
                gen1(return_gc, const_op(0));
                return;
            }
            if (sel) {
                emit_ret_select(term, sel);
                return;
            }
        }
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
        if (bp->control == CONTROL_GOTO) {
            emit_planned_edge(&bp->edges[bp->goto_edge], cur);
            flush_planned_stubs(bp, cur);
            return;
        }
        {   LLVMValueRef cond = LLVMGetCondition(term);
            emit_tree_branch(cond,
                planned_target_label(bp, bp->branch_edge),
                bp->branch_invert, term);
            emit_planned_edge(&bp->edges[bp->goto_edge], cur);
            flush_planned_stubs(bp, cur);
        }
        return;

    case LLVMSwitch:
        {   assembly_operand cond = resolve(LLVMGetOperand(term, 0));
            int nops = LLVMGetNumOperands(term);
            int c, ft = bp->switch_fallthrough;
            for (c = 2; c < nops; c += 2) {
                int edge = c / 2;
                if (edge == ft) continue;
                gen_branch2(jeq_gc, cond,
                    resolve(LLVMGetOperand(term, c)),
                    planned_target_label(bp, edge));
            }
            if (ft >= 0) {
                gen_branch2(jne_gc, cond,
                    resolve(LLVMGetOperand(term, ft * 2)),
                    planned_target_label(bp, 0));
                return;
            }
            emit_planned_edge(&bp->edges[bp->goto_edge], cur);
            flush_planned_stubs(bp, cur);
        }
        return;

    default:
        compiler_error("llvm_lower: unhandled terminator");
        return;
    }
}

static int term_has_succ(LLVMValueRef term, LLVMBasicBlockRef s)
{
    unsigned i, n = LLVMGetNumSuccessors(term);
    for (i = 0; i < n; i++)
        if (LLVMGetSuccessor(term, i) == s) return TRUE;
    return FALSE;
}

/* Straighten jump-only transfers, minimizing explicit jumps without any
   likelihood guess. A block B that nothing can fall into (its layout
   predecessor is not a CFG predecessor) and that cannot fall out of its
   position (no successor is its layout successor) costs the same
   explicit transfers wherever it sits. Moving it directly after a block
   A that branches to B and currently falls into nothing (no successor
   equals A's layout successor) lets A's transfer to B become a
   fallthrough. No existing fallthrough involves either position, so no
   path gains a transfer; A's paths to B lose one. The entry block never
   moves. */
static void straighten_layout(emitblock *blocks, int n)
{
    int pass, i, j, k, moved = TRUE;

    for (pass = 0; pass < n && moved; pass++) {
        moved = FALSE;
        for (j = 1; j < n && !moved; j++) {
            LLVMBasicBlockRef bbb = blkinfo[blocks[j].bi].bb;
            LLVMValueRef bt = LLVMGetBasicBlockTerminator(bbb);
            LLVMValueRef pt =
                LLVMGetBasicBlockTerminator(blkinfo[blocks[j - 1].bi].bb);
            if (!bt || !pt) continue;
            if (term_has_succ(pt, bbb)) continue;
            if (j + 1 < n
                && term_has_succ(bt, blkinfo[blocks[j + 1].bi].bb))
                continue;
            for (i = 0; i < n; i++) {
                LLVMValueRef at;
                emitblock tmp;
                if (i == j || i == j - 1) continue;
                at = LLVMGetBasicBlockTerminator(blkinfo[blocks[i].bi].bb);
                if (!at || !term_has_succ(at, bbb)) continue;
                if (i + 1 < n
                    && term_has_succ(at, blkinfo[blocks[i + 1].bi].bb))
                    continue;
                /* Slide B out of j and in after i. */
                tmp = blocks[j];
                if (i < j)
                    for (k = j; k > i + 1; k--) blocks[k] = blocks[k - 1];
                else
                    for (k = j; k < i; k++) blocks[k] = blocks[k + 1];
                blocks[i < j ? i + 1 : i] = tmp;
                moved = TRUE;
                break;
            }
        }
    }
}

/* Arrange a side test between two arms which join the same following block:

       A, B, C, M       A, C, B, M
       A -> M           C -> A or B
       B -> M     =>    B -> M (fallthrough)
       C -> A or B

   A still jumps to M, while C gains a fallthrough arm and B gains a
   fallthrough merge. This cannot add a transfer on any path. Phi-bearing arms
   are excluded because their incoming edges would require copy stubs. */
static int build_block_layout(emitblock *blocks)
{
    int i, n = 0;
    for (i = 0; i < n_blocks; i++)
        if (blkinfo[i].emit_body)
            blocks[n++].bi = i;

    for (i = 0; i + 3 < n; i++) {
        int ai = blocks[i].bi, bi = blocks[i + 1].bi;
        int ci = blocks[i + 2].bi, mi = blocks[i + 3].bi;
        LLVMBasicBlockRef a = blkinfo[ai].bb, b = blkinfo[bi].bb;
        LLVMBasicBlockRef c = blkinfo[ci].bb, m = blkinfo[mi].bb;
        LLVMValueRef at, bt, ct, cond;
        LLVMBasicBlockRef cs0, cs1;

        if (ai == 0 || phi_count(a) != 0 || phi_count(b) != 0)
            continue;
        at = LLVMGetBasicBlockTerminator(a);
        bt = LLVMGetBasicBlockTerminator(b);
        ct = LLVMGetBasicBlockTerminator(c);
        if (!at || !bt || !ct
            || LLVMGetInstructionOpcode(at) != LLVMBr
            || LLVMGetInstructionOpcode(bt) != LLVMBr
            || LLVMGetInstructionOpcode(ct) != LLVMBr
            || LLVMIsConditional(at) || LLVMIsConditional(bt)
            || !LLVMIsConditional(ct))
            continue;
        if (LLVMGetSuccessor(at, 0) != m || LLVMGetSuccessor(bt, 0) != m)
            continue;
        cs0 = LLVMGetSuccessor(ct, 0);
        cs1 = LLVMGetSuccessor(ct, 1);
        if (!((cs0 == a && cs1 == b) || (cs0 == b && cs1 == a)))
            continue;
        cond = LLVMGetCondition(ct);
        if (LLVMIsAConstantInt(cond) || LLVMIsUndef(cond) || LLVMIsPoison(cond))
            continue;

        blocks[i + 1].bi = ci;
        blocks[i + 2].bi = bi;
        i += 2;
    }

    straighten_layout(blocks, n);
    return n;
}

/* A return block whose every incoming edge is inlined (a goto-side
   "return v", an rfalse/rtrue branch encoding, or a ret-phi stub) has no
   remaining reference; dropping its body may create new fallthroughs, so
   re-plan until stable. TRUE if any body was dropped. */
static int drop_unreferenced_returns(emitplan *plan)
{
    int i, k, changed = FALSE;
    for (i = 0; i < plan->n_blocks; i++) {
        blockinfo *sb = &blkinfo[plan->blocks[i].bi];
        if (!sb->emit_body || plan->blocks[i].bi == 0) continue;
        if (!sb->ret_inline && !sb->ret_phi) continue;
        {   int referenced = FALSE;
            for (k = 0; k < plan->n_blocks && !referenced; k++) {
                emitblock *bp = &plan->blocks[k];
                int e;
                for (e = 0; e < bp->n_edges; e++) {
                    edgeplan *ep = &bp->edges[e];
                    if (ep->succ != sb->bb) continue;
                    if (ep->kind == EDGE_FALLTHROUGH
                        || (ep->kind == EDGE_DIRECT && ep->target_label >= 0)
                        || (ep->kind == EDGE_PHI_STUB && !ep->return_value)) {
                        referenced = TRUE;
                        break;
                    }
                }
            }
            if (!referenced) {
                sb->emit_body = FALSE;
                changed = TRUE;
            }
        }
    }
    return changed;
}

static void plan_layout_pass(emitplan *plan)
{
    int i, n_edges = 0, edge_pos = 0;
    plan->n_blocks = build_block_layout(plan->blocks);

    for (i = 0; i < plan->n_blocks; i++) {
        emitblock *bp = &plan->blocks[i];
        bp->term = LLVMGetBasicBlockTerminator(blkinfo[bp->bi].bb);
        bp->n_edges = (int)LLVMGetNumSuccessors(bp->term);
        n_edges += bp->n_edges;
    }

    for (i = 0; i < plan->n_blocks; i++) {
        emitblock *bp = &plan->blocks[i];
        LLVMBasicBlockRef next_bb = i + 1 < plan->n_blocks
            ? blkinfo[plan->blocks[i + 1].bi].bb : NULL;
        bp->edges = &plan->edges[edge_pos];
        edge_pos += bp->n_edges;
        plan_terminator(bp, next_bb);
    }
}

static void build_emit_plan(emitplan *plan)
{
    int i, n_edges = 0, edge_pos = 0, guard;
    memset(plan, 0, sizeof(*plan));
    plan->blocks = my_calloc(sizeof(emitblock), n_blocks,
        "llvm lower emission blocks");
    plan->n_blocks = build_block_layout(plan->blocks);

    for (i = 0; i < plan->n_blocks; i++) {
        emitblock *bp = &plan->blocks[i];
        bp->term = LLVMGetBasicBlockTerminator(blkinfo[bp->bi].bb);
        bp->n_edges = (int)LLVMGetNumSuccessors(bp->term);
        n_edges += bp->n_edges;
    }
    plan->edges = my_calloc(sizeof(edgeplan), n_edges ? n_edges : 1,
        "llvm lower emission edges");

    (void)edge_pos;
    for (guard = 0; guard < 8; guard++) {
        plan_layout_pass(plan);
        if (!drop_unreferenced_returns(plan))
            break;
    }
}

static void free_emit_plan(emitplan *plan)
{
    my_free(&plan->edges, "llvm lower emission edges");
    my_free(&plan->blocks, "llvm lower emission blocks");
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
    emitplan plan;

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
    if (!lower_failed) ret_select_pass();
    if (!lower_failed) branch_tree_pass();
    if (!lower_failed) {
        for (bb = LLVMGetFirstBasicBlock(fn); bb && !lower_failed;
             bb = LLVMGetNextBasicBlock(bb))
            for (in = LLVMGetFirstInstruction(bb); in && !lower_failed;
                 in = LLVMGetNextInstruction(in))
                validate(in);
    }
    if (!lower_failed) global_operand_pass();
    if (!lower_failed) store_fold_pass();
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

    llvm_lower_insts_in += llvm_shadow_instruction_count;
    *insts_in = llvm_shadow_instruction_count;
    n_emitted = 0;

    llvm_buffer_reset();
    for (i = 1; i < n_blocks; i++)
        blkinfo[i].label = alloc_label();

    build_emit_plan(&plan);
    for (i = 0; i < plan.n_blocks; i++) {
        emitblock *bp = &plan.blocks[i];
        int bi = bp->bi;
        bb = blkinfo[bi].bb;
        if (bi > 0)
            gen_label(blkinfo[bi].label);
        for (in = LLVMGetFirstInstruction(bb); in;
             in = LLVMGetNextInstruction(in)) {
            if (in == LLVMGetBasicBlockTerminator(bb))
                emit_terminator(in, bb, bp);
            else
                emit_instruction(in);
        }
    }

    llvm_lower_insts_out += n_emitted;
    *insts_out = n_emitted;

    free_emit_plan(&plan);
    my_free(&vals, "llvm lower values");
    my_free(&blkinfo, "llvm lower blocks");
    return TRUE;
}
