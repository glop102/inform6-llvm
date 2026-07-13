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
/*   Value allocation is deliberately naive: every SSA value that needs a    */
/*   runtime representation gets its own fresh Glulx local slot, written     */
/*   exactly once per execution of its definition. Function parameters      */
/*   keep their original local numbers; extra slots are appended and the     */
/*   routine header's locals count is patched. Correct first; compact       */
/*   later (M4).                                                             */
/*                                                                           */
/*   Lowering runs in two phases:                                            */
/*     A. classify + validate: every instruction is checked and assigned     */
/*        a slot, with no side effects on the compiler state, so an          */
/*        unsupported construct can still fall back to the classic replay;   */
/*     B. emit: cannot fail; rewrites the capture buffer.                    */
/* ------------------------------------------------------------------------- */

#include "header.h"
#include "llvm_codegen.h"

#include <llvm-c/Core.h>

/* The most local slots a lowered routine may use (Glulx has no hard limit
   below 255 per locals-format pair, but stay conservative). */
#define MAX_LOWER_SLOTS 118

/* The most phi-copy stub blocks one terminator may need. */
#define MAX_STUBS 64

/* ------------------------------------------------------------------------- */
/*   Lowering state                                                          */
/* ------------------------------------------------------------------------- */

typedef enum valkind_e {
    VK_SLOT,        /* value lives in a Glulx local slot                    */
    VK_FUSED,       /* icmp folded into its single branch/select user       */
    VK_ALIAS,       /* same runtime representation as another value         */
    VK_SKIP         /* no general runtime representation (i6.sym calls,     */
                    /*   void calls, stores, narrow truncs, terminators...) */
} valkind;

typedef struct valinfo_s {
    LLVMValueRef v;
    valkind kind;
    int slot;               /* VK_SLOT */
    int staging;            /* phi only: slot for parallel-copy staging     */
    LLVMValueRef alias_to;  /* VK_ALIAS */
} valinfo;

