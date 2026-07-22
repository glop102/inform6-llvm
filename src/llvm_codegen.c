/* ------------------------------------------------------------------------- */
/*   "llvm_codegen" : Direct Glulx LLVM IR generation                        */
/*                                                                           */
/*   Part of inform6-llvm. Not part of the upstream Inform 6 compiler.       */
/*                                                                           */
/*   Expression and statement parsing build one LLVM function per routine,   */
/*   all in a single module shared by the whole compile. When a routine is   */
/*   processed (at end of pass under deferred lowering), its function is     */
/*   verified, run through LLVM's function passes, legalized into shapes     */
/*   the lowerer accepts, and handed to llvm_lower.c, which turns it into    */
/*   buffered Glulx instructions for the classic encoder.                    */
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

static LLVMContextRef llctx;        /* created lazily, lives all compile     */
static LLVMModuleRef  llmod;        /* the one module holding every routine  */
static FILE *dump_file;             /* diagnostics dump, opened lazily       */

static int no_routines_direct;
static int no_routines_classic;
static int no_routines_direct_fallback;
static int no_routines_direct_build_failed;  /* rejected before lowering     */
static int no_routines_direct_lower_failed;  /* built IR the lowerer refused */

static int llvm_diagnostics_enabled(void)
{
    const char *value = getenv("I6_LLVM_DIAGNOSTICS");
    return value && value[0] && strcmp(value, "0") != 0;
}

/* The optimization pipeline (new pass manager syntax), run per routine
   function. All IR-level optimization belongs here (or in the
   legalization step below) — the lowerer is a plain translator. */
#define LLVM_PASS_PIPELINE \
    "mem2reg,instcombine,simplifycfg," \
    "loop-mssa(licm),gvn,jump-threading,dce,simplifycfg"

/* The i32 type; context-owned, so it is stable for the whole compile. */
static LLVMTypeRef    i32t;

enum direct_state_e {
    DIRECT_INACTIVE, DIRECT_BUILDING, DIRECT_REJECTED, DIRECT_READY
};

/* Inline assembly's "sp" operands ride a parse-time symbolic stack of
   SSA values. A symbolic value has no runtime stack presence, so
   wherever control flow diverges or joins the pending values are
   spilled to the real VM stack (real pushes at the same source point
   classic pushed); sp reads past the symbolic window then pop the real
   stack, exactly as classic's would. */
#define DIRECT_SYMSTACK_MAX 64

/* All per-routine IR-build state, gathered into one container. Exactly
   one routine is being built at a time (`direct_ru`, reset at each
   routine_begin); a finished routine's function is retained past parse
   time as one of these so it can be lowered at end of pass. */
typedef struct routine_unit_s {
    enum direct_state_e state;
    LLVMBuilderRef bld;
    LLVMValueRef   fn;
    LLVMValueRef  *locals;
    int            local_count;
    LLVMBasicBlockRef *labels;
    int            label_count;
    int            suspended;
    const char    *reason;
    LLVMValueRef   symstack[DIRECT_SYMSTACK_MAX];
    int            symstack_depth;
    int            spilled;      /* values our code left on the real stack */
    int            routine_symbol;
} routine_unit;

/* The one routine currently being built. */
static routine_unit direct_ru;

static LLVMValueRef direct_opaque_call_ex(const char *name,
    LLVMValueRef *args, int count, int void_return);

static LLVMValueRef direct_sympop(void)
{
    if (direct_ru.symstack_depth > 0)
        return direct_ru.symstack[--direct_ru.symstack_depth];
    if (direct_ru.spilled > 0) {
        direct_ru.spilled--;
        return direct_opaque_call_ex("i6.stkpop", NULL, 0, FALSE);
    }
    llvm_direct_reject("sp read with empty symbolic stack");
    return NULL;
}

static void direct_sympush(LLVMValueRef v)
{
    if (direct_ru.symstack_depth >= DIRECT_SYMSTACK_MAX) {
        llvm_direct_reject("symbolic stack overflow");
        return;
    }
    direct_ru.symstack[direct_ru.symstack_depth++] = v;
}

/* Materialize pending symbolic values onto the real VM stack (bottom
   first) ahead of a control-flow point. TRUE if the stack is clear. */
static int direct_symstack_spill(void)
{
    LLVMBasicBlockRef bb;
    int i;
    if (direct_ru.symstack_depth == 0) return TRUE;
    bb = LLVMGetInsertBlock(direct_ru.bld);
    if (!bb || LLVMGetBasicBlockTerminator(bb)) {
        llvm_direct_reject("VM stack value carried across control flow");
        return FALSE;
    }
    for (i = 0; i < direct_ru.symstack_depth; i++)
        (void)direct_opaque_call_ex("i6.stkpush",
            &direct_ru.symstack[i], 1, TRUE);
    direct_ru.spilled += direct_ru.symstack_depth;
    direct_ru.symstack_depth = 0;
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/*   Helpers for names, declarations, and operand access                     */
/* ------------------------------------------------------------------------- */

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
/*   back into VM code; their opaque declarations carry the matching         */
/*   memory attributes. Stream opcodes are deliberately absent: under        */
/*   @setiosys 1 ("filter") every one of them invokes an arbitrary           */
/*   routine per character.                                                  */
/* ------------------------------------------------------------------------- */

/* Pure functions of their operands (Glulx float math is deterministic
   and cannot fault). */
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

static void mark_opaque_fn_attrs(LLVMValueRef f, const char *opname)
{
    if (strcmp(opname, "deref") == 0)
        mark_fn_as_readonly(f);
    else if (name_in_list(opname, pure_opcodes))
        mark_fn_as_constant(f);
    else if (name_in_list(opname, readonly_opcodes))
        mark_fn_as_readonly(f);
    else if (name_in_list(opname, inaccessible_opcodes))
        mark_fn_as_inaccessible_rw(f);
}

/* ------------------------------------------------------------------------- */
/*   Post-optimization cleanup                                               */
/*                                                                           */
/*   simplifycfg/instcombine merge conditional reads of two globals into a   */
/*   load of a select-of-pointers, which has no Glulx encoding. Rewrite      */
/*   such loads back into selects of loads (globals are always readable,     */
/*   so hoisting the loads is safe), leaving IR the lowerer understands.     */
/* ------------------------------------------------------------------------- */

/* The optimizer likes to merge accesses of different globals into a
   load/store through a select or phi of global POINTERS (instcombine,
   jump-threading and simplifycfg all do it).  The lowerer's operand
   model has no indirect global access, so this rewrite de-pointerizes
   them: every select/phi network whose leaves are all i6.g globals is
   mirrored by an i32 "tag" network carrying the leaf's index, loads
   become tag-driven select chains of direct loads, and stores write
   every candidate global with either the new value or its own current
   value.  Extra reads/rewrites of plain VM globals are unobservable. */

#define UNMERGE_MAX 24

static LLVMValueRef unm_nodes[UNMERGE_MAX];    /* ptr selects and phis */
static LLVMValueRef unm_tags[UNMERGE_MAX];     /* their i32 mirrors */
static int unm_node_count;
static LLVMValueRef unm_globals[UNMERGE_MAX];  /* distinct leaf globals */
static int unm_global_count;

static int unm_node_index(LLVMValueRef p)
{
    int i;
    for (i = 0; i < unm_node_count; i++)
        if (unm_nodes[i] == p) return i;
    return -1;
}

static int unm_global_index(LLVMValueRef g)
{
    int i;
    for (i = 0; i < unm_global_count; i++)
        if (unm_globals[i] == g) return i;
    return -1;
}

/* Validate the network reachable from p, registering nodes and leaf
   globals. Phis may be cyclic; a node being visited counts as valid. */
static int unm_validate(LLVMValueRef p)
{
    int idx, i, n;
    if (LLVMIsAGlobalVariable(p)) {
        size_t len;
        const char *name = LLVMGetValueName2(p, &len);
        if (!name || strncmp(name, "i6.g", 4) != 0) return FALSE;
        if (unm_global_index(p) < 0) {
            if (unm_global_count >= UNMERGE_MAX) return FALSE;
            unm_globals[unm_global_count++] = p;
        }
        return TRUE;
    }
    if (!LLVMIsASelectInst(p) && !LLVMIsAPHINode(p)) return FALSE;
    idx = unm_node_index(p);
    if (idx >= 0) return TRUE;
    if (unm_node_count >= UNMERGE_MAX) return FALSE;
    idx = unm_node_count++;
    unm_nodes[idx] = p;
    unm_tags[idx] = NULL;
    if (LLVMIsASelectInst(p))
        return unm_validate(LLVMGetOperand(p, 1))
            && unm_validate(LLVMGetOperand(p, 2));
    n = LLVMCountIncoming(p);
    for (i = 0; i < n; i++)
        if (!unm_validate(LLVMGetIncomingValue(p, i))) return FALSE;
    return TRUE;
}

/* The i32 tag mirroring pointer value p, built beside p's definition.
   Phi tags are created empty by the caller before any operands are
   resolved, so cycles terminate here. */
static LLVMValueRef unm_tag_of(LLVMBuilderRef b, LLVMValueRef p)
{
    int idx;
    if (LLVMIsAGlobalVariable(p))
        return LLVMConstInt(i32t, (unsigned)unm_global_index(p), FALSE);
    idx = unm_node_index(p);
    if (unm_tags[idx]) return unm_tags[idx];
    /* Selects only: phi tags were pre-created.  Resolve both child
       tags before positioning: recursion moves the builder, and the
       mirror select must sit exactly at p to keep dominance. */
    {   LLVMValueRef t1 = unm_tag_of(b, LLVMGetOperand(p, 1));
        LLVMValueRef t2 = unm_tag_of(b, LLVMGetOperand(p, 2));
        LLVMPositionBuilderBefore(b, p);
        unm_tags[idx] = LLVMBuildSelect(b, LLVMGetOperand(p, 0),
            t1, t2, "unmerge.tag");
    }
    return unm_tags[idx];
}

/* Prepare the tag network for the already-validated pointer p. */
static LLVMValueRef unm_build_tags(LLVMBuilderRef b, LLVMValueRef root)
{
    int i, j, n;
    /* Empty phi tags first, so cyclic references resolve. */
    for (i = 0; i < unm_node_count; i++) {
        if (!LLVMIsAPHINode(unm_nodes[i]) || unm_tags[i]) continue;
        LLVMPositionBuilderBefore(b, unm_nodes[i]);
        unm_tags[i] = LLVMBuildPhi(b, i32t, "unmerge.tag");
    }
    for (i = 0; i < unm_node_count; i++) {
        if (!LLVMIsAPHINode(unm_nodes[i])) continue;
        if (LLVMCountIncoming(unm_tags[i]) > 0) continue;
        n = LLVMCountIncoming(unm_nodes[i]);
        for (j = 0; j < n; j++) {
            LLVMValueRef iv = unm_tag_of(b,
                LLVMGetIncomingValue(unm_nodes[i], j));
            LLVMBasicBlockRef ib = LLVMGetIncomingBlock(unm_nodes[i], j);
            LLVMAddIncoming(unm_tags[i], &iv, &ib, 1);
        }
    }
    return unm_tag_of(b, root);
}

static void unmerge_pointer_selects(LLVMValueRef f)
{
    LLVMBuilderRef b = LLVMCreateBuilderInContext(llctx);
    LLVMBasicBlockRef bb;
    LLVMValueRef in, next;
    int changed, i;

    for (bb = LLVMGetFirstBasicBlock(f); bb; bb = LLVMGetNextBasicBlock(bb)) {
        for (in = LLVMGetFirstInstruction(bb); in; in = next) {
            LLVMValueRef ptr, tag, v;
            int is_load;
            next = LLVMGetNextInstruction(in);
            if (LLVMIsALoadInst(in)) {
                if (LLVMTypeOf(in) != i32t) continue;
                ptr = LLVMGetOperand(in, 0);
                is_load = TRUE;
            }
            else if (LLVMIsAStoreInst(in)) {
                if (LLVMTypeOf(LLVMGetOperand(in, 0)) != i32t) continue;
                ptr = LLVMGetOperand(in, 1);
                is_load = FALSE;
            }
            else continue;
            if (LLVMIsAGlobalVariable(ptr)) continue;
            unm_node_count = 0;
            unm_global_count = 0;
            if (!unm_validate(ptr) || unm_global_count < 1) continue;
            tag = unm_build_tags(b, ptr);
            LLVMPositionBuilderBefore(b, in);
            if (is_load) {
                /* select(tag==0, load g0, select(tag==1, load g1, ...)) */
                v = LLVMBuildLoad2(b, i32t,
                    unm_globals[unm_global_count - 1], "");
                for (i = unm_global_count - 2; i >= 0; i--) {
                    LLVMValueRef cmp = LLVMBuildICmp(b, LLVMIntEQ, tag,
                        LLVMConstInt(i32t, (unsigned)i, FALSE), "");
                    v = LLVMBuildSelect(b, cmp,
                        LLVMBuildLoad2(b, i32t, unm_globals[i], ""),
                        v, "");
                }
                LLVMReplaceAllUsesWith(in, v);
            }
            else {
                /* Each candidate takes the new value or keeps its own. */
                LLVMValueRef stored = LLVMGetOperand(in, 0);
                for (i = 0; i < unm_global_count; i++) {
                    LLVMValueRef cmp = LLVMBuildICmp(b, LLVMIntEQ, tag,
                        LLVMConstInt(i32t, (unsigned)i, FALSE), "");
                    LLVMValueRef cur = LLVMBuildLoad2(b, i32t,
                        unm_globals[i], "");
                    v = LLVMBuildSelect(b, cmp, stored, cur, "");
                    LLVMBuildStore(b, v, unm_globals[i]);
                }
            }
            LLVMInstructionEraseFromParent(in);
        }
    }
    /* Sweep the now-dead pointer selects and phis (innermost last). */
    do {
        changed = FALSE;
        for (bb = LLVMGetFirstBasicBlock(f); bb;
             bb = LLVMGetNextBasicBlock(bb)) {
            for (in = LLVMGetFirstInstruction(bb); in; in = next) {
                next = LLVMGetNextInstruction(in);
                if ((LLVMIsASelectInst(in) || LLVMIsAPHINode(in))
                    && !LLVMGetFirstUse(in)
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

extern int llvm_codegen_available(void)
{
    return TRUE;
}

extern void llvm_note_classic_routine(const char *reason)
{
    const char *p = reason ? reason : "policy";
    no_routines_classic++;
    if (!llvm_diagnostics_enabled())
        return;
    printf("LLVM-BACKEND\tname=%s\tbackend=classic\tstage=policy"
           "\tinput=-1\temitted=-1\treason=",
        llvm_current_routine_name());
    for (; *p; p++)
        putchar((*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p);
    putchar('\n');
}

/* Discard the routine being built (or just lowered): its function is
   deleted from the shared module. Routines never reference each other's
   functions directly (calls are opaque i6.callf* operations), so a
   deletion is always safe. */
static void direct_dispose_ir(void)
{
    if (direct_ru.bld) {
        LLVMDisposeBuilder(direct_ru.bld);
        direct_ru.bld = NULL;
    }
    if (direct_ru.fn) {
        LLVMDeleteFunction(direct_ru.fn);
        direct_ru.fn = NULL;
    }
    free(direct_ru.locals);
    direct_ru.locals = NULL;
    free(direct_ru.labels);
    direct_ru.labels = NULL;
    direct_ru.local_count = 0;
    direct_ru.label_count = 0;
}

extern void llvm_direct_reject(const char *reason)
{
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return;
    direct_ru.reason = reason;
    direct_dispose_ir();
    direct_ru.state = DIRECT_REJECTED;
}

extern void llvm_direct_routine_begin(const char *name, int local_count,
    int embedded_flag, int stack_arguments)
{
    (void)embedded_flag;
    direct_ru.state = DIRECT_INACTIVE;
    direct_ru.reason = NULL;
    direct_ru.suspended = 0;
    direct_ru.symstack_depth = 0;
    direct_ru.spilled = 0;
    llvm_direct_expression_reset();
    if (!LLVM_CODEGEN)
        return;
    if (stack_arguments) {
        direct_ru.state = DIRECT_REJECTED;
        direct_ru.reason = "stack-argument routine";
        return;
    }

    if (!llctx) {
        llctx = LLVMContextCreate();
        i32t = LLVMInt32TypeInContext(llctx);
        llmod = LLVMModuleCreateWithNameInContext("i6", llctx);
        LLVMSetDataLayout(llmod, "e-p:32:32-i32:32-n32");
    }
    {   LLVMTypeRef *params = local_count
            ? malloc((size_t)local_count * sizeof(*params)) : NULL;
        LLVMTypeRef ft;
        char *fname;
        int i;
        if (local_count && !params)
            fatalerror("Out of memory creating direct LLVM parameters");
        for (i = 0; i < local_count; i++)
            params[i] = i32t;
        ft = LLVMFunctionType(i32t, params, (unsigned)local_count, FALSE);
        /* The "i6fn." prefix keeps routine names out of the "i6.*"
           helper-declaration namespace; LLVM uniquifies duplicates. */
        fname = malloc(strlen(name) + 7);
        if (!fname)
            fatalerror("Out of memory creating direct LLVM function");
        sprintf(fname, "i6fn.%s", name);
        direct_ru.fn = LLVMAddFunction(llmod, fname, ft);
        free(fname);
        free(params);
    }
    direct_ru.bld = LLVMCreateBuilderInContext(llctx);
    LLVMPositionBuilderAtEnd(direct_ru.bld,
        LLVMAppendBasicBlockInContext(llctx, direct_ru.fn, "entry"));
    direct_ru.local_count = local_count;
    direct_ru.locals = local_count
        ? calloc((size_t)local_count + 1, sizeof(*direct_ru.locals)) : NULL;
    if (local_count && !direct_ru.locals)
        fatalerror("Out of memory creating direct LLVM locals");
    {   int i;
        for (i = 1; i <= local_count; i++) {
            direct_ru.locals[i] = LLVMBuildAlloca(direct_ru.bld, i32t, "local");
            LLVMBuildStore(direct_ru.bld,
                LLVMGetParam(direct_ru.fn, (unsigned)(i-1)), direct_ru.locals[i]);
        }
    }
    direct_ru.state = DIRECT_BUILDING;
}

extern void llvm_direct_routine_finish(int embedded_flag,
    int fallthrough_reachable)
{
    LLVMBasicBlockRef bb;
    if (direct_ru.state != DIRECT_BUILDING)
        return;
    bb = LLVMGetInsertBlock(direct_ru.bld);
    if (fallthrough_reachable) {
        if (!bb || LLVMGetBasicBlockTerminator(bb)) {
            llvm_direct_reject("invalid fallthrough block");
            return;
        }
        LLVMBuildRet(direct_ru.bld, LLVMConstInt(i32t,
            embedded_flag ? 0 : 1, FALSE));
    }
    else if (!bb || !LLVMGetBasicBlockTerminator(bb)) {
        /* The front end says fallthrough is unreachable, but the builder is
           parked on an open block.  Two benign shapes end this way: a
           compiler label bound at an unreachable point (if/else where both
           arms return) and a continuation only entered through branches the
           front end folded to constants.  Classic emitted no code on these
           paths, so sealing the block with unreachable is exact.  A single
           empty entry block instead means the body was raw-assembled behind
           the builder's back, which every emitter must announce; reject so
           a missed announcement cannot silently drop a routine body. */
        if (!bb || (LLVMGetEntryBasicBlock(direct_ru.fn) == bb
                && !LLVMGetNextBasicBlock(bb))) {
            llvm_direct_reject("empty direct body");
            return;
        }
        LLVMBuildUnreachable(direct_ru.bld);
    }
    LLVMDisposeBuilder(direct_ru.bld);
    direct_ru.bld = NULL;
    direct_ru.state = DIRECT_READY;
}

extern void llvm_direct_routine_abandon(void)
{
    if (LLVM_CODEGEN && direct_ru.state != DIRECT_INACTIVE) {
        no_routines_direct_fallback++;
        if (llvm_diagnostics_enabled())
            printf("LLVM-BACKEND\tname=%s\tbackend=classic-fallback"
                   "\tstage=direct-abandon\tinput=-1\temitted=-1"
                   "\treason=source-errors\n",
                llvm_current_routine_name());
    }
    direct_dispose_ir();
    direct_ru.state = DIRECT_INACTIVE;
    direct_ru.reason = NULL;
}

static int direct_can_emit(void)
{
    LLVMBasicBlockRef bb;
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return FALSE;
    bb = LLVMGetInsertBlock(direct_ru.bld);
    if (!bb || LLVMGetBasicBlockTerminator(bb)) {
        if (bb && execution_never_reaches_here)
            return FALSE;
        llvm_direct_reject("operation after terminator");
        return FALSE;
    }
    return TRUE;
}

extern int llvm_direct_can_generate(void)
{
    LLVMBasicBlockRef bb;
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return FALSE;
    bb = LLVMGetInsertBlock(direct_ru.bld);
    if (!bb)
        return FALSE;
    if (!LLVMGetBasicBlockTerminator(bb))
        return TRUE;
    return !execution_never_reaches_here;
}

extern void llvm_direct_suspend(void)
{
    if (direct_ru.state == DIRECT_BUILDING)
        direct_ru.suspended++;
}

extern void llvm_direct_resume(void)
{
    if (direct_ru.state == DIRECT_BUILDING && direct_ru.suspended > 0)
        direct_ru.suspended--;
}

static LLVMValueRef direct_global_var(int32 index)
{
    char name[32];
    LLVMValueRef g;
    sprintf(name, "i6.g%d", (int)index);
    g = LLVMGetNamedGlobal(llmod, name);
    if (!g)
        g = LLVMAddGlobal(llmod, i32t, name);
    return g;
}

extern llvm_direct_value llvm_direct_constant(int32 value, int marker,
    int32 symindex)
{
    LLVMTypeRef params[3], ft;
    LLVMValueRef f, args[3];
    int is_new;
    if (!direct_can_emit()) return NULL;
    if (!marker)
        return LLVMConstInt(i32t, (uint32_t)value, FALSE);

    params[0] = i32t; params[1] = i32t; params[2] = i32t;
    ft = LLVMFunctionType(i32t, params, 3, FALSE);
    f = LLVMGetNamedFunction(llmod, "i6.sym");
    is_new = (f == NULL);
    if (!f)
        f = LLVMAddFunction(llmod, "i6.sym", ft);
    if (is_new)
        mark_fn_as_constant(f);
    args[0] = LLVMConstInt(i32t, (unsigned)marker, FALSE);
    args[1] = LLVMConstInt(i32t, (uint32_t)value, FALSE);
    args[2] = LLVMConstInt(i32t, (uint32_t)symindex, FALSE);
    return LLVMBuildCall2(direct_ru.bld, ft, f, args, 3, "sym");
}

extern llvm_direct_value llvm_direct_load_local(int source)
{
    if (!direct_can_emit()) return NULL;
    if (source < 1 || source > direct_ru.local_count) {
        llvm_direct_reject("invalid local expression");
        return NULL;
    }
    return LLVMBuildLoad2(direct_ru.bld, i32t, direct_ru.locals[source], "local");
}

extern llvm_direct_value llvm_direct_load_global(int source)
{
    if (!direct_can_emit()) return NULL;
    if (source < MAX_LOCAL_VARIABLES) {
        llvm_direct_reject("invalid global expression");
        return NULL;
    }
    return LLVMBuildLoad2(direct_ru.bld, i32t,
        direct_global_var(source - MAX_LOCAL_VARIABLES), "global");
}

extern llvm_direct_value llvm_direct_unary(int operator_number,
    llvm_direct_value operand)
{
    if (!direct_can_emit() || !operand) return NULL;
    switch (operator_number) {
    case UNARY_MINUS_OP:
        return LLVMBuildNeg(direct_ru.bld, operand, "neg");
    case ARTNOT_OP:
        return LLVMBuildNot(direct_ru.bld, operand, "not");
    default:
        llvm_direct_reject("unsupported unary expression operator");
        return NULL;
    }
}

extern llvm_direct_value llvm_direct_binary(int operator_number,
    llvm_direct_value left, llvm_direct_value right)
{
    if (!direct_can_emit() || !left || !right) return NULL;
    switch (operator_number) {
    case PLUS_OP:
        return LLVMBuildAdd(direct_ru.bld, left, right, "add");
    case MINUS_OP:
        return LLVMBuildSub(direct_ru.bld, left, right, "sub");
    case TIMES_OP:
        return LLVMBuildMul(direct_ru.bld, left, right, "mul");
    case ARTAND_OP:
        return LLVMBuildAnd(direct_ru.bld, left, right, "and");
    case ARTOR_OP:
        return LLVMBuildOr(direct_ru.bld, left, right, "or");
    default:
        llvm_direct_reject("unsupported binary expression operator");
        return NULL;
    }
}

static LLVMValueRef direct_opaque_call_ex(const char *name, LLVMValueRef *args,
    int count, int void_return)
{
    LLVMTypeRef params[16], ft;
    LLVMValueRef f;
    int i, is_new;
    for (i = 0; i < count; i++)
        params[i] = i32t;
    ft = LLVMFunctionType(void_return ? LLVMVoidTypeInContext(llctx) : i32t,
        params, (unsigned)count, FALSE);
    f = LLVMGetNamedFunction(llmod, name);
    is_new = (f == NULL);
    if (!f)
        f = LLVMAddFunction(llmod, name, ft);
    if (is_new)
        mark_opaque_fn_attrs(f, name + 3);
    return LLVMBuildCall2(direct_ru.bld, ft, f, args, (unsigned)count,
        void_return ? "" : "opaque");
}

static LLVMValueRef direct_opaque_call(const char *name, LLVMValueRef *args,
    int count)
{
    return direct_opaque_call_ex(name, args, count, FALSE);
}

extern llvm_direct_value llvm_direct_division(int operator_number,
    llvm_direct_value left, llvm_direct_value right, int check_zero)
{
    const char *name = operator_number == DIVIDE_OP ? "i6.div" : "i6.mod";
    LLVMValueRef args[3], condition, error_fn, divisor = right;
    if (!direct_can_emit() || !left || !right) return NULL;
    if (operator_number != DIVIDE_OP && operator_number != REMAINDER_OP) {
        llvm_direct_reject("invalid division operator");
        return NULL;
    }
    if (check_zero) {
        assembly_operand error_routine = veneer_routine(RT__Err_VR);
        LLVMBasicBlockRef error = LLVMAppendBasicBlockInContext(
            llctx, direct_ru.fn, "divide.zero");
        LLVMBasicBlockRef valid = LLVMAppendBasicBlockInContext(
            llctx, direct_ru.fn, "divide.valid");
        LLVMBasicBlockRef join = LLVMAppendBasicBlockInContext(
            llctx, direct_ru.fn, "divide.join");
        LLVMValueRef incoming[2];
        LLVMBasicBlockRef blocks[2];

        condition = LLVMBuildICmp(direct_ru.bld, LLVMIntEQ, right,
            LLVMConstInt(i32t, 0, FALSE), "divide.iszero");
        LLVMBuildCondBr(direct_ru.bld, condition, error, valid);

        LLVMPositionBuilderAtEnd(direct_ru.bld, error);
        error_fn = llvm_direct_constant(error_routine.value,
            error_routine.marker, error_routine.symindex);
        args[0] = error_fn;
        args[1] = LLVMConstInt(i32t, 1, FALSE);
        args[2] = LLVMConstInt(i32t, DBYZERO_RTE, FALSE);
        (void)direct_opaque_call("i6.call.s", args, 3);
        LLVMBuildBr(direct_ru.bld, join);

        LLVMPositionBuilderAtEnd(direct_ru.bld, valid);
        LLVMBuildBr(direct_ru.bld, join);

        LLVMPositionBuilderAtEnd(direct_ru.bld, join);
        divisor = LLVMBuildPhi(direct_ru.bld, i32t, "divide.divisor");
        incoming[0] = LLVMConstInt(i32t, 1, FALSE);
        incoming[1] = right;
        blocks[0] = error;
        blocks[1] = valid;
        LLVMAddIncoming(divisor, incoming, blocks, 2);
    }
    else if (LLVMIsAConstantInt(right)) {
        int32 value = (int32)LLVMConstIntGetSExtValue(right);
        if (value != 0 && value != -1)
            return LLVMBuildBinOp(direct_ru.bld,
                operator_number == DIVIDE_OP ? LLVMSDiv : LLVMSRem,
                left, right, "arithmetic");
    }
    args[0] = left; args[1] = divisor;
    return direct_opaque_call(name, args, 2);
}

extern llvm_direct_value llvm_direct_compare(int operator_number,
    llvm_direct_value left, llvm_direct_value right)
{
    LLVMIntPredicate predicate;
    LLVMValueRef condition;
    if (!direct_can_emit() || !left || !right) return NULL;
    switch (operator_number) {
    case CONDEQUALS_OP: predicate = LLVMIntEQ;  break;
    case NOTEQUAL_OP:   predicate = LLVMIntNE;  break;
    case GE_OP:         predicate = LLVMIntSGE; break;
    case GREATER_OP:    predicate = LLVMIntSGT; break;
    case LE_OP:         predicate = LLVMIntSLE; break;
    case LESS_OP:       predicate = LLVMIntSLT; break;
    default:
        llvm_direct_reject("unsupported comparison operator");
        return NULL;
    }
    condition = LLVMBuildICmp(direct_ru.bld, predicate, left, right, "compare");
    return LLVMBuildZExt(direct_ru.bld, condition, i32t, "boolean");
}

/* A source-level function call. Glulx has dedicated opcodes for zero to
   three arguments; larger calls pass every argument on the VM stack, which
   the opaque-call scheme carries as explicit ".s" arguments in runtime pop
   order (first argument popped first). Calls have unknown effects, so the
   opaque declaration stays unmarked and acts as a full barrier. */
extern llvm_direct_value llvm_direct_call(llvm_direct_value function,
    llvm_direct_value *arguments, int count)
{
    static const char *const direct_call_names[4] =
        { "i6.callf", "i6.callfi", "i6.callfii", "i6.callfiii" };
    LLVMValueRef args[16];
    int i;
    if (!direct_can_emit() || !function) return NULL;
    if (count < 0 || count > 12) {
        llvm_direct_reject("unsupported call arity");
        return NULL;
    }
    for (i = 0; i < count; i++)
        if (!arguments[i]) return NULL;
    if (count <= 3) {
        args[0] = function;
        for (i = 0; i < count; i++)
            args[1 + i] = arguments[i];
        return direct_opaque_call(direct_call_names[count], args, count + 1);
    }
    args[0] = function;
    args[1] = LLVMConstInt(i32t, (uint32_t)count, FALSE);
    for (i = 0; i < count; i++)
        args[2 + i] = arguments[i];
    return direct_opaque_call("i6.call.s", args, count + 2);
}

/* A Glulx opcode with no native IR form, emitted as a typed opaque call
   whose centralized effect grading pins its ordering. Store-form opcodes
   return the stored value; void opcodes return a non-NULL sentinel so
   callers can distinguish success from rejection. */
extern llvm_direct_value llvm_direct_glulx_op(const char *opcode,
    llvm_direct_value *arguments, int count)
{
    char name[64];
    LLVMValueRef args[16], result;
    int32 opnum;
    int flags, n_src, i;
    if (!direct_can_emit()) return NULL;
    opnum = glulx_opcode_by_name(opcode);
    if (opnum < 0) {
        llvm_direct_reject("unknown direct opcode");
        return NULL;
    }
    flags = glulx_opcode_flags(opnum);
    if ((flags & (OPFLAG_STORE2 | OPFLAG_BRANCH))) {
        llvm_direct_reject("unsupported direct opcode shape");
        return NULL;
    }
    n_src = glulx_opcode_operand_count(opnum)
        - ((flags & OPFLAG_STORE) ? 1 : 0);
    if (count != n_src || count > 16) {
        llvm_direct_reject("direct opcode arity mismatch");
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (!arguments[i]) return NULL;
        args[i] = arguments[i];
    }
    sprintf(name, "i6.%.32s", opcode);
    result = direct_opaque_call_ex(name, args, count,
        !(flags & OPFLAG_STORE));
    /* Opcodes that never return (quit, restart...) end the block. */
    if (flags & OPFLAG_RETURNS)
        LLVMBuildUnreachable(direct_ru.bld);
    return (flags & OPFLAG_STORE) ? result : (llvm_direct_value)direct_ru.fn;
}

extern llvm_direct_value llvm_direct_store_local_value(int destination,
    llvm_direct_value value)
{
    if (!direct_can_emit() || !value) return NULL;
    if (destination < 1 || destination > direct_ru.local_count) {
        llvm_direct_reject("invalid local assignment");
        return NULL;
    }
    LLVMBuildStore(direct_ru.bld, value, direct_ru.locals[destination]);
    return value;
}

extern llvm_direct_value llvm_direct_store_global_value(int destination,
    llvm_direct_value value)
{
    if (!direct_can_emit() || !value) return NULL;
    if (destination < MAX_LOCAL_VARIABLES) {
        llvm_direct_reject("invalid global assignment");
        return NULL;
    }
    LLVMBuildStore(direct_ru.bld, value,
        direct_global_var(destination - MAX_LOCAL_VARIABLES));
    return value;
}

extern void llvm_direct_adjust_variable(int variable, int amount)
{
    LLVMValueRef pointer, value;
    if (!direct_can_emit()) return;
    if (variable >= 1 && variable <= direct_ru.local_count)
        pointer = direct_ru.locals[variable];
    else if (variable >= MAX_LOCAL_VARIABLES)
        pointer = direct_global_var(variable - MAX_LOCAL_VARIABLES);
    else {
        llvm_direct_reject("invalid adjusted variable");
        return;
    }
    value = LLVMBuildLoad2(direct_ru.bld, i32t, pointer, "adjust.value");
    value = LLVMBuildAdd(direct_ru.bld, value,
        LLVMConstInt(i32t, (uint32_t)amount, FALSE), "adjust");
    LLVMBuildStore(direct_ru.bld, value, pointer);
}

extern void llvm_direct_return_value(llvm_direct_value value)
{
    if (!direct_can_emit() || !value) return;
    LLVMBuildRet(direct_ru.bld, value);
}

extern void llvm_direct_note_statement(int statement_code)
{
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return;
    if (statement_code != RETURN_CODE && statement_code != RTRUE_CODE
        && statement_code != RFALSE_CODE && statement_code != JUMP_CODE
        && statement_code != IF_CODE && statement_code != BREAK_CODE
        && statement_code != CONTINUE_CODE && statement_code != DO_CODE
        && statement_code != FOR_CODE && statement_code != WHILE_CODE
        && statement_code != SWITCH_CODE && statement_code != PRINT_CODE
        && statement_code != PRINT_RET_CODE
        && statement_code != NEW_LINE_CODE && statement_code != GIVE_CODE
        && statement_code != MOVE_CODE && statement_code != REMOVE_CODE
        && statement_code != FONT_CODE && statement_code != STYLE_CODE
        && statement_code != STRING_CODE && statement_code != QUIT_CODE
        && statement_code != OBJECTLOOP_CODE
        && statement_code != SPACES_CODE
        && statement_code != INVERSION_CODE)
        llvm_direct_reject("unsupported statement");
}

extern void llvm_direct_store_local_constant(int destination, int32 value)
{
    if (!direct_can_emit()) return;
    if (destination < 1 || destination > direct_ru.local_count) {
        llvm_direct_reject("invalid local assignment");
        return;
    }
    LLVMBuildStore(direct_ru.bld, LLVMConstInt(i32t, (uint32_t)value, FALSE),
        direct_ru.locals[destination]);
}

extern void llvm_direct_store_local(int destination, int source)
{
    LLVMValueRef value;
    if (!direct_can_emit()) return;
    if (destination < 1 || destination > direct_ru.local_count
        || source < 1 || source > direct_ru.local_count) {
        llvm_direct_reject("invalid local assignment");
        return;
    }
    value = LLVMBuildLoad2(direct_ru.bld, i32t, direct_ru.locals[source], "value");
    LLVMBuildStore(direct_ru.bld, value, direct_ru.locals[destination]);
}

extern void llvm_direct_return_constant(int32 value)
{
    if (!direct_can_emit()) return;
    LLVMBuildRet(direct_ru.bld, LLVMConstInt(i32t, (uint32_t)value, FALSE));
}

extern void llvm_direct_return_local(int source)
{
    LLVMValueRef value;
    if (!direct_can_emit()) return;
    if (source < 1 || source > direct_ru.local_count) {
        llvm_direct_reject("invalid local return");
        return;
    }
    value = LLVMBuildLoad2(direct_ru.bld, i32t, direct_ru.locals[source], "value");
    LLVMBuildRet(direct_ru.bld, value);
}

static LLVMBasicBlockRef direct_label_block(int label)
{
    int i;
    if (label < 0) {
        llvm_direct_reject("invalid source label");
        return NULL;
    }
    if (label >= direct_ru.label_count) {
        int new_count = label + 1;
        LLVMBasicBlockRef *new_labels = realloc(direct_ru.labels,
            (size_t)new_count * sizeof(*new_labels));
        if (!new_labels)
            fatalerror("Out of memory creating direct LLVM labels");
        direct_ru.labels = new_labels;
        for (i = direct_ru.label_count; i < new_count; i++)
            direct_ru.labels[i] = NULL;
        direct_ru.label_count = new_count;
    }
    if (!direct_ru.labels[label])
        direct_ru.labels[label] = LLVMAppendBasicBlockInContext(
            llctx, direct_ru.fn, "label");
    return direct_ru.labels[label];
}

extern llvm_direct_block llvm_direct_new_block(void)
{
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return NULL;
    return LLVMAppendBasicBlockInContext(llctx, direct_ru.fn, "control");
}

extern llvm_direct_block llvm_direct_source_block(int label)
{
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return NULL;
    return direct_label_block(label);
}

extern void llvm_direct_bind_block(llvm_direct_block block)
{
    LLVMBasicBlockRef current;
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended || !block)
        return;
    current = LLVMGetInsertBlock(direct_ru.bld);
    if (current && !LLVMGetBasicBlockTerminator(current))
        LLVMBuildBr(direct_ru.bld, block);
    LLVMPositionBuilderAtEnd(direct_ru.bld, block);
}

extern void llvm_direct_jump_block(llvm_direct_block block)
{
    if (!direct_can_emit() || !block)
        return;
    LLVMBuildBr(direct_ru.bld, block);
}

extern void llvm_direct_branch(llvm_direct_value condition,
    llvm_direct_block true_block, llvm_direct_block false_block)
{
    LLVMValueRef test;
    if (!direct_can_emit() || !condition || !true_block || !false_block)
        return;
    test = LLVMBuildICmp(direct_ru.bld, LLVMIntNE, condition,
        LLVMConstInt(i32t, 0, FALSE), "condition");
    LLVMBuildCondBr(direct_ru.bld, test, true_block, false_block);
}

extern llvm_direct_block llvm_direct_current_block(void)
{
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return NULL;
    return LLVMGetInsertBlock(direct_ru.bld);
}

extern llvm_direct_value llvm_direct_phi(llvm_direct_value first,
    llvm_direct_block first_block, llvm_direct_value second,
    llvm_direct_block second_block)
{
    LLVMValueRef phi, values[2];
    LLVMBasicBlockRef blocks[2];
    if (!direct_can_emit() || !first || !first_block || !second
        || !second_block)
        return NULL;
    phi = LLVMBuildPhi(direct_ru.bld, i32t, "condition.value");
    values[0] = first; values[1] = second;
    blocks[0] = first_block; blocks[1] = second_block;
    LLVMAddIncoming(phi, values, blocks, 2);
    return phi;
}

/* An empty phi in the current block, for loops whose back-edge value
   does not exist yet; incoming edges are appended with
   llvm_direct_phi_add.  A phi left with missing edges fails the
   direct-verify stage, so soft failures stay safe. */
extern llvm_direct_value llvm_direct_phi_empty(void)
{
    if (!direct_can_emit()) return NULL;
    return LLVMBuildPhi(direct_ru.bld, i32t, "loop.value");
}

extern void llvm_direct_phi_add(llvm_direct_value phi,
    llvm_direct_value value, llvm_direct_block block)
{
    LLVMValueRef v = value;
    LLVMBasicBlockRef b = block;
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended
        || !phi || !value || !block)
        return;
    LLVMAddIncoming(phi, &v, &b, 1);
}

extern llvm_direct_value llvm_direct_phi_list(llvm_direct_value *values,
    llvm_direct_block *blocks, int count)
{
    LLVMValueRef phi;
    int i;
    if (!direct_can_emit() || count < 1)
        return NULL;
    for (i = 0; i < count; i++)
        if (!values[i] || !blocks[i]) return NULL;
    phi = LLVMBuildPhi(direct_ru.bld, i32t, "condition.value");
    LLVMAddIncoming(phi, (LLVMValueRef *)values,
        (LLVMBasicBlockRef *)blocks, (unsigned)count);
    return phi;
}

extern void llvm_direct_jump(int label)
{
    LLVMBasicBlockRef target;
    if (!direct_can_emit()) return;
    if (!direct_symstack_spill()) return;
    target = direct_label_block(label);
    if (target)
        LLVMBuildBr(direct_ru.bld, target);
}

extern void llvm_direct_bind_label(int label)
{
    LLVMBasicBlockRef current, target;
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return;
    if (!direct_symstack_spill()) return;
    target = direct_label_block(label);
    if (!target) return;
    current = LLVMGetInsertBlock(direct_ru.bld);
    if (current && !LLVMGetBasicBlockTerminator(current))
        LLVMBuildBr(direct_ru.bld, target);
    LLVMPositionBuilderAtEnd(direct_ru.bld, target);
}

extern void llvm_direct_resolve_label(int label, int used)
{
    LLVMBasicBlockRef target;
    LLVMBuilderRef builder;
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return;
    if (used) {
        llvm_direct_bind_label(label);
        return;
    }
    target = direct_label_block(label);
    if (!target || LLVMGetBasicBlockTerminator(target))
        return;
    builder = LLVMCreateBuilderInContext(llctx);
    LLVMPositionBuilderAtEnd(builder, target);
    LLVMBuildUnreachable(builder);
    LLVMDisposeBuilder(builder);
}

/* ------------------------------------------------------------------------- */
/*   Direct translation of parsed inline Glulx assembly                       */
/*                                                                           */
/*   Inline instructions arrive as assembly_instruction records exactly as   */
/*   the lifter sees captured ones, and translate under the same rules:      */
/*   native IR where semantics provably match, typed opaque i6.<opcode>      */
/*   calls otherwise, explicit control flow for branches and returns.        */
/*   Operands naming "sp" use a parse-time symbolic stack of SSA values;     */
/*   it must be empty wherever control flow diverges or joins, because a     */
/*   symbolic value has no runtime stack presence and classic's real stack   */
/*   could otherwise carry a value along only one path.                      */
/* ------------------------------------------------------------------------- */

static LLVMValueRef direct_asm_operand(const assembly_operand *op)
{
    switch (op->type) {
    case CONSTANT_OT:
    case HALFCONSTANT_OT:
    case BYTECONSTANT_OT:
        return llvm_direct_constant(op->value, op->marker, op->symindex);
    case ZEROCONSTANT_OT:
        return LLVMConstInt(i32t, 0, FALSE);
    case LOCALVAR_OT:
        if (op->value == 0)
            return direct_sympop();
        if (op->value < 1 || op->value > direct_ru.local_count) {
            llvm_direct_reject("inline local out of range");
            return NULL;
        }
        return LLVMBuildLoad2(direct_ru.bld, i32t, direct_ru.locals[op->value],
            "asm");
    case GLOBALVAR_OT:
        return LLVMBuildLoad2(direct_ru.bld, i32t,
            direct_global_var(op->value - MAX_LOCAL_VARIABLES), "asm");
    case DEREFERENCE_OT:
        {   LLVMValueRef addr = llvm_direct_constant(op->value, op->marker,
                op->symindex);
            if (!addr) return NULL;
            return direct_opaque_call_ex("i6.deref", &addr, 1, FALSE);
        }
    default:
        llvm_direct_reject("unsupported inline operand");
        return NULL;
    }
}

static void direct_asm_store(const assembly_operand *op, LLVMValueRef v)
{
    if (!v) return;
    switch (op->type) {
    case ZEROCONSTANT_OT:
        return;                       /* discard */
    case LOCALVAR_OT:
        if (op->value == 0) {
            direct_sympush(v);
            return;
        }
        if (op->value < 1 || op->value > direct_ru.local_count) {
            llvm_direct_reject("inline local out of range");
            return;
        }
        LLVMBuildStore(direct_ru.bld, v, direct_ru.locals[op->value]);
        return;
    case GLOBALVAR_OT:
        LLVMBuildStore(direct_ru.bld, v,
            direct_global_var(op->value - MAX_LOCAL_VARIABLES));
        return;
    case DEREFERENCE_OT:
        {   LLVMValueRef args[2];
            args[0] = llvm_direct_constant(op->value, op->marker,
                op->symindex);
            args[1] = v;
            if (!args[0]) return;
            (void)direct_opaque_call_ex("i6.deref.store", args, 2, TRUE);
        }
        return;
    default:
        llvm_direct_reject("unsupported inline store operand");
        return;
    }
}

/* Opaque form: sources become arguments, a store operand receives the
   result, stack-passed extras follow the sources. */
static void direct_asm_opaque(const assembly_instruction *ai, int flags,
    LLVMValueRef *extra, int n_extra)
{
    char name[64];
    LLVMValueRef args[16], result;
    int n_src = ai->operand_count - ((flags & OPFLAG_STORE) ? 1 : 0);
    int i, n_args = 0;

    if (flags & OPFLAG_STORE2) {
        llvm_direct_reject("inline opcode with two stores");
        return;
    }
    if (n_src + n_extra > 16) {
        llvm_direct_reject("inline opcode with too many operands");
        return;
    }
    for (i = 0; i < n_src; i++) {
        args[n_args] = direct_asm_operand(&ai->operand[i]);
        if (!args[n_args++]) return;
    }
    for (i = 0; i < n_extra; i++)
        args[n_args++] = extra[i];
    sprintf(name, "i6.%.32s%s", glulx_opcode_name(ai->internal_number),
        n_extra ? ".s" : "");
    result = direct_opaque_call_ex(name, args, n_args,
        !(flags & OPFLAG_STORE));
    if (flags & OPFLAG_STORE)
        direct_asm_store(&ai->operand[ai->operand_count - 1], result);
    if (flags & OPFLAG_RETURNS)
        LLVMBuildUnreachable(direct_ru.bld);
}

static void direct_asm_binop(const assembly_instruction *ai, LLVMOpcode opc)
{
    LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
    LLVMValueRef b = direct_asm_operand(&ai->operand[1]);
    if (!a || !b) return;
    direct_asm_store(&ai->operand[2], LLVMBuildBinOp(direct_ru.bld, opc, a, b,
        "asm"));
}

static void direct_asm_condbranch(const assembly_instruction *ai,
    LLVMIntPredicate pred, int with_zero)
{
    LLVMValueRef a, b, cond;
    LLVMBasicBlockRef then_bb, else_bb;
    int32 target = ai->operand[ai->operand_count - 1].value;

    a = direct_asm_operand(&ai->operand[0]);
    b = with_zero ? LLVMConstInt(i32t, 0, FALSE)
        : direct_asm_operand(&ai->operand[1]);
    if (!a || !b) return;
    if (!direct_symstack_spill()) return;
    then_bb = direct_label_block((int)target);
    if (!then_bb) return;
    cond = LLVMBuildICmp(direct_ru.bld, pred, a, b, "asm.cond");
    else_bb = LLVMAppendBasicBlockInContext(llctx, direct_ru.fn, "asm.next");
    LLVMBuildCondBr(direct_ru.bld, cond, then_bb, else_bb);
    LLVMPositionBuilderAtEnd(direct_ru.bld, else_bb);
}

/* If the operand is an unmarked integer constant, put its value in *v. */
static int direct_asm_constant(const assembly_operand *op, int32 *v)
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

extern void llvm_direct_glulx_assembly(const assembly_instruction *ai)
{
    int flags, opct;

    if (!direct_can_emit()) return;
    if (ai->internal_number == -1) {
        llvm_direct_reject("custom opcode");
        return;
    }
    flags = glulx_opcode_flags(ai->internal_number);
    opct = glulx_opcode_operand_count(ai->internal_number);
    if (ai->operand_count != opct) {
        llvm_direct_reject("inline operand count mismatch");
        return;
    }

    switch (ai->internal_number) {

    case nop_gc:
        return;

    case add_gc:    direct_asm_binop(ai, LLVMAdd); return;
    case sub_gc:    direct_asm_binop(ai, LLVMSub); return;
    case mul_gc:    direct_asm_binop(ai, LLVMMul); return;
    case bitand_gc: direct_asm_binop(ai, LLVMAnd); return;
    case bitor_gc:  direct_asm_binop(ai, LLVMOr);  return;
    case bitxor_gc: direct_asm_binop(ai, LLVMXor); return;

    /* Division: only visibly safe constant divisors are native (zero
       faults; INT_MIN / -1 overflows; both are LLVM UB). */
    case div_gc:
    case mod_gc:
        {   int32 dv;
            if (direct_asm_constant(&ai->operand[1], &dv)
                && dv != 0 && dv != -1)
                direct_asm_binop(ai, ai->internal_number == div_gc
                    ? LLVMSDiv : LLVMSRem);
            else
                direct_asm_opaque(ai, flags, NULL, 0);
        }
        return;

    /* Shifts: Glulx defines counts >= 32; LLVM gives poison. Constant
       counts fold to their defined results, variables stay opaque. */
    case shiftl_gc:
    case sshiftr_gc:
    case ushiftr_gc:
        {   int32 cnt;
            if (!direct_asm_constant(&ai->operand[1], &cnt)) {
                direct_asm_opaque(ai, flags, NULL, 0);
                return;
            }
            if ((uint32_t)cnt < 32) {
                direct_asm_binop(ai,
                    ai->internal_number == shiftl_gc ? LLVMShl
                    : ai->internal_number == sshiftr_gc ? LLVMAShr
                    : LLVMLShr);
                return;
            }
            {   LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
                LLVMValueRef r;
                if (!a) return;
                if (ai->internal_number == sshiftr_gc)
                    r = LLVMBuildAShr(direct_ru.bld, a,
                        LLVMConstInt(i32t, 31, FALSE), "asm");
                else
                    r = LLVMConstInt(i32t, 0, FALSE);
                direct_asm_store(&ai->operand[2], r);
            }
        }
        return;

    case neg_gc:
        {   LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
            if (!a) return;
            direct_asm_store(&ai->operand[1],
                LLVMBuildNeg(direct_ru.bld, a, "asm"));
        }
        return;
    case bitnot_gc:
        {   LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
            if (!a) return;
            direct_asm_store(&ai->operand[1],
                LLVMBuildNot(direct_ru.bld, a, "asm"));
        }
        return;
    case copy_gc:
        {   LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
            if (!a) return;
            direct_asm_store(&ai->operand[1], a);
        }
        return;
    case copys_gc:
    case copyb_gc:
        /* Sub-word copies read and write their operands at narrow
           widths; the word-based operand model would misread them. */
        llvm_direct_reject("byte/short copy opcode");
        return;
    case sexs_gc:
    case sexb_gc:
        {   LLVMTypeRef small = (ai->internal_number == sexs_gc)
                ? LLVMInt16TypeInContext(llctx)
                : LLVMInt8TypeInContext(llctx);
            LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
            if (!a) return;
            a = LLVMBuildTrunc(direct_ru.bld, a, small, "asm");
            a = LLVMBuildSExt(direct_ru.bld, a, i32t, "asm");
            direct_asm_store(&ai->operand[1], a);
        }
        return;

    case jump_gc:
        {   LLVMBasicBlockRef target;
            if (!direct_symstack_spill()) return;
            target = direct_label_block((int)ai->operand[0].value);
            if (!target) return;
            LLVMBuildBr(direct_ru.bld, target);
        }
        return;
    case jz_gc:   direct_asm_condbranch(ai, LLVMIntEQ,  TRUE);  return;
    case jnz_gc:  direct_asm_condbranch(ai, LLVMIntNE,  TRUE);  return;
    case jeq_gc:  direct_asm_condbranch(ai, LLVMIntEQ,  FALSE); return;
    case jne_gc:  direct_asm_condbranch(ai, LLVMIntNE,  FALSE); return;
    case jlt_gc:  direct_asm_condbranch(ai, LLVMIntSLT, FALSE); return;
    case jle_gc:  direct_asm_condbranch(ai, LLVMIntSLE, FALSE); return;
    case jgt_gc:  direct_asm_condbranch(ai, LLVMIntSGT, FALSE); return;
    case jge_gc:  direct_asm_condbranch(ai, LLVMIntSGE, FALSE); return;
    case jltu_gc: direct_asm_condbranch(ai, LLVMIntULT, FALSE); return;
    case jleu_gc: direct_asm_condbranch(ai, LLVMIntULE, FALSE); return;
    case jgtu_gc: direct_asm_condbranch(ai, LLVMIntUGT, FALSE); return;
    case jgeu_gc: direct_asm_condbranch(ai, LLVMIntUGE, FALSE); return;

    case return_gc:
        {   LLVMValueRef a = direct_asm_operand(&ai->operand[0]);
            if (!a) return;
            /* Anything on the VM stack dies with the call frame. */
            direct_ru.symstack_depth = 0;
            direct_ru.spilled = 0;
            LLVMBuildRet(direct_ru.bld, a);
        }
        return;

    case call_gc:
    case glk_gc:
    case tailcall_gc:
        {   /* (addr, count[, store]): count operands are popped from
               the stack at runtime; peel them off the symbolic stack
               and pass them explicitly. */
            LLVMValueRef extra[DIRECT_SYMSTACK_MAX];
            const assembly_operand *cntop = &ai->operand[1];
            int32 cnt;
            int i;
            if (!direct_asm_constant(cntop, &cnt)) {
                llvm_direct_reject("call with non-constant argument count");
                return;
            }
            if (cnt < 0 || cnt > direct_ru.symstack_depth) {
                llvm_direct_reject("call consumes more than the symbolic stack");
                return;
            }
            /* Runtime pop order: the first argument was pushed last. */
            for (i = 0; i < cnt; i++)
                extra[i] = direct_sympop();
            direct_asm_opaque(ai, flags, extra, cnt);
        }
        return;

    case stkcount_gc:
    case stkpeek_gc:
    case stkswap_gc:
    case stkroll_gc:
    case stkcopy_gc:
        llvm_direct_reject("explicit stack-manipulation opcode");
        return;

    case catch_gc:
    case throw_gc:
        llvm_direct_reject("catch/throw");
        return;

    default:
        if (flags & OPFLAG_BRANCH) {
            llvm_direct_reject("unhandled inline branch opcode");
            return;
        }
        direct_asm_opaque(ai, flags, NULL, 0);
        return;
    }
}

/* Statement-level control flow (conditions, switch selectors) must not
   straddle pending symbolic stack values; see the empty-stack rule. */
extern void llvm_direct_note_control_flow(void)
{
    if (direct_ru.state != DIRECT_BUILDING || direct_ru.suspended)
        return;
    (void)direct_symstack_spill();
}

/* Inline assembly macros: @pull and @push move values between the
   symbolic stack and an operand; @dload/@dstore expand to word pairs. */
extern void llvm_direct_glulx_macro(const assembly_instruction *ai,
    int macro_code)
{
    if (!direct_can_emit()) return;
    switch (macro_code) {
    case pull_gm:
        {   LLVMValueRef v = direct_sympop();
            if (v) direct_asm_store(&ai->operand[0], v);
        }
        return;
    case push_gm:
        {   LLVMValueRef v = direct_asm_operand(&ai->operand[0]);
            if (v) direct_sympush(v);
        }
        return;
    case dload_gm:
    case dstore_gm:
        {   LLVMValueRef addr = direct_asm_operand(&ai->operand[0]);
            LLVMValueRef args[3];
            int is_load = (macro_code == dload_gm);
            if (!addr) return;
            if (is_load) {
                args[0] = addr;
                args[1] = LLVMConstInt(i32t, 1, FALSE);
                direct_asm_store(&ai->operand[1],
                    direct_opaque_call_ex("i6.aload", args, 2, FALSE));
                args[1] = LLVMConstInt(i32t, 0, FALSE);
                direct_asm_store(&ai->operand[2],
                    direct_opaque_call_ex("i6.aload", args, 2, FALSE));
            }
            else {
                LLVMValueRef hi = direct_asm_operand(&ai->operand[1]);
                LLVMValueRef lo = direct_asm_operand(&ai->operand[2]);
                if (!hi || !lo) return;
                args[0] = addr;
                args[1] = LLVMConstInt(i32t, 0, FALSE);
                args[2] = hi;
                (void)direct_opaque_call_ex("i6.astore", args, 3, TRUE);
                args[1] = LLVMConstInt(i32t, 1, FALSE);
                args[2] = lo;
                (void)direct_opaque_call_ex("i6.astore", args, 3, TRUE);
            }
        }
        return;
    default:
        llvm_direct_reject("unsupported assembly macro");
        return;
    }
}

/* Dump the routine's IR when machine-readable diagnostics are enabled. */
static void dump_fn(LLVMValueRef fn, const char *phase)
{
    char *ir;
    if (!llvm_diagnostics_enabled())
        return;
    if (!dump_file)
        dump_file = fopen("inform6-llvm-dump.ll", "w");
    if (!dump_file)
        return;
    ir = LLVMPrintValueToString(fn);
    fprintf(dump_file, "; ===== %s (%s)\n%s\n",
        llvm_current_routine_name(), phase, ir);
    LLVMDisposeMessage(ir);
}

/* Stable per-routine record for migration coverage tests. Keep the free-form
   failure detail in the existing human diagnostics above this TSV layer. */
static void report_backend(const char *backend, const char *stage,
    int insts_in, int insts_out)
{
    if (!llvm_diagnostics_enabled())
        return;
    printf("LLVM-BACKEND\tname=%s\tbackend=%s\tstage=%s"
           "\tinput=%d\temitted=%d\treason=-\n",
        llvm_current_routine_name(), backend, stage, insts_in, insts_out);
}

static void direct_fallback(const char *stage, const char *detail)
{
    const char *p = detail ? detail : "unknown";
    no_routines_direct_fallback++;
    if (strcmp(stage, "direct-lower") == 0)
        no_routines_direct_lower_failed++;
    else
        no_routines_direct_build_failed++;
    if (llvm_diagnostics_enabled()) {
        printf("LLVM-BACKEND\tname=%s\tbackend=classic-fallback\tstage=%s"
               "\tinput=-1\temitted=-1\treason=",
            llvm_current_routine_name(), stage);
        for (; *p; p++)
            putchar((*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p);
        putchar('\n');
        printf("! LLVM direct: bailed on %s: %s\n",
            llvm_current_routine_name(), detail ? detail : "unknown");
    }
    direct_dispose_ir();
    direct_ru.state = DIRECT_INACTIVE;
    direct_ru.reason = NULL;
}

/* The per-routine pipeline: verify the function, run LLVM's function
   passes over it, legalize the result into shapes the lowerer accepts,
   and lower it into the event buffer. Returns FALSE with a stage/reason
   on any failure, leaving fallback logging to the caller. */
static int process_direct_routine(void)
{
    const char *stage = NULL, *why = NULL;
    int insts_in = 0, insts_out = 0;
    int ok = TRUE;

    if (LLVMVerifyFunction(direct_ru.fn, LLVMReturnStatusAction)) {
        stage = "direct-verify";
        why = "verification failed";
        ok = FALSE;
    }
    if (ok) {
        dump_fn(direct_ru.fn, "direct-pre-opt");
        {   LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
            LLVMErrorRef err = LLVMRunPassesOnFunction(direct_ru.fn,
                LLVM_PASS_PIPELINE, NULL, opts);
            LLVMDisposePassBuilderOptions(opts);
            if (err) {
                stage = "direct-optimize";
                why = "optimization pass failure";
                LLVMConsumeError(err);
                ok = FALSE;
            }
        }
    }
    if (ok) {
        unmerge_pointer_selects(direct_ru.fn);
        if (LLVMVerifyFunction(direct_ru.fn, LLVMReturnStatusAction)) {
            stage = "direct-post-verify";
            why = "post-optimization verification failed";
            ok = FALSE;
        }
    }
    if (ok) {
        dump_fn(direct_ru.fn, "direct-post-opt");
        if (!llvm_lower_routine(llmod, direct_ru.fn, &why,
                &insts_in, &insts_out)) {
            stage = "direct-lower";
            if (!why) why = "lowering failed";
            ok = FALSE;
        }
    }
    if (!ok) {
        direct_fallback(stage, why);
        return FALSE;
    }
    report_backend("direct", "lower", insts_in, insts_out);
    no_routines_direct++;
    if (llvm_diagnostics_enabled())
        printf("LLVM: direct routine %s: %d instructions -> %d\n",
            llvm_current_routine_name(), insts_in, insts_out);
    direct_dispose_ir();
    direct_ru.state = DIRECT_INACTIVE;
    direct_ru.reason = NULL;
    return TRUE;
}

/* Verify, optimize, and lower the directly generated routine. Returns
   TRUE when lowering succeeded and the event buffer holds the lowered
   stream; FALSE when asm.c must replay the front-end shadow stream
   instead. Either way the buffer is replayed through the classic
   encoder by asm.c. */
extern int llvm_pipeline_routine(void)
{
    if (direct_ru.state == DIRECT_READY)
        return process_direct_routine();
    if (direct_ru.state == DIRECT_REJECTED)
        direct_fallback("direct-build", direct_ru.reason);
    else if (direct_ru.state == DIRECT_BUILDING)
        direct_fallback("direct-finish", "builder was not finalized");
    return FALSE;
}

/* --- Deferred lowering ------------------------------------------------- */
/* Under deferred lowering, a finished routine is not lowered at parse
   time. Its function stays in the shared module, tracked in a retained
   array keyed by handle, and the build state resets so the next routine
   builds cleanly. At end of pass asm.c restores each handle and runs the
   normal pipeline. Rejected/absent IR retains nothing (-1); the routine
   then replays its shadow stream. */
static routine_unit *retained_units;
static int retained_unit_count, retained_unit_cap;

extern int llvm_retain_direct_routine(int routine_symbol)
{
    if (direct_ru.state != DIRECT_READY) {
        /* No lowerable IR: log the build fallback now so end-of-pass
           totals are right. The routine's captured shadow stream is
           replayed at end of pass. A classic (INACTIVE) routine had no
           direct attempt and was already noted, so it logs nothing. */
        if (direct_ru.state == DIRECT_REJECTED)
            direct_fallback("direct-build", direct_ru.reason);
        else if (direct_ru.state == DIRECT_BUILDING)
            direct_fallback("direct-finish", "builder was not finalized");
        return -1;
    }
    if (retained_unit_count >= retained_unit_cap) {
        int nc = retained_unit_cap ? retained_unit_cap * 2 : 128;
        routine_unit *n = realloc(retained_units, (size_t)nc * sizeof *n);
        if (!n)
            fatalerror("Out of memory retaining direct routines");
        retained_units = n;
        retained_unit_cap = nc;
    }
    /* The per-build scratch (allocas/blocks) is dead once the function is
       finalized; only the function is needed to lower it later. */
    free(direct_ru.locals); direct_ru.locals = NULL; direct_ru.local_count = 0;
    free(direct_ru.labels); direct_ru.labels = NULL; direct_ru.label_count = 0;
    direct_ru.routine_symbol = routine_symbol;
    retained_units[retained_unit_count] = direct_ru;  /* move ownership */
    memset(&direct_ru, 0, sizeof direct_ru);
    direct_ru.state = DIRECT_INACTIVE;
    return retained_unit_count++;
}

extern int llvm_lower_retained_routine(int handle)
{
    if (handle < 0 || handle >= retained_unit_count)
        return FALSE;
    /* Run the pipeline on the retained function. Success or failure, the
       function is deleted from the shared module by the pipeline's
       disposal, so clear the handle. */
    direct_ru = retained_units[handle];
    {   int ok = llvm_pipeline_routine();
        memset(&direct_ru, 0, sizeof direct_ru);
        direct_ru.state = DIRECT_INACTIVE;
        retained_units[handle].fn = NULL;
        return ok;
    }
}

extern void llvm_codegen_free(void)
{
    direct_dispose_ir();
    direct_ru.state = DIRECT_INACTIVE;
    free(retained_units);
    retained_units = NULL;
    retained_unit_count = retained_unit_cap = 0;
    if (no_routines_direct || no_routines_classic
        || no_routines_direct_fallback) {
        printf("LLVM: backends direct=%d classic=%d fallback=%d\n",
            no_routines_direct, no_routines_classic,
            no_routines_direct_fallback);
    }
    if (no_routines_direct_build_failed || no_routines_direct_lower_failed) {
        printf("LLVM: direct fallbacks build=%d lower=%d\n",
            no_routines_direct_build_failed,
            no_routines_direct_lower_failed);
    }
    no_routines_direct = 0;
    no_routines_classic = 0;
    no_routines_direct_fallback = 0;
    no_routines_direct_build_failed = 0;
    no_routines_direct_lower_failed = 0;
    llvm_lower_insts_in = 0;
    llvm_lower_insts_out = 0;
    if (dump_file) {
        fclose(dump_file);
        dump_file = NULL;
    }
    if (llctx) {
        if (llmod) {
            LLVMDisposeModule(llmod);
            llmod = NULL;
        }
        LLVMContextDispose(llctx);
        llctx = NULL;
    }
}