typedef struct blockinfo_s {
    LLVMBasicBlockRef bb;
    int label;              /* -1 for the entry block                       */
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

/* ------------------------------------------------------------------------- */
/*   Phase A, pass 1: classify every instruction and assign slots            */
/* ------------------------------------------------------------------------- */

static int alloc_slot(void)
{
    return next_slot++;
}

static void classify(LLVMValueRef in, valinfo *vi)
{
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);
    int w = int_width(in);

    vi->v = in;
    vi->kind = VK_SKIP;
    vi->slot = 0;
    vi->staging = 0;
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
        vi->kind = VK_SLOT; vi->slot = alloc_slot();
        return;

    case LLVMAnd: case LLVMOr: case LLVMXor:
        /* i1 logic works on the 0/1 slot representation; i1 *arithmetic*
           (add = xor etc.) would not, but instcombine canonicalizes it
           away and the case above rejects it. */
        if (w != 32 && w != 1) { lfail("narrow logic op"); return; }
        vi->kind = VK_SLOT; vi->slot = alloc_slot();
        return;

    case LLVMUDiv: case LLVMURem:
        lfail("unsigned division");
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
            if (icmp_fusable(in))
                vi->kind = VK_FUSED;
            else {
                vi->kind = VK_SLOT; vi->slot = alloc_slot();
            }
        }
        return;

    case LLVMSelect:
        if (w != 32 && w != 1) { lfail("narrow select"); return; }
        vi->kind = VK_SLOT; vi->slot = alloc_slot();
        return;

    case LLVMPHI:
        if (w != 32 && w != 1) { lfail("narrow phi"); return; }
        vi->kind = VK_SLOT;
        vi->slot = alloc_slot();
        vi->staging = alloc_slot();
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
                vi->kind = VK_SLOT; vi->slot = alloc_slot();
            }
            else lfail("zext from unhandled width");
        }
        return;

    case LLVMSExt:
        {   int sw = int_width(LLVMGetOperand(in, 0));
            if (w != 32) { lfail("sext to narrow type"); return; }
            if (sw == 1 || sw == 8 || sw == 16) {
                vi->kind = VK_SLOT; vi->slot = alloc_slot();
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
            vi->kind = VK_SLOT; vi->slot = alloc_slot();
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
            if (opc == LLVMLoad) {
                vi->kind = VK_SLOT; vi->slot = alloc_slot();
            }
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
                    vi->kind = VK_SLOT; vi->slot = alloc_slot();
                    return;
                }
                lfail("unhandled intrinsic");
                return;
            }

            if (strcmp(name, "i6.deref") == 0) {
                vi->kind = VK_SLOT; vi->slot = alloc_slot();
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
                if ((flags & OPFLAG_STORE) && LLVMGetFirstUse(in)) {
                    vi->kind = VK_SLOT; vi->slot = alloc_slot();
                }
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

static assembly_operand const_op(int32 v)
{
    assembly_operand o = mkop(CONSTANT_OT, 0);
    set_constant_otv(&o, v);
    return o;
}

static int32 const_int_value(LLVMValueRef v)
{   return (int32)(uint32_t)LLVMConstIntGetZExtValue(v);
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

/* Emit "branch to <label> if <cond> is true". cond is an i1 value: a
   fusable icmp becomes a native compare-branch, anything else is a 0/1
   slot tested with jnz. Constants are the caller's business. */
static void gen_cond_branch(LLVMValueRef cond, int label)
{
    valinfo *ci = lookup(cond);
    if (ci && ci->kind == VK_FUSED) {
        gen_branch2(pred_branch_opcode(LLVMGetICmpPredicate(cond)),
            resolve(LLVMGetOperand(cond, 0)),
            resolve(LLVMGetOperand(cond, 1)),
            label);
    }
    else
        gen_branch1(jnz_gc, resolve(cond), label);
}

/* Copies for the phis of succ along the edge from pred. Every phi gets
   its own slot; with more than one phi the copies are a parallel
   assignment (sources may be other phis' slots), so stage them first. */
static void emit_edge_copies(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ)
{
    LLVMValueRef phi;
    int n = phi_count(succ);

    if (n == 0) return;
    if (n == 1) {
        phi = LLVMGetFirstInstruction(succ);
        gen_copy(resolve(incoming_for(phi, pred)),
            slot_op(lookup(phi)->slot));
        return;
    }
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi))
        gen_copy(resolve(incoming_for(phi, pred)),
            slot_op(lookup(phi)->staging));
    for (phi = LLVMGetFirstInstruction(succ); phi && LLVMIsAPHINode(phi);
         phi = LLVMGetNextInstruction(phi))
        gen_copy(slot_op(lookup(phi)->staging),
            slot_op(lookup(phi)->slot));
}

/* Label to branch to for the edge pred->succ: the successor itself if the
   edge carries no phi copies, else a fresh stub (recorded for flushing
   after the terminator) that performs the copies and jumps on. */
typedef struct edgestub_s {
    int label;
    LLVMBasicBlockRef pred, succ;
} edgestub;

static edgestub stubs[MAX_STUBS];
static int n_stubs;

static int edge_target_label(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ)
{
    if (phi_count(succ) == 0)
        return block_info(succ)->label;
    if (n_stubs >= MAX_STUBS) {
        compiler_error("llvm_lower: stub overflow");
        return block_info(succ)->label;
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

/* An unconditional transfer to succ: edge copies inline, then a jump
   unless succ is the next block in layout order. */
static void emit_goto(LLVMBasicBlockRef pred, LLVMBasicBlockRef succ,
    LLVMBasicBlockRef next_bb)
{
    emit_edge_copies(pred, succ);
    if (succ != next_bb)
        gen_jump(block_info(succ)->label);
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
    assembly_operand tv = resolve(LLVMGetOperand(in, 1));
    assembly_operand fv = resolve(LLVMGetOperand(in, 2));
    int done;

    if (LLVMIsAConstantInt(cond)) {
        gen_copy((const_int_value(cond) & 1) ? tv : fv, slot_op(vi->slot));
        return;
    }
    if (LLVMIsUndef(cond) || LLVMIsPoison(cond)) {
        gen_copy(fv, slot_op(vi->slot));
        return;
    }
    done = alloc_label();
    gen_copy(tv, slot_op(vi->slot));
    gen_cond_branch(cond, done);
    gen_copy(fv, slot_op(vi->slot));
    gen_label(done);
}

static void emit_binop(LLVMValueRef in, valinfo *vi)
{
    int op;
    switch (LLVMGetInstructionOpcode(in)) {
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
    default:
        compiler_error("llvm_lower: bad binop");
        return;
    }
    gen3(op, resolve(LLVMGetOperand(in, 0)), resolve(LLVMGetOperand(in, 1)),
        slot_op(vi->slot));
}

/* The global variable behind an i6.g<index> pointer, as an operand. */
static assembly_operand global_op(LLVMValueRef ptr)
{
    size_t len;
    const char *name = LLVMGetValueName2(ptr, &len);
    int idx = atoi(name + 4);
    return mkop(GLOBALVAR_OT, MAX_LOCAL_VARIABLES + idx);
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
        ops[n_src] = (vi->kind == VK_SLOT)
            ? slot_op(vi->slot)
            : mkop(ZEROCONSTANT_OT, 0);   /* discard */
    genop(opnum, opct, ops);
}

static void emit_instruction(LLVMValueRef in)
{
    valinfo *vi = lookup(in);
    LLVMOpcode opc = LLVMGetInstructionOpcode(in);

    switch (opc) {
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMSDiv: case LLVMSRem:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
        emit_binop(in, vi);
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
            if (name_has_prefix(name, "llvm.")) {
                emit_minmax(in, vi, name);
                return;
            }
            if (strcmp(name, "i6.deref") == 0) {
                gen_copy(deref_op(LLVMGetOperand(in, 0)), slot_op(vi->slot));
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
            gen_cond_branch(cond, edge_target_label(cur, then_bb));
            /* If a stub is pending it is emitted right below, in the
               fallthrough path — so the else edge must jump explicitly. */
            emit_goto(cur, else_bb, n_stubs ? NULL : next_bb);
            flush_stubs();
        }
        return;

    case LLVMSwitch:
        {   assembly_operand cond = resolve(LLVMGetOperand(term, 0));
            LLVMBasicBlockRef defbb = LLVMGetSwitchDefaultDest(term);
            int nops = LLVMGetNumOperands(term);
            int c;
            for (c = 2; c < nops; c += 2) {
                LLVMBasicBlockRef dest =
                    LLVMValueAsBasicBlock(LLVMGetOperand(term, c + 1));
                gen_branch2(jeq_gc, cond,
                    resolve(LLVMGetOperand(term, c)),
                    edge_target_label(cur, dest));
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
    for (bb = LLVMGetFirstBasicBlock(fn); bb && !lower_failed;
         bb = LLVMGetNextBasicBlock(bb)) {
        for (in = LLVMGetFirstInstruction(bb); in && !lower_failed;
             in = LLVMGetNextInstruction(in)) {
            classify(in, &vals[n_vals]);
            n_vals++;
        }
    }
    if (!lower_failed && next_slot - 1 > MAX_LOWER_SLOTS)
        lfail("too many local slots");
    if (!lower_failed) {
        for (bb = LLVMGetFirstBasicBlock(fn); bb && !lower_failed;
             bb = LLVMGetNextBasicBlock(bb))
            for (in = LLVMGetFirstInstruction(bb); in && !lower_failed;
                 in = LLVMGetNextInstruction(in))
                validate(in);
    }

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
        LLVMBasicBlockRef next_bb = (i + 1 < n_blocks) ? blkinfo[i+1].bb : NULL;
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
