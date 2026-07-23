/* ------------------------------------------------------------------------- */
/*   "states" :  Statement translator                                        */
/*                                                                           */
/*   Part of Inform 6.45                                                     */
/*   copyright (c) Graham Nelson 1993 - 2025                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

#include "header.h"

static void direct_operand_call(assembly_operand fn,
    llvm_direct_value *arguments, int count);

static int match_colon(void)
{   get_next_token();
    if (token_type == SEP_TT)
    {   if (token_value == SEMICOLON_SEP)
            warning("Unlike C, Inform uses ':' to divide parts \
of a 'for' loop specification: replacing ';' with ':'");
        else
        if (token_value != COLON_SEP)
        {   ebf_curtoken_error("':'");
            panic_mode_error_recovery();
            return(FALSE);
        }
    }
    else
    {   ebf_curtoken_error("':'");
        panic_mode_error_recovery();
        return(FALSE);
    }
    return(TRUE);
}

static void match_open_bracket(void)
{   get_next_token();
    if ((token_type == SEP_TT) && (token_value == OPENB_SEP)) return;
    put_token_back();
    ebf_curtoken_error("'('");
}

extern void match_close_bracket(void)
{   get_next_token();
    if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP)) return;
    put_token_back();
    ebf_curtoken_error("')'");
}

static void parse_action(void)
{   int level = 1, args = 0, codegen_action;
    assembly_operand AO, AO2, AO3, AO4, AO5;

    /* An action statement has the form <ACTION NOUN SECOND, ACTOR>
       or <<ACTION NOUN SECOND, ACTOR>>. It simply compiles into a call
       to R_Process() with those four arguments. (The latter form,
       with double brackets, means "return true afterwards".)

       The R_Process() function should be supplied by the library, 
       although a stub is defined in the veneer.

       The NOUN, SECOND, and ACTOR arguments are optional. If not
       supplied, R_Process() will be called with fewer arguments. 
       (But if you supply ACTOR, it must be preceded by a comma.
       <ACTION, ACTOR> is equivalent to <ACTION 0 0, ACTOR>.)

       To complicate life, the ACTION argument may be a bare action
       name or a parenthesized expression. (So <Take> is equivalent
       to <(##Take)>.) We have to peek at the first token, checking
       whether it's an open-paren, to distinguish these cases.

       You may ask why the ACTOR argument is last; the "natural"
       Inform ordering would be "<floyd, take ball>". True! Sadly,
       Inform's lexer isn't smart enough to parse this consistently,
       so we can't do it.
    */

    dont_enter_into_symbol_table = TRUE;
    get_next_token();
    if ((token_type == SEP_TT) && (token_value == LESS_SEP))
    {   level = 2; get_next_token();
    }
    dont_enter_into_symbol_table = FALSE;

    /* Peek at the next token; see if it's an open-paren. */
    if ((token_type==SEP_TT) && (token_value==OPENB_SEP))
    {   put_token_back();
        AO2 = parse_expression(ACTION_Q_CONTEXT);
        codegen_action = TRUE;
    }
    else
    {
        if (token_type != UQ_TT) {
            ebf_curtoken_error("name of action");
        }
        codegen_action = FALSE;
        AO2 = action_of_name(token_text);
    }

    get_next_token();
    AO3 = zero_operand;
    AO4 = zero_operand;
    AO5 = zero_operand;
    if (!((token_type == SEP_TT) && (token_value == GREATER_SEP || token_value == COMMA_SEP)))
    {   put_token_back();
        args = 1;
        AO3 = parse_expression(ACTION_Q_CONTEXT);

        get_next_token();
    }
    if (!((token_type == SEP_TT) && (token_value == GREATER_SEP || token_value == COMMA_SEP)))
    {   put_token_back();
        args = 2;
        AO4 = parse_expression(QUANTITY_CONTEXT);
        get_next_token();
    }
    if (!((token_type == SEP_TT) && (token_value == GREATER_SEP || token_value == COMMA_SEP)))
    {
        ebf_curtoken_error("',' or '>'");
    }

    if ((token_type == SEP_TT) && (token_value == COMMA_SEP))
    {
        if (!glulx_mode && (version_number < 4))
        {
            error("<x, y> syntax is not available in Z-code V3 or earlier");
        }
        args = 3;
        AO5 = parse_expression(QUANTITY_CONTEXT);
        get_next_token();
        if (!((token_type == SEP_TT) && (token_value == GREATER_SEP)))
        {
            ebf_curtoken_error("'>'");
        }
    }

    if (level == 2)
    {   get_next_token();
        if (!((token_type == SEP_TT) && (token_value == GREATER_SEP)))
        {   put_token_back();
            ebf_curtoken_error("'>>'");
        }
    }

    if (!glulx_mode) {

        AO = veneer_routine(R_Process_VR);

        switch(args)
        {   case 0:
                if (codegen_action) AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
                if (version_number>=5)
                    assemblez_2(call_2n_zc, AO, AO2);
                else
                    if (version_number==4)
                        assemblez_2_to(call_vs_zc, AO, AO2, temp_var1);
                    else
                        assemblez_2_to(call_zc, AO, AO2, temp_var1);
                break;
        case 1:
            AO3 = code_generate(AO3, QUANTITY_CONTEXT, -1);
            if (codegen_action) AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            if (version_number>=5)
                assemblez_3(call_vn_zc, AO, AO2, AO3);
            else
                if (version_number==4)
                    assemblez_3_to(call_vs_zc, AO, AO2, AO3, temp_var1);
                else
                    assemblez_3_to(call_zc, AO, AO2, AO3, temp_var1);
            break;
        case 2:
            AO4 = code_generate(AO4, QUANTITY_CONTEXT, -1);
            AO3 = code_generate(AO3, QUANTITY_CONTEXT, -1);
            if (codegen_action) AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            if (version_number>=5)
                assemblez_4(call_vn_zc, AO, AO2, AO3, AO4);
            else
                if (version_number==4)
                    assemblez_4_to(call_vs_zc, AO, AO2, AO3, AO4, temp_var1);
                else
                    assemblez_4_to(call_zc, AO, AO2, AO3, AO4, temp_var1);
            break;
        case 3:
            AO5 = code_generate(AO5, QUANTITY_CONTEXT, -1);
            AO4 = code_generate(AO4, QUANTITY_CONTEXT, -1);
            AO3 = code_generate(AO3, QUANTITY_CONTEXT, -1);
            if (codegen_action) AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            if (version_number>=5)
                assemblez_5(call_vn2_zc, AO, AO2, AO3, AO4, AO5);
            else
                if (version_number==4)
                    assemblez_5_to(call_vs2_zc, AO, AO2, AO3, AO4, AO5, temp_var1);
            /* if V3 or earlier, we've already displayed an error */
            break;
            break;
        }

        if (level == 2) assemblez_0(rtrue_zc);

    }
    else {
        llvm_direct_value direct_args[4];

        AO = veneer_routine(R_Process_VR);

        switch (args) {

        case 0:
            direct_args[0] = llvm_direct_quantity(AO2);
            if (codegen_action)
                AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            assembleg_call_1(AO, AO2, zero_operand);
            break;

        case 1:
            direct_args[1] = llvm_direct_quantity(AO3);
            AO3 = code_generate(AO3, QUANTITY_CONTEXT, -1);
            direct_args[0] = llvm_direct_quantity(AO2);
            if (codegen_action)
                AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            assembleg_call_2(AO, AO2, AO3, zero_operand);
            break;

        case 2:
            direct_args[2] = llvm_direct_quantity(AO4);
            AO4 = code_generate(AO4, QUANTITY_CONTEXT, -1);
            direct_args[1] = llvm_direct_quantity(AO3);
            AO3 = code_generate(AO3, QUANTITY_CONTEXT, -1);
            direct_args[0] = llvm_direct_quantity(AO2);
            if (codegen_action) 
                AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            assembleg_call_3(AO, AO2, AO3, AO4, zero_operand);
            break;

        case 3:
            direct_args[3] = llvm_direct_quantity(AO5);
            AO5 = code_generate(AO5, QUANTITY_CONTEXT, -1);
            if (!((AO5.type == LOCALVAR_OT) && (AO5.value == 0)))
                assembleg_store(stack_pointer, AO5);
            direct_args[2] = llvm_direct_quantity(AO4);
            AO4 = code_generate(AO4, QUANTITY_CONTEXT, -1);
            if (!((AO4.type == LOCALVAR_OT) && (AO4.value == 0)))
                assembleg_store(stack_pointer, AO4);
            direct_args[1] = llvm_direct_quantity(AO3);
            AO3 = code_generate(AO3, QUANTITY_CONTEXT, -1);
            if (!((AO3.type == LOCALVAR_OT) && (AO3.value == 0)))
                assembleg_store(stack_pointer, AO3);
            direct_args[0] = llvm_direct_quantity(AO2);
            if (codegen_action) 
                AO2 = code_generate(AO2, QUANTITY_CONTEXT, -1);
            if (!((AO2.type == LOCALVAR_OT) && (AO2.value == 0)))
                assembleg_store(stack_pointer, AO2);
            assembleg_3(call_gc, AO, four_operand, zero_operand);
            break;
        }

        direct_operand_call(AO, direct_args, args + 1);

        if (level == 2) {
            llvm_direct_return_constant(1);
            assembleg_1(return_gc, one_operand);
        }

    }
}

extern int parse_label(void)
{
    get_next_token();

    if ((token_type == SYMBOL_TT) &&
        (symbols[token_value].type == LABEL_T))
    {   symbols[token_value].flags |= USED_SFLAG;
        return(symbols[token_value].value);
    }

    if ((token_type == SYMBOL_TT) && (symbols[token_value].flags & UNKNOWN_SFLAG))
    {   int label = alloc_label();
        assign_symbol(token_value, label, LABEL_T);
        define_symbol_label(token_value);
        symbols[token_value].flags |= CHANGE_SFLAG + USED_SFLAG;
        return(symbols[token_value].value);
    }

    ebf_curtoken_error("label name");
    return 0;
}

static void parse_print_z(int finally_return)
{   int count = 0; assembly_operand AO;

    /*  print <printlist> -------------------------------------------------- */
    /*  print_ret <printlist> ---------------------------------------------- */
    /*  <literal-string> --------------------------------------------------- */
    /*                                                                       */
    /*  <printlist> is a comma-separated list of items:                      */
    /*                                                                       */
    /*       <literal-string>                                                */
    /*       <other-expression>                                              */
    /*       (char) <expression>                                             */
    /*       (address) <expression>                                          */
    /*       (string) <expression>                                           */
    /*       (a) <expression>                                                */
    /*       (the) <expression>                                              */
    /*       (The) <expression>                                              */
    /*       (name) <expression>                                             */
    /*       (number) <expression>                                           */
    /*       (property) <expression>                                         */
    /*       (<routine>) <expression>                                        */
    /*       (object) <expression>     (for use in low-level code only)      */
    /* --------------------------------------------------------------------- */

    do
    {   AI.text = token_text;
        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) break;
        switch(token_type)
        {   case DQ_TT:
              if (token_text[0] == '^' && token_text[1] == '\0') {
                  /* The string "^" is always a simple newline. */
                  assemblez_0(new_line_zc);
                  break;
              }
              if ((int)strlen(token_text) > ZCODE_MAX_INLINE_STRING)
              {   INITAOT(&AO, LONG_CONSTANT_OT);
                  AO.marker = STRING_MV;
                  AO.value  = compile_string(token_text, STRCTX_GAME);
                  assemblez_1(print_paddr_zc, AO);
                  if (finally_return)
                  {   get_next_token();
                      if ((token_type == SEP_TT)
                          && (token_value == SEMICOLON_SEP))
                      {   assemblez_0(new_line_zc);
                          assemblez_0(rtrue_zc);
                          return;
                      }
                      put_token_back();
                  }
                  break;
              }
              if (finally_return)
              {   get_next_token();
                  if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
                  {   assemblez_0(print_ret_zc); return;
                  }
                  put_token_back();
              }
              assemblez_0(print_zc);
              break;

            case SEP_TT:
              if (token_value == OPENB_SEP)
              {   misc_keywords.enabled = TRUE;
                  get_next_token();
                  get_next_token();
                  if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP))
                  {   assembly_operand AO1;

                      put_token_back(); put_token_back();
                      local_variables.enabled = FALSE;
                      get_next_token();
                      misc_keywords.enabled = FALSE;
                      local_variables.enabled = TRUE;

                      if ((token_type == STATEMENT_TT)
                          &&(token_value == STRING_CODE))
                      {   token_type = MISC_KEYWORD_TT;
                          token_value = STRING_MK;
                      }

                      switch(token_type)
                      {
                        case MISC_KEYWORD_TT:
                          switch(token_value)
                          {   case CHAR_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintC_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  AO1 = code_generate(
                                      parse_expression(QUANTITY_CONTEXT),
                                      QUANTITY_CONTEXT, -1);
                                  assemblez_1(print_char_zc, AO1);
                                  goto PrintTermDone;
                              case ADDRESS_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintA_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  AO1 = code_generate(
                                      parse_expression(QUANTITY_CONTEXT),
                                      QUANTITY_CONTEXT, -1);
                                  assemblez_1(print_addr_zc, AO1);
                                  goto PrintTermDone;
                              case STRING_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintS_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  AO1 = code_generate(
                                      parse_expression(QUANTITY_CONTEXT),
                                      QUANTITY_CONTEXT, -1);
                                  assemblez_1(print_paddr_zc, AO1);
                                  goto PrintTermDone;
                              case OBJECT_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintO_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  AO1 = code_generate(
                                      parse_expression(QUANTITY_CONTEXT),
                                      QUANTITY_CONTEXT, -1);
                                  assemblez_1(print_obj_zc, AO1);
                                  goto PrintTermDone;
                              case THE_MK:
                                  AO = veneer_routine(DefArt_VR);
                                  goto PrintByRoutine;
                              case AN_MK:
                              case A_MK:
                                  AO = veneer_routine(InDefArt_VR);
                                  goto PrintByRoutine;
                              case CAP_THE_MK:
                                  AO = veneer_routine(CDefArt_VR);
                                  goto PrintByRoutine;
                              case CAP_A_MK:
                                  AO = veneer_routine(CInDefArt_VR);
                                  goto PrintByRoutine;
                              case NAME_MK:
                                  AO = veneer_routine(PrintShortName_VR);
                                  goto PrintByRoutine;
                              case NUMBER_MK:
                                  AO = veneer_routine(EnglishNumber_VR);
                                  goto PrintByRoutine;
                              case PROPERTY_MK:
                                  AO = veneer_routine(Print__Pname_VR);
                                  goto PrintByRoutine;
                              default:
               error_named("A reserved word was used as a print specification:",
                                      token_text);
                          }
                          break;

                        case SYMBOL_TT:
                          if (symbols[token_value].flags & UNKNOWN_SFLAG)
                          {   INITAOT(&AO, LONG_CONSTANT_OT);
                              AO.value = token_value;
                              AO.marker = SYMBOL_MV;
                              AO.symindex = token_value;
                          }
                          else
                          {   INITAOT(&AO, LONG_CONSTANT_OT);
                              AO.value = symbols[token_value].value;
                              AO.marker = IROUTINE_MV;
                              AO.symindex = token_value;
                              if (symbols[token_value].type != ROUTINE_T)
                                ebf_curtoken_error("printing routine name");
                          }
                          symbols[token_value].flags |= USED_SFLAG;

                          PrintByRoutine:

                          get_next_token();
                          if (version_number >= 5)
                            assemblez_2(call_2n_zc, AO,
                              code_generate(parse_expression(QUANTITY_CONTEXT),
                                QUANTITY_CONTEXT, -1));
                          else if (version_number == 4)
                            assemblez_2_to(call_vs_zc, AO,
                              code_generate(parse_expression(QUANTITY_CONTEXT),
                                QUANTITY_CONTEXT, -1), temp_var1);
                          else
                            assemblez_2_to(call_zc, AO,
                              code_generate(parse_expression(QUANTITY_CONTEXT),
                                QUANTITY_CONTEXT, -1), temp_var1);
                          goto PrintTermDone;

                        default: ebf_curtoken_error("print specification");
                          get_next_token();
                          assemblez_1(print_num_zc,
                          code_generate(parse_expression(QUANTITY_CONTEXT),
                                QUANTITY_CONTEXT, -1));
                          goto PrintTermDone;
                      }
                  }
                  put_token_back(); put_token_back(); put_token_back();
                  misc_keywords.enabled = FALSE;
                  assemblez_1(print_num_zc,
                      code_generate(parse_expression(QUANTITY_CONTEXT),
                          QUANTITY_CONTEXT, -1));
                  break;
              }
              /* Fall through */

            default:
              put_token_back(); misc_keywords.enabled = FALSE;
              assemblez_1(print_num_zc,
                  code_generate(parse_expression(QUANTITY_CONTEXT),
                      QUANTITY_CONTEXT, -1));
              break;
        }

        PrintTermDone: misc_keywords.enabled = FALSE;

        count++;
        get_next_token();
        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) break;
        if ((token_type != SEP_TT) || (token_value != COMMA_SEP))
        {   ebf_curtoken_error("comma");
            panic_mode_error_recovery(); return;
        }
        else get_next_token();
    } while(TRUE);

    if (count == 0) ebf_curtoken_error("something to print");
    if (finally_return)
    {   assemblez_0(new_line_zc);
        assemblez_0(rtrue_zc);
    }
}

/* Direct-IR mirrors of the emissions below; each is a quiet no-op when
   direct generation is inactive or has rejected the routine. */
static void direct_stream_char(int32 c)
{
    llvm_direct_value v = llvm_direct_constant(c, 0, 0);
    if (v) (void)llvm_direct_glulx_op("streamchar", &v, 1);
}

static void direct_stream_op(const char *opcode, llvm_direct_value v)
{
    if (v) (void)llvm_direct_glulx_op(opcode, &v, 1);
}

/* Read or write an objectloop-style variable operand (a local or a
   global; the parser has already rejected anything else). */
static llvm_direct_value direct_variable_value(assembly_operand AO)
{
    if (AO.type == LOCALVAR_OT) return llvm_direct_load_local(AO.value);
    return llvm_direct_load_global(AO.value);
}

static void direct_variable_store(assembly_operand AO, llvm_direct_value v)
{
    if (!v) return;
    if (AO.type == LOCALVAR_OT)
        (void)llvm_direct_store_local_value(AO.value, v);
    else
        (void)llvm_direct_store_global_value(AO.value, v);
}

/* aload of an object field, the classic object-tree access. */
static llvm_direct_value direct_object_field(llvm_direct_value obj,
    int32 field)
{
    llvm_direct_value args[2];
    args[0] = obj;
    args[1] = llvm_direct_constant(field, 0, 0);
    if (!args[0] || !args[1]) return NULL;
    return llvm_direct_glulx_op("aload", args, 2);
}

/* Call a routine named by an operand (a veneer routine or a print
   routine symbol), discarding the result. */
static void direct_operand_call(assembly_operand fn,
    llvm_direct_value *args, int count)
{
    llvm_direct_value f;
    int i;
    for (i = 0; i < count; i++)
        if (!args[i]) return;
    f = llvm_direct_constant(fn.value, fn.marker, fn.symindex);
    if (f) (void)llvm_direct_call(f, args, count);
}

static void parse_print_g(int finally_return)
{   int count = 0; assembly_operand AO, AO2;

    /*  print <printlist> -------------------------------------------------- */
    /*  print_ret <printlist> ---------------------------------------------- */
    /*  <literal-string> --------------------------------------------------- */
    /*                                                                       */
    /*  <printlist> is a comma-separated list of items:                      */
    /*                                                                       */
    /*       <literal-string>                                                */
    /*       <other-expression>                                              */
    /*       (char) <expression>                                             */
    /*       (address) <expression>                                          */
    /*       (string) <expression>                                           */
    /*       (a) <expression>                                                */
    /*       (A) <expression>                                                */
    /*       (the) <expression>                                              */
    /*       (The) <expression>                                              */
    /*       (name) <expression>                                             */
    /*       (number) <expression>                                           */
    /*       (property) <expression>                                         */
    /*       (<routine>) <expression>                                        */
    /*       (object) <expression>     (for use in low-level code only)      */
    /* --------------------------------------------------------------------- */

    do
    {   
        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) break;
        switch(token_type)
        {   case DQ_TT:
              if (token_text[0] == '^' && token_text[1] == '\0') {
                  /* The string "^" is always a simple newline. */
                  INITAOTV(&AO, BYTECONSTANT_OT, 0x0A);
                  direct_stream_char(0x0A);
                  assembleg_1(streamchar_gc, AO);
                  break;
              }
              /* We can't compile a string into the instruction,
                 so this always goes into the string area. */
              {   INITAOT(&AO, CONSTANT_OT);
                  AO.marker = STRING_MV;
                  AO.value  = compile_string(token_text, STRCTX_GAME);
                  direct_stream_op("streamstr",
                      llvm_direct_constant(AO.value, AO.marker, AO.symindex));
                  assembleg_1(streamstr_gc, AO);
                  if (finally_return)
                  {   get_next_token();
                      if ((token_type == SEP_TT)
                          && (token_value == SEMICOLON_SEP))
                      {   INITAOTV(&AO, BYTECONSTANT_OT, 0x0A);
                          direct_stream_char(0x0A);
                          llvm_direct_return_constant(1);
                          assembleg_1(streamchar_gc, AO);
                          INITAOTV(&AO, BYTECONSTANT_OT, 1);
                          assembleg_1(return_gc, AO);
                          return;
                      }
                      put_token_back();
                  }
                  break;
              }
              break;

            case SEP_TT:
              if (token_value == OPENB_SEP)
              {   misc_keywords.enabled = TRUE;
                  get_next_token();
                  get_next_token();
                  if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP))
                  {   assembly_operand AO1;

                      put_token_back(); put_token_back();
                      local_variables.enabled = FALSE;
                      get_next_token();
                      misc_keywords.enabled = FALSE;
                      local_variables.enabled = TRUE;

                      if ((token_type == STATEMENT_TT)
                          &&(token_value == STRING_CODE))
                      {   token_type = MISC_KEYWORD_TT;
                          token_value = STRING_MK;
                      }

                      switch(token_type)
                      {
                        case MISC_KEYWORD_TT:
                          switch(token_value)
                          {   case CHAR_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintC_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  {   assembly_operand tree =
                                          parse_expression(QUANTITY_CONTEXT);
                                      llvm_direct_value dv =
                                          llvm_direct_quantity(tree);
                                      AO1 = code_generate(tree,
                                          QUANTITY_CONTEXT, -1);
                                      if (is_constant_ot(AO1.type)
                                          && AO1.marker == 0
                                          && AO1.value >= 0
                                          && AO1.value < 0x100)
                                          direct_stream_op("streamchar", dv);
                                      else
                                          direct_stream_op("streamunichar",
                                              dv);
                                  }
                                  if (is_constant_ot(AO1.type) && AO1.marker == 0) {
                                      if (AO1.value >= 0 && AO1.value < 0x100)
                                          assembleg_1(streamchar_gc, AO1);
                                      else
                                          assembleg_1(streamunichar_gc, AO1);
                                  }
                                  else {
                                      assembleg_1(streamunichar_gc, AO1);
                                  }
                                  goto PrintTermDone;
                              case ADDRESS_MK:
                                  if (runtime_error_checking_switch)
                                      AO = veneer_routine(RT__ChPrintA_VR);
                                  else
                                      AO = veneer_routine(Print__Addr_VR);
                                  goto PrintByRoutine;
                              case STRING_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintS_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  {   assembly_operand tree =
                                          parse_expression(QUANTITY_CONTEXT);
                                      direct_stream_op("streamstr",
                                          llvm_direct_quantity(tree));
                                      AO1 = code_generate(tree,
                                          QUANTITY_CONTEXT, -1);
                                  }
                                  assembleg_1(streamstr_gc, AO1);
                                  goto PrintTermDone;
                              case OBJECT_MK:
                                  if (runtime_error_checking_switch)
                                  {   AO = veneer_routine(RT__ChPrintO_VR);
                                      goto PrintByRoutine;
                                  }
                                  get_next_token();
                                  {   assembly_operand tree =
                                          parse_expression(QUANTITY_CONTEXT);
                                      llvm_direct_value args[2];
                                      args[0] = llvm_direct_quantity(tree);
                                      args[1] = llvm_direct_constant(
                                          GOBJFIELD_NAME(), 0, 0);
                                      if (args[0] && args[1])
                                          direct_stream_op("streamstr",
                                              llvm_direct_glulx_op("aload",
                                                  args, 2));
                                      AO1 = code_generate(tree,
                                          QUANTITY_CONTEXT, -1);
                                  }
                                  INITAOT(&AO2, BYTECONSTANT_OT);
                                  AO2.value = GOBJFIELD_NAME();
                                  assembleg_3(aload_gc, AO1, AO2,
                                    stack_pointer);
                                  assembleg_1(streamstr_gc, stack_pointer);
                                  goto PrintTermDone;
                              case THE_MK:
                                  AO = veneer_routine(DefArt_VR);
                                  goto PrintByRoutine;
                              case AN_MK:
                              case A_MK:
                                  AO = veneer_routine(InDefArt_VR);
                                  goto PrintByRoutine;
                              case CAP_THE_MK:
                                  AO = veneer_routine(CDefArt_VR);
                                  goto PrintByRoutine;
                              case CAP_A_MK:
                                  AO = veneer_routine(CInDefArt_VR);
                                  goto PrintByRoutine;
                              case NAME_MK:
                                  AO = veneer_routine(PrintShortName_VR);
                                  goto PrintByRoutine;
                              case NUMBER_MK:
                                  AO = veneer_routine(EnglishNumber_VR);
                                  goto PrintByRoutine;
                              case PROPERTY_MK:
                                  AO = veneer_routine(Print__Pname_VR);
                                  goto PrintByRoutine;
                              default:
               error_named("A reserved word was used as a print specification:",
                                      token_text);
                          }
                          break;

                        case SYMBOL_TT:
                          if (symbols[token_value].flags & UNKNOWN_SFLAG)
                          {   INITAOT(&AO, CONSTANT_OT);
                              AO.value = token_value;
                              AO.marker = SYMBOL_MV;
                              AO.symindex = token_value;
                          }
                          else
                          {   INITAOT(&AO, CONSTANT_OT);
                              /* Defer the address through the symbol so
                                 end-of-pass address assignment can set it;
                                 same final address (Glulx). */
                              AO.value = token_value;
                              AO.marker = SYMBOL_MV;
                              AO.symindex = token_value;
                              if (symbols[token_value].type != ROUTINE_T)
                                ebf_curtoken_error("printing routine name");
                          }
                          symbols[token_value].flags |= USED_SFLAG;

                          PrintByRoutine:

                          get_next_token();
                          INITAOT(&AO2, ZEROCONSTANT_OT);
                          {   assembly_operand tree =
                                  parse_expression(QUANTITY_CONTEXT);
                              llvm_direct_value dv =
                                  llvm_direct_quantity(tree);
                              direct_operand_call(AO, &dv, 1);
                              assembleg_call_1(AO,
                                code_generate(tree, QUANTITY_CONTEXT, -1),
                                AO2);
                          }
                          goto PrintTermDone;

                        default: ebf_curtoken_error("print specification");
                          get_next_token();
                          {   assembly_operand tree =
                                  parse_expression(QUANTITY_CONTEXT);
                              direct_stream_op("streamnum",
                                  llvm_direct_quantity(tree));
                              assembleg_1(streamnum_gc,
                                  code_generate(tree, QUANTITY_CONTEXT, -1));
                          }
                          goto PrintTermDone;
                      }
                  }
                  put_token_back(); put_token_back(); put_token_back();
                  misc_keywords.enabled = FALSE;
                  {   assembly_operand tree =
                          parse_expression(QUANTITY_CONTEXT);
                      direct_stream_op("streamnum",
                          llvm_direct_quantity(tree));
                      assembleg_1(streamnum_gc,
                          code_generate(tree, QUANTITY_CONTEXT, -1));
                  }
                  break;
              }
              /* Fall through */

            default:
              put_token_back(); misc_keywords.enabled = FALSE;
              {   assembly_operand tree = parse_expression(QUANTITY_CONTEXT);
                  direct_stream_op("streamnum", llvm_direct_quantity(tree));
                  assembleg_1(streamnum_gc,
                      code_generate(tree, QUANTITY_CONTEXT, -1));
              }
              break;
        }

        PrintTermDone: misc_keywords.enabled = FALSE;

        count++;
        get_next_token();
        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) break;
        if ((token_type != SEP_TT) || (token_value != COMMA_SEP))
        {   ebf_curtoken_error("comma");
            panic_mode_error_recovery(); return;
        }
        else get_next_token();
    } while(TRUE);

    if (count == 0) ebf_curtoken_error("something to print");
    if (finally_return)
    {
        direct_stream_char(0x0A);
        llvm_direct_return_constant(1);
        INITAOTV(&AO, BYTECONSTANT_OT, 0x0A);
        assembleg_1(streamchar_gc, AO);
        INITAOTV(&AO, BYTECONSTANT_OT, 1);
        assembleg_1(return_gc, AO);
    }
}

/* Parse any number of ".Label;" lines before a statement.
   Returns whether a statement can in fact follow. */
static int parse_named_label_statements()
{
    while ((token_type == SEP_TT) && (token_value == PROPERTY_SEP))
    {   /*  That is, a full stop, signifying a label  */

        get_next_token();
        if (token_type != SYMBOL_TT)
        {
            ebf_curtoken_error("label name");
            return TRUE;
        }

        if (symbols[token_value].flags & UNKNOWN_SFLAG)
        {   int label = alloc_label();
            assign_symbol(token_value, label, LABEL_T);
            symbols[token_value].flags |= USED_SFLAG;
            assemble_label_no(label);
            define_symbol_label(token_value);
            llvm_direct_bind_label(label);
        }
        else
        {   if (symbols[token_value].type != LABEL_T) {
                ebf_curtoken_error("label name");
                return TRUE;
            }
            if (symbols[token_value].flags & CHANGE_SFLAG)
            {   symbols[token_value].flags &= (~(CHANGE_SFLAG));
                assemble_label_no(symbols[token_value].value);
                define_symbol_label(token_value);
                llvm_direct_bind_label(symbols[token_value].value);
            }
            else error_named("Duplicate definition of label:", token_text);
        }

        get_next_token();
        if ((token_type != SEP_TT) || (token_value != SEMICOLON_SEP))
        {   ebf_curtoken_error("';'");
            put_token_back(); return FALSE;
        }

        /*  Interesting point of Inform grammar: a statement can only
            consist solely of a label when it is immediately followed
            by a "}".                                                    */

        get_next_token();
        if ((token_type == SEP_TT) && (token_value == CLOSE_BRACE_SEP))
        {   put_token_back(); return FALSE;
        }
        /* The following line prevents labels from influencing the positions
           of sequence points. */
        statement_debug_location = get_token_location();
        
        /* Another label might follow */
    }

    /* On with the statement */
    return TRUE;
}

static void parse_statement_z(int break_label, int continue_label)
{   int ln, ln2, ln3, ln4, flag;
    int pre_unreach, labelexists;
    assembly_operand AO, AO2, AO3, AO4;
    debug_location spare_debug_location1, spare_debug_location2;

    ASSERT_ZCODE();

    if ((token_type == SEP_TT) && (token_value == HASH_SEP))
    {   parse_directive(TRUE);
        parse_statement(break_label, continue_label); return;
    }

    if ((token_type == SEP_TT) && (token_value == AT_SEP))
    {   parse_assembly(); return;
    }

    if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) return;

    if ((token_type == SEP_TT) && (token_value == OPEN_BRACE_SEP))
    {
        put_token_back();
        parse_code_block(break_label, continue_label, 0);
        return;
    }
    
    if (token_type == DQ_TT)
    {   parse_print_z(TRUE); return;
    }

    if ((token_type == SEP_TT) && (token_value == LESS_SEP))
    {   parse_action(); goto StatementTerminator; }

    if (token_type == EOF_TT)
    {   ebf_curtoken_error("statement"); return; }

    /* If we don't see a keyword, this must be a function call or
       other expression-with-side-effects. */
    if (token_type != STATEMENT_TT)
    {   put_token_back();
        AO = parse_expression(VOID_CONTEXT);
        code_generate(AO, VOID_CONTEXT, -1);
        if (vivc_flag) { panic_mode_error_recovery(); return; }
        goto StatementTerminator;
    }

    statements.enabled = FALSE;

    switch(token_value)
    {
    /*  -------------------------------------------------------------------- */
    /*  box <string-1> ... <string-n> -------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case BOX_CODE:
             if (version_number == 3)
                 warning("The 'box' statement has no effect in a version 3 game");
             INITAOT(&AO3, LONG_CONSTANT_OT);
                 AO3.value = begin_table_array();
                 AO3.marker = ARRAY_MV;
                 ln = 0; ln2 = 0;
                 do
                 {   get_next_token();
                     if ((token_type==SEP_TT)&&(token_value==SEMICOLON_SEP))
                         break;
                     if (token_type != DQ_TT)
                         ebf_curtoken_error("text of box line in double-quotes");
                     {   int i, j;
                         for (i=0, j=0; token_text[i] != 0; j++)
                             if (token_text[i] == '@')
                             {   if (token_text[i+1] == '@')
                                 {   i = i + 2;
                                     while (isdigit((uchar)token_text[i])) i++;
                                 }
                                 else
                                 {   i++;
                                     if (token_text[i] != 0) i++;
                                     if (token_text[i] != 0) i++;
                                 }
                             }
                             else i++;
                         if (j > ln2) ln2 = j;
                     }
                     put_token_back();
                     array_entry(ln++, FALSE, parse_expression(CONSTANT_CONTEXT));
                 } while (TRUE);
                 finish_array(ln, FALSE);
                 if (ln == 0)
                     error("No lines of text given for 'box' display");

                 if (version_number == 3) return;

                 INITAOTV(&AO2, SHORT_CONSTANT_OT, ln2);
                 INITAOTV(&AO4, VARIABLE_OT, globalv_z_temp_var1);
                 assemblez_3_to(call_vs_zc, veneer_routine(Box__Routine_VR),
                     AO2, AO3, AO4);
                 return;

    /*  -------------------------------------------------------------------- */
    /*  break -------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case BREAK_CODE:
                 if (break_label == -1)
                 error("'break' can only be used in a loop or 'switch' block");
                 else
                     assemblez_jump(break_label);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  continue ----------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case CONTINUE_CODE:
                 if (continue_label == -1)
                 error("'continue' can only be used in a loop block");
                 else
                     assemblez_jump(continue_label);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  do <codeblock> until (<condition>) --------------------------------- */
    /*  -------------------------------------------------------------------- */

        case DO_CODE:
                 assemble_label_no(ln = alloc_label());
                 ln2 = alloc_label(); ln3 = alloc_label();
                 parse_code_block(ln3, ln2, 0);
                 statements.enabled = TRUE;
                 get_next_token();
                 if ((token_type == STATEMENT_TT)
                     && (token_value == UNTIL_CODE))
                 {   assemble_forward_label_no(ln2);
                     match_open_bracket();
                     AO = parse_expression(CONDITION_CONTEXT);
                     match_close_bracket();
                     code_generate(AO, CONDITION_CONTEXT, ln);
                 }
                 else error("'do' without matching 'until'");

                 assemble_forward_label_no(ln3);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  font on/off -------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case FONT_CODE:
                 misc_keywords.enabled = TRUE;
                 get_next_token();
                 misc_keywords.enabled = FALSE;
                 if ((token_type != MISC_KEYWORD_TT)
                     || ((token_value != ON_MK)
                         && (token_value != OFF_MK)))
                 {   ebf_curtoken_error("'on' or 'off'");
                     panic_mode_error_recovery();
                     break;
                 }

                 if (version_number >= 5)
                 {   /* Use the V5 @set_font opcode, setting font 4
                        (for font off) or 1 (for font on). */
                     INITAOT(&AO, SHORT_CONSTANT_OT);
                     if (token_value == ON_MK)
                         AO.value = 1;
                     else
                         AO.value = 4;
                     assemblez_1_to(set_font_zc, AO, temp_var1);
                     break;
                 }

                 /* Set the fixed-pitch header bit. */
                 INITAOTV(&AO, SHORT_CONSTANT_OT, 0);
                 INITAOTV(&AO2, SHORT_CONSTANT_OT, 8);
                 INITAOTV(&AO3, VARIABLE_OT, globalv_z_temp_var1);
                 assemblez_2_to(loadw_zc, AO, AO2, AO3);

                 if (token_value == ON_MK)
                 {   INITAOTV(&AO4, LONG_CONSTANT_OT, 0xfffd);
                     assemblez_2_to(and_zc, AO4, AO3, AO3);
                 }
                 else
                 {   INITAOTV(&AO4, SHORT_CONSTANT_OT, 2);
                     assemblez_2_to(or_zc, AO4, AO3, AO3);
                 }

                 assemblez_3(storew_zc, AO, AO2, AO3);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  for (<initialisation> : <continue-condition> : <updating>) --------- */
    /*  -------------------------------------------------------------------- */

        /*  Note that it's legal for any or all of the three sections of a
            'for' specification to be empty.  This 'for' implementation
            often wastes 3 bytes with a redundant branch rather than keep
            expression parse trees for long periods (as previous versions
            of Inform did, somewhat crudely by simply storing the textual
            form of a 'for' loop).  It is adequate for now.                  */

        case FOR_CODE:
                 match_open_bracket();
                 get_next_token();

                 /*  Initialisation code  */
                 AO.type = OMITTED_OT;
                 spare_debug_location1 = statement_debug_location;
                 AO2.type = OMITTED_OT; flag = 0;
                 spare_debug_location2 = statement_debug_location;

                 if (!((token_type==SEP_TT)&&(token_value==COLON_SEP)))
                 {   put_token_back();
                     if (!((token_type==SEP_TT)&&(token_value==SUPERCLASS_SEP)))
                     {   sequence_point_follows = TRUE;
                         statement_debug_location = get_token_location();
                         code_generate(parse_expression(FORINIT_CONTEXT),
                             VOID_CONTEXT, -1);
                     }
                     get_next_token();
                     if ((token_type==SEP_TT)&&(token_value == SUPERCLASS_SEP))
                     {   get_next_token();
                         if ((token_type==SEP_TT)&&(token_value == CLOSEB_SEP))
                         {   assemble_label_no(ln = alloc_label());
                             ln2 = alloc_label();
                             parse_code_block(ln2, ln, 0);
                             sequence_point_follows = FALSE;
                             if (!execution_never_reaches_here)
                                 assemblez_jump(ln);
                             assemble_forward_label_no(ln2);
                             return;
                         }
                         goto ParseUpdate;
                     }
                     put_token_back();
                     if (!match_colon()) break;
                 }

                 get_next_token();
                 if (!((token_type==SEP_TT)&&(token_value==COLON_SEP)))
                 {   put_token_back();
                     spare_debug_location1 = get_token_location();
                     AO = parse_expression(CONDITION_CONTEXT);
                     if (!match_colon()) break;
                 }
                 get_next_token();

                 ParseUpdate:
                 if (!((token_type==SEP_TT)&&(token_value==CLOSEB_SEP)))
                 {   put_token_back();
                     spare_debug_location2 = get_token_location();
                     AO2 = parse_expression(VOID_CONTEXT);
                     match_close_bracket();
                     flag = test_for_incdec(AO2);
                 }

                 ln = alloc_label();
                 ln2 = alloc_label();
                 ln3 = alloc_label();

                 if ((AO2.type == OMITTED_OT) || (flag != 0))
                 {
                     assemble_label_no(ln);
                     if (flag==0) assemble_label_no(ln2);

                     /*  The "finished yet?" condition  */

                     if (AO.type != OMITTED_OT)
                     {   sequence_point_follows = TRUE;
                         statement_debug_location = spare_debug_location1;
                         code_generate(AO, CONDITION_CONTEXT, ln3);
                     }

                 }
                 else
                 {
                     /*  This is the jump which could be avoided with the aid
                         of long-term expression storage  */

                     sequence_point_follows = FALSE;
                     assemblez_jump(ln2);

                     /*  The "update" part  */

                     assemble_label_no(ln);
                     sequence_point_follows = TRUE;
                     statement_debug_location = spare_debug_location2;
                     code_generate(AO2, VOID_CONTEXT, -1);

                     assemble_label_no(ln2);

                     /*  The "finished yet?" condition  */

                     if (AO.type != OMITTED_OT)
                     {   sequence_point_follows = TRUE;
                         statement_debug_location = spare_debug_location1;
                         code_generate(AO, CONDITION_CONTEXT, ln3);
                     }
                 }

                 if (flag != 0)
                 {
                     /*  In this optimised case, update code is at the end
                         of the loop block, so "continue" goes there  */

                     parse_code_block(ln3, ln2, 0);
                     assemble_label_no(ln2);

                     sequence_point_follows = TRUE;
                     statement_debug_location = spare_debug_location2;
                     if (flag > 0)
                     {   INITAOTV(&AO3, SHORT_CONSTANT_OT, flag);
                         assemblez_1(inc_zc, AO3);
                     }
                     else
                     {   INITAOTV(&AO3, SHORT_CONSTANT_OT, -flag);
                         assemblez_1(dec_zc, AO3);
                     }
                     assemblez_jump(ln);
                 }
                 else
                 {
                     /*  In the unoptimised case, update code is at the
                         start of the loop block, so "continue" goes there  */

                     parse_code_block(ln3, ln, 0);
                     if (!execution_never_reaches_here)
                     {   sequence_point_follows = FALSE;
                         assemblez_jump(ln);
                     }
                 }

                 assemble_forward_label_no(ln3);
                 return;

    /*  -------------------------------------------------------------------- */
    /*  give <expression> [~]attr [, [~]attr [, ...]] ---------------------- */
    /*  -------------------------------------------------------------------- */

        case GIVE_CODE:
                 AO = code_generate(parse_expression(QUANTITY_CONTEXT),
                          QUANTITY_CONTEXT, -1);
                 check_warn_symbol_type(&AO, OBJECT_T, 0, "\"give\" statement");
                 if ((AO.type == VARIABLE_OT) && (AO.value == 0))
                 {   INITAOTV(&AO, SHORT_CONSTANT_OT, 252);
                     if (version_number != 6) assemblez_1(pull_zc, AO);
                     else assemblez_0_to(pull_zc, AO);
                     AO.type = VARIABLE_OT;
                 }

                 do
                 {   get_next_token();
                     if ((token_type == SEP_TT)&&(token_value == SEMICOLON_SEP))
                         return;
                     if ((token_type == SEP_TT)&&(token_value == ARTNOT_SEP))
                         ln = clear_attr_zc;
                     else
                     {   ln = set_attr_zc;
                         put_token_back();
                     }
                     AO2 = code_generate(parse_expression(QUANTITY_CONTEXT),
                               QUANTITY_CONTEXT, -1);
                     check_warn_symbol_type(&AO2, ATTRIBUTE_T, 0, "\"give\" statement");
                     if (runtime_error_checking_switch)
                     {   ln2 = (ln==set_attr_zc)?RT__ChG_VR:RT__ChGt_VR;
                         if (version_number >= 5)
                             assemblez_3(call_vn_zc, veneer_routine(ln2),
                             AO, AO2);
                         else
                         {   
                             assemblez_3_to(call_zc, veneer_routine(ln2),
                                 AO, AO2, temp_var1);
                         }
                     }
                     else
                         assemblez_2(ln, AO, AO2);
                 } while(TRUE);

    /*  -------------------------------------------------------------------- */
    /*  if (<condition>) <codeblock> [else <codeblock>] -------------------- */
    /*  -------------------------------------------------------------------- */

        case IF_CODE:
                 flag = FALSE; /* set if there's an "else" */
                 ln2 = 0;
                 pre_unreach = execution_never_reaches_here;

                 match_open_bracket();
                 AO = parse_expression(CONDITION_CONTEXT);
                 match_close_bracket();

                 statements.enabled = TRUE;
                 get_next_token();
                 if ((token_type == STATEMENT_TT)&&(token_value == RTRUE_CODE))
                     ln = -4;
                 else
                 if ((token_type == STATEMENT_TT)&&(token_value == RFALSE_CODE))
                     ln = -3;
                 else
                 {   put_token_back();
                     ln = alloc_label();
                 }

                 /* The condition */
                 code_generate(AO, CONDITION_CONTEXT, ln);

                 if (!pre_unreach && ln >= 0 && execution_never_reaches_here) {
                     /* If the condition never falls through to here, then
                        it was an "if (0)" test. Our convention is to skip
                        the "not reached" warnings for this case. */
                     execution_never_reaches_here |= EXECSTATE_NOWARN;
                 }

                 /* The "if" block */
                 if (ln >= 0) parse_code_block(break_label, continue_label, 0);
                 else
                 {   get_next_token();
                     if ((token_type != SEP_TT)
                         || (token_value != SEMICOLON_SEP))
                     {   ebf_curtoken_error("';'");
                         put_token_back();
                     }
                 }

                 statements.enabled = TRUE;
                 get_next_token();

                 /* An #if directive around the ELSE clause is legal. */
                 while ((token_type == SEP_TT) && (token_value == HASH_SEP))
                 {   parse_directive(TRUE);
                     statements.enabled = TRUE;
                     get_next_token();
                 }
                 
                 if ((token_type == STATEMENT_TT) && (token_value == ELSE_CODE))
                 {   flag = TRUE;
                     if (ln >= 0)
                     {   ln2 = alloc_label();
                         if (!execution_never_reaches_here)
                         {   sequence_point_follows = FALSE;
                             assemblez_jump(ln2);
                         }
                     }
                 }
                 else put_token_back();

                 /* The "else" label (or end of statement, if there is no "else") */
                 labelexists = FALSE;
                 if (ln >= 0) labelexists = assemble_forward_label_no(ln);

                 if (flag)
                 {
                     /* If labelexists is false, then we started with
                        "if (1)". In this case, we don't want a "not
                        reached" warning on the "else" block. We
                        temporarily disable the NOWARN flag, and restore it
                        afterwards. */
                     int saved_unreach = 0;
                     if (execution_never_reaches_here && !labelexists) {
                         saved_unreach = execution_never_reaches_here;
                         execution_never_reaches_here |= EXECSTATE_NOWARN;
                     }

                     /* The "else" block */
                     parse_code_block(break_label, continue_label, 0);

                     if (execution_never_reaches_here && !labelexists) {
                         if (saved_unreach & EXECSTATE_NOWARN)
                             execution_never_reaches_here |= EXECSTATE_NOWARN;
                         else
                             execution_never_reaches_here &= ~EXECSTATE_NOWARN;
                     }

                     /* The post-"else" label */
                     if (ln >= 0) assemble_forward_label_no(ln2);
                 }
                 else
                 {
                     /* There was no "else". If we're unreachable, then the
                        statement returned unconditionally, which means 
                        "if (1) return". Skip warnings. */
                     if (!pre_unreach && execution_never_reaches_here) {
                         execution_never_reaches_here |= EXECSTATE_NOWARN;
                     }
                 }
                         
                 return;

    /*  -------------------------------------------------------------------- */
    /*  inversion ---------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case INVERSION_CODE:
                 INITAOTV(&AO, SHORT_CONSTANT_OT, 0);
                 INITAOT(&AO2, SHORT_CONSTANT_OT);

                 AO2.value  = 60;
                 assemblez_2_to(loadb_zc, AO, AO2, temp_var1);
                 assemblez_1(print_char_zc, temp_var1);
                 AO2.value  = 61;
                 assemblez_2_to(loadb_zc, AO, AO2, temp_var1);
                 assemblez_1(print_char_zc, temp_var1);
                 AO2.value  = 62;
                 assemblez_2_to(loadb_zc, AO, AO2, temp_var1);
                 assemblez_1(print_char_zc, temp_var1);
                 AO2.value  = 63;
                 assemblez_2_to(loadb_zc, AO, AO2, temp_var1);
                 assemblez_1(print_char_zc, temp_var1);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  jump <label> ------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case JUMP_CODE:
                 assemblez_jump(parse_label());
                 break;

    /*  -------------------------------------------------------------------- */
    /*  move <expression> to <expression> ---------------------------------- */
    /*  -------------------------------------------------------------------- */

        case MOVE_CODE:
                 misc_keywords.enabled = TRUE;
                 AO = parse_expression(QUANTITY_CONTEXT);

                 get_next_token();
                 misc_keywords.enabled = FALSE;
                 if ((token_type != MISC_KEYWORD_TT)
                     || (token_value != TO_MK))
                 {   ebf_curtoken_error("'to'");
                     panic_mode_error_recovery();
                     return;
                 }

                 AO2 = code_generate(parse_expression(QUANTITY_CONTEXT),
                     QUANTITY_CONTEXT, -1);
                 AO = code_generate(AO, QUANTITY_CONTEXT, -1);
                 check_warn_symbol_type(&AO, OBJECT_T, 0, "\"move\" statement");
                 check_warn_symbol_type(&AO2, OBJECT_T, CLASS_T, "\"move\" statement");
                 if ((runtime_error_checking_switch) && (veneer_mode == FALSE))
                 {   if (version_number >= 5)
                         assemblez_3(call_vn_zc, veneer_routine(RT__ChT_VR),
                             AO, AO2);
                     else
                     {   assemblez_3_to(call_zc, veneer_routine(RT__ChT_VR),
                             AO, AO2, temp_var1);
                     }
                 }
                 else
                     assemblez_2(insert_obj_zc, AO, AO2);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  new_line ----------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case NEW_LINE_CODE:  assemblez_0(new_line_zc); break;

    /*  -------------------------------------------------------------------- */
    /*  objectloop (<initialisation>) <codeblock> -------------------------- */
    /*  -------------------------------------------------------------------- */

        case OBJECTLOOP_CODE:

                 match_open_bracket();
                 get_next_token();
                 INITAOT(&AO, VARIABLE_OT);
                 if (token_type == LOCAL_VARIABLE_TT)
                     AO.value = token_value;
                 else
                 if ((token_type == SYMBOL_TT) &&
                     (symbols[token_value].type == GLOBAL_VARIABLE_T))
                     AO.value = symbols[token_value].value;
                 else
                 {   ebf_curtoken_error("'objectloop' variable");
                     panic_mode_error_recovery(); break;
                 }
                 misc_keywords.enabled = TRUE;
                 get_next_token(); flag = TRUE;
                 misc_keywords.enabled = FALSE;
                 if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP))
                     flag = FALSE;

                 ln = 0;
                 if ((token_type == MISC_KEYWORD_TT)
                     && (token_value == NEAR_MK)) ln = 1;
                 if ((token_type == MISC_KEYWORD_TT)
                     && (token_value == FROM_MK)) ln = 2;
                 if ((token_type == CND_TT) && (token_value == IN_COND))
                 {   get_next_token();
                     get_next_token();
                     if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP))
                         ln = 3;
                     put_token_back();
                     put_token_back();
                 }

                 if (ln > 0)
                 {   /*  Old style (Inform 5) objectloops: note that we
                         implement objectloop (a in b) in the old way since
                         this runs through objects in a different order from
                         the new way, and there may be existing Inform code
                         relying on this.                                    */
                     assembly_operand AO4;
                     INITAO(&AO4);

                     sequence_point_follows = TRUE;
                     AO2 = code_generate(parse_expression(QUANTITY_CONTEXT),
                         QUANTITY_CONTEXT, -1);
                     match_close_bracket();
                     if (ln == 1)
                     {   INITAOTV(&AO3, VARIABLE_OT, 0);
                         if (runtime_error_checking_switch)
                                 AO2 = check_nonzero_at_runtime(AO2, -1,
                                     OBJECTLOOP_RTE);
                         assemblez_1_to(get_parent_zc, AO2, AO3);
                         assemblez_objcode(get_child_zc, AO3, AO3, -2, TRUE);
                         AO2 = AO3;
                     }
                     if (ln == 3)
                     {   INITAOTV(&AO3, VARIABLE_OT, 0);
                         if (runtime_error_checking_switch)
                         {   AO4 = AO2;
                             AO2 = check_nonzero_at_runtime(AO2, -1,
                                 CHILD_RTE);
                         }
                         assemblez_objcode(get_child_zc, AO2, AO3, -2, TRUE);
                         AO2 = AO3;
                     }
                     assemblez_store(AO, AO2);
                     assemblez_1_branch(jz_zc, AO, ln2 = alloc_label(), TRUE);
                     assemble_label_no(ln4 = alloc_label());
                     parse_code_block(ln2, ln3 = alloc_label(), 0);
                     sequence_point_follows = FALSE;
                     assemble_label_no(ln3);
                     if (runtime_error_checking_switch)
                     {   AO2 = check_nonzero_at_runtime(AO, ln2,
                              OBJECTLOOP2_RTE);
                         if ((ln == 3)
                             && ((AO4.type != VARIABLE_OT)||(AO4.value != 0))
                             && ((AO4.type != VARIABLE_OT)
                                 ||(AO4.value != AO.value)))
                         {   int label = alloc_label();
                             assembly_operand en_ao;
                             INITAOTV(&en_ao, SHORT_CONSTANT_OT, OBJECTLOOP_BROKEN_RTE);
                             assemblez_2_branch(jin_zc, AO, AO4,
                                 label, TRUE);
                             assemblez_3(call_vn_zc, veneer_routine(RT__Err_VR),
                                 en_ao, AO);
                             assemblez_jump(ln2);
                             assemble_label_no(label);
                         }
                     }
                     else AO2 = AO;
                     assemblez_objcode(get_sibling_zc, AO2, AO, ln4, TRUE);
                     assemble_label_no(ln2);
                     return;
                 }

                 sequence_point_follows = TRUE;
                 INITAOTV(&AO2, SHORT_CONSTANT_OT, 1);
                 assemblez_store(AO, AO2);

                 assemble_label_no(ln = alloc_label());
                 ln2 = alloc_label();
                 ln3 = alloc_label();
                 if (flag)
                 {   put_token_back();
                     put_token_back();
                     sequence_point_follows = TRUE;
                     code_generate(parse_expression(CONDITION_CONTEXT),
                         CONDITION_CONTEXT, ln3);
                     match_close_bracket();
                 }
                 parse_code_block(ln2, ln3, 0);

                 sequence_point_follows = FALSE;
                 assemble_label_no(ln3);
                 assemblez_inc(AO);
                 INITAOTV(&AO2, LONG_CONSTANT_OT, no_objects);
                 AO2.marker = NO_OBJS_MV;
                 assemblez_2_branch(jg_zc, AO, AO2, ln2, TRUE);
                 assemblez_jump(ln);
                 assemble_label_no(ln2);
                 return;

    /*  -------------------------------------------------------------------- */
    /*  (see routine above) ------------------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case PRINT_CODE:
            get_next_token();
            parse_print_z(FALSE); return;
        case PRINT_RET_CODE:
            get_next_token();
            parse_print_z(TRUE); return;

    /*  -------------------------------------------------------------------- */
    /*  quit --------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case QUIT_CODE:      assemblez_0(quit_zc); break;

    /*  -------------------------------------------------------------------- */
    /*  read <expression> <expression> [<Routine>] ------------------------- */
    /*  -------------------------------------------------------------------- */

        case READ_CODE:
                 INITAOTV(&AO, VARIABLE_OT, globalv_z_temp_var4);
                 assemblez_store(AO,
                     code_generate(parse_expression(QUANTITY_CONTEXT),
                                   QUANTITY_CONTEXT, -1));
                 if (version_number > 3)
                 {   INITAOTV(&AO3, SHORT_CONSTANT_OT, 1);
                     INITAOTV(&AO4, SHORT_CONSTANT_OT, 0);
                     assemblez_3(storeb_zc, AO, AO3, AO4);
                 }
                 AO2 = code_generate(parse_expression(QUANTITY_CONTEXT),
                           QUANTITY_CONTEXT, -1);

                 get_next_token();
                 if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
                     put_token_back();
                 else
                 {   if (version_number == 3)
                         error(
"In Version 3 no status-line drawing routine can be given");
                     else
                     {   assembly_operand AO5;
                         /* Move the temp4 (buffer) value to the stack,
                            since the routine might alter temp4. */
                         assemblez_store(stack_pointer, AO);
                         AO = stack_pointer;
                         put_token_back();
                         AO5 = parse_expression(CONSTANT_CONTEXT);

                         if (version_number >= 5)
                             assemblez_1(call_1n_zc, AO5);
                         else
                             assemblez_1_to(call_zc, AO5, temp_var1);
                     }
                 }

                 if (version_number > 4)
                 {   assemblez_2_to(aread_zc, AO, AO2, temp_var1);
                 }
                 else assemblez_2(sread_zc, AO, AO2);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  remove <expression> ------------------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case REMOVE_CODE:
                 AO = code_generate(parse_expression(QUANTITY_CONTEXT),
                     QUANTITY_CONTEXT, -1);
                 check_warn_symbol_type(&AO, OBJECT_T, 0, "\"remove\" statement");
                 if ((runtime_error_checking_switch) && (veneer_mode == FALSE))
                 {   if (version_number >= 5)
                         assemblez_2(call_2n_zc, veneer_routine(RT__ChR_VR),
                             AO);
                     else
                     {   assemblez_2_to(call_zc, veneer_routine(RT__ChR_VR),
                             AO, temp_var1);
                     }
                 }
                 else
                     assemblez_1(remove_obj_zc, AO);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  restore <label> ---------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RESTORE_CODE:
                 if (version_number < 5)
                     assemblez_0_branch(restore_zc, parse_label(), TRUE);
                 else
                 {   INITAOTV(&AO2, SHORT_CONSTANT_OT, 2);
                     assemblez_0_to(restore_zc, temp_var1);
                     assemblez_2_branch(je_zc, temp_var1, AO2, parse_label(), TRUE);
                 }
                 break;

    /*  -------------------------------------------------------------------- */
    /*  return [<expression>] ---------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RETURN_CODE:
                 get_next_token();
                 if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
                 {   assemblez_0(rtrue_zc); return; }
                 put_token_back();
                 AO = code_generate(parse_expression(RETURN_Q_CONTEXT),
                     QUANTITY_CONTEXT, -1);
                 if ((AO.type == SHORT_CONSTANT_OT) && (AO.value == 0)
                     && (AO.marker == 0))
                 {   assemblez_0(rfalse_zc); break; }
                 if ((AO.type == SHORT_CONSTANT_OT) && (AO.value == 1)
                     && (AO.marker == 0))
                 {   assemblez_0(rtrue_zc); break; }
                 if ((AO.type == VARIABLE_OT) && (AO.value == 0))
                 {   assemblez_0(ret_popped_zc); break; }
                 assemblez_1(ret_zc, AO);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  rfalse ------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RFALSE_CODE:  assemblez_0(rfalse_zc); break;

    /*  -------------------------------------------------------------------- */
    /*  rtrue -------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RTRUE_CODE:   assemblez_0(rtrue_zc); break;

    /*  -------------------------------------------------------------------- */
    /*  save <label> ------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case SAVE_CODE:
                 if (version_number < 5)
                     assemblez_0_branch(save_zc, parse_label(), TRUE);
                 else
                 {   INITAOTV(&AO, VARIABLE_OT, globalv_z_temp_var1);
                     assemblez_0_to(save_zc, AO);
                     assemblez_1_branch(jz_zc, AO, parse_label(), FALSE);
                 }
                 break;

    /*  -------------------------------------------------------------------- */
    /*  spaces <expression> ------------------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case SPACES_CODE:
                 AO = code_generate(parse_expression(QUANTITY_CONTEXT),
                     QUANTITY_CONTEXT, -1);
                 INITAOTV(&AO2, VARIABLE_OT, globalv_z_temp_var1);

                 assemblez_store(AO2, AO);

                 INITAOTV(&AO, SHORT_CONSTANT_OT, 32);
                 INITAOTV(&AO3, SHORT_CONSTANT_OT, 1);

                 assemblez_2_branch(jl_zc, AO2, AO3, ln = alloc_label(), TRUE);
                 assemble_label_no(ln2 = alloc_label());
                 assemblez_1(print_char_zc, AO);
                 assemblez_dec(AO2);
                 assemblez_1_branch(jz_zc, AO2, ln2, FALSE);
                 assemble_label_no(ln);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  string <expression> <literal-string> ------------------------------- */
    /*  -------------------------------------------------------------------- */

        case STRING_CODE:
                 INITAOTV(&AO, SHORT_CONSTANT_OT, 0);
                 INITAOTV(&AO2, SHORT_CONSTANT_OT, 12);
                 INITAOTV(&AO3, VARIABLE_OT, globalv_z_temp_var4);
                 assemblez_2_to(loadw_zc, AO, AO2, AO3);
                 AO2 = code_generate(parse_expression(QUANTITY_CONTEXT),
                     QUANTITY_CONTEXT, -1);
                 if (is_constant_ot(AO2.type) && AO2.marker == 0) {
                     /* Compile-time check */
                     if (AO2.value < 0 || AO2.value >= 96 || AO2.value >= MAX_DYNAMIC_STRINGS) {
                         error_max_dynamic_strings(AO2.value);
                         AO2.value = 0;
                     }
                 }
                 get_next_token();
                 if (token_type == DQ_TT)
                 {   INITAOT(&AO4, LONG_CONSTANT_OT);
                     /* This string must be in low memory so that the
                        dynamic string table can refer to it. */
                     AO4.value = compile_string(token_text, STRCTX_LOWSTRING);
                 }
                 else
                 {   put_token_back();
                     AO4 = parse_expression(CONSTANT_CONTEXT);
                 }
                 assemblez_3(storew_zc, AO3, AO2, AO4);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  style roman/reverse/bold/underline/fixed --------------------------- */
    /*  -------------------------------------------------------------------- */

        case STYLE_CODE:
                 if (version_number==3)
                 {   error(
"The 'style' statement cannot be used for Version 3 games");
                     panic_mode_error_recovery();
                     break;
                 }

                 misc_keywords.enabled = TRUE;
                 get_next_token();
                 misc_keywords.enabled = FALSE;
                 if ((token_type != MISC_KEYWORD_TT)
                     || ((token_value != ROMAN_MK)
                         && (token_value != REVERSE_MK)
                         && (token_value != BOLD_MK)
                         && (token_value != UNDERLINE_MK)
                         && (token_value != FIXED_MK)))
                 {   ebf_curtoken_error(
"'roman', 'bold', 'underline', 'reverse' or 'fixed'");
                     panic_mode_error_recovery();
                     break;
                 }

                 INITAOT(&AO, SHORT_CONSTANT_OT);
                 switch(token_value)
                 {   case ROMAN_MK: AO.value = 0; break;
                     case REVERSE_MK: AO.value = 1; break;
                     case BOLD_MK: AO.value = 2; break;
                     case UNDERLINE_MK: AO.value = 4; break;
                     case FIXED_MK: AO.value = 8; break;
                 }
                 assemblez_1(set_text_style_zc, AO); break;

    /*  -------------------------------------------------------------------- */
    /*  switch (<expression>) <codeblock> ---------------------------------- */
    /*  -------------------------------------------------------------------- */

        case SWITCH_CODE:
                 match_open_bracket();
                 AO = code_generate(parse_expression(QUANTITY_CONTEXT),
                     QUANTITY_CONTEXT, -1);
                 match_close_bracket();

                 INITAOTV(&AO2, VARIABLE_OT, globalv_z_temp_var1);
                 assemblez_store(AO2, AO);

                 parse_code_block(ln = alloc_label(), continue_label, 1);
                 assemble_forward_label_no(ln);
                 return;

    /*  -------------------------------------------------------------------- */
    /*  while (<condition>) <codeblock> ------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case WHILE_CODE:
                 assemble_label_no(ln = alloc_label());
                 match_open_bracket();

                 code_generate(parse_expression(CONDITION_CONTEXT),
                     CONDITION_CONTEXT, ln2 = alloc_label());
                 match_close_bracket();

                 parse_code_block(ln2, ln, 0);
                 sequence_point_follows = FALSE;
                 if (!execution_never_reaches_here)
                     assemblez_jump(ln);
                 assemble_forward_label_no(ln2);
                 return;

    /*  -------------------------------------------------------------------- */

        case SDEFAULT_CODE:
                 error("'default' without matching 'switch'"); break;
        case ELSE_CODE:
                 error("'else' without matching 'if'"); break;
        case UNTIL_CODE:
                 error("'until' without matching 'do'");
                 panic_mode_error_recovery(); return;
    }

    StatementTerminator:

    get_next_token();
    if ((token_type != SEP_TT) || (token_value != SEMICOLON_SEP))
    {   ebf_curtoken_error("';'");
        put_token_back();
    }
}

static void parse_statement_g(int break_label, int continue_label)
{   int ln, ln2, ln3, ln4, flag, onstack;
    int pre_unreach, labelexists;
    assembly_operand AO, AO2, AO3, AO4;
    llvm_direct_value direct_qv1 = NULL, direct_qv2 = NULL;
    debug_location spare_debug_location1, spare_debug_location2;

    ASSERT_GLULX();
    pre_unreach = execution_never_reaches_here;

    if ((token_type == SEP_TT) && (token_value == HASH_SEP))
    {   parse_directive(TRUE);
        parse_statement(break_label, continue_label); return;
    }

    if ((token_type == SEP_TT) && (token_value == AT_SEP))
    {   parse_assembly(); return;
    }

    if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) return;

    if ((token_type == SEP_TT) && (token_value == OPEN_BRACE_SEP))
    {
        llvm_direct_reject("code block");
        put_token_back();
        parse_code_block(break_label, continue_label, 0);
        return;
    }

    if (token_type == DQ_TT)
    {   parse_print_g(TRUE); return;
    }

    if ((token_type == SEP_TT) && (token_value == LESS_SEP))
    {   parse_action(); goto StatementTerminator; }

    if (token_type == EOF_TT)
    {   ebf_curtoken_error("statement"); return; }

    /* If we don't see a keyword, this must be a function call or
       other expression-with-side-effects. */
    if (token_type != STATEMENT_TT)
    {   put_token_back();
        AO = parse_expression(VOID_CONTEXT);
        llvm_direct_expression_statement(AO);
        code_generate(AO, VOID_CONTEXT, -1);
        if (vivc_flag) { panic_mode_error_recovery(); return; }
        goto StatementTerminator;
    }

    statements.enabled = FALSE;
    llvm_direct_note_statement(token_value);

    switch(token_value)
    {

    /*  -------------------------------------------------------------------- */
    /*  box <string-1> ... <string-n> -------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case BOX_CODE:
            INITAOT(&AO3, CONSTANT_OT);
                 AO3.value = begin_table_array();
                 AO3.marker = ARRAY_MV;
                 ln = 0; ln2 = 0;
                 do
                 {   get_next_token();
                     if ((token_type==SEP_TT)&&(token_value==SEMICOLON_SEP))
                         break;
                     if (token_type != DQ_TT)
                         ebf_curtoken_error("text of box line in double-quotes");
                     {   int i, j;
                         for (i=0, j=0; token_text[i] != 0; j++)
                             if (token_text[i] == '@')
                             {   if (token_text[i+1] == '@')
                                 {   i = i + 2;
                                     while (isdigit((uchar)token_text[i])) i++;
                                 }
                                 else
                                 {   i++;
                                     if (token_text[i] != 0) i++;
                                     if (token_text[i] != 0) i++;
                                 }
                             }
                             else i++;
                         if (j > ln2) ln2 = j;
                     }
                     put_token_back();
                     array_entry(ln++, FALSE, parse_expression(CONSTANT_CONTEXT));
                 } while (TRUE);
                 finish_array(ln, FALSE);
                 if (ln == 0)
                     error("No lines of text given for 'box' display");

                 INITAO(&AO2);
                 AO2.value = ln2; set_constant_ot(&AO2);
                 /* The text table above is built by this handler (once,
                    at parse time), so both backends call the veneer on
                    the same array: no ownership conflict. */
                 {   assembly_operand vr = veneer_routine(Box__Routine_VR);
                     llvm_direct_value dargs[2];
                     llvm_direct_value fn = llvm_direct_constant(vr.value,
                         vr.marker, vr.symindex);
                     dargs[0] = llvm_direct_constant(AO2.value, AO2.marker,
                         AO2.symindex);
                     dargs[1] = llvm_direct_constant(AO3.value, AO3.marker,
                         AO3.symindex);
                     if (fn && dargs[0] && dargs[1])
                         (void)llvm_direct_call(fn, dargs, 2);
                 }
                 assembleg_call_2(veneer_routine(Box__Routine_VR),
                     AO2, AO3, zero_operand);
                 return;

    /*  -------------------------------------------------------------------- */
    /*  break -------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case BREAK_CODE:
                 if (break_label == -1)
                 error("'break' can only be used in a loop or 'switch' block");
                 else {
                     llvm_direct_jump(break_label);
                     assembleg_jump(break_label);
                 }
                 break;

    /*  -------------------------------------------------------------------- */
    /*  continue ----------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case CONTINUE_CODE:
                 if (continue_label == -1)
                 error("'continue' can only be used in a loop block");
                 else {
                     llvm_direct_jump(continue_label);
                     assembleg_jump(continue_label);
                 }
                 break;

    /*  -------------------------------------------------------------------- */
    /*  do <codeblock> until (<condition>) --------------------------------- */
    /*  -------------------------------------------------------------------- */

        case DO_CODE:
                 if (pre_unreach) llvm_direct_suspend();
                 ln = alloc_label();
                 llvm_direct_bind_label(ln);
                 assemble_label_no(ln);
                 ln2 = alloc_label(); ln3 = alloc_label();
                 parse_code_block(ln3, ln2, 0);
                 statements.enabled = TRUE;
                 get_next_token();
                 if ((token_type == STATEMENT_TT)
                     && (token_value == UNTIL_CODE))
                 {   labelexists = assemble_forward_label_no(ln2);
                     llvm_direct_resolve_label(ln2, labelexists);
                     match_open_bracket();
                     AO = parse_expression(CONDITION_CONTEXT);
                     match_close_bracket();
                     code_generate(AO, CONDITION_CONTEXT, ln);
                 }
                 else error("'do' without matching 'until'");

                 labelexists = assemble_forward_label_no(ln3);
                 llvm_direct_resolve_label(ln3, labelexists);
                 if (pre_unreach) llvm_direct_resume();
                 break;

    /*  -------------------------------------------------------------------- */
    /*  font on/off -------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case FONT_CODE:
                 misc_keywords.enabled = TRUE;
                 get_next_token();
                 misc_keywords.enabled = FALSE;
                 if ((token_type != MISC_KEYWORD_TT)
                     || ((token_value != ON_MK)
                         && (token_value != OFF_MK)))
                 {   ebf_curtoken_error("'on' or 'off'");
                     panic_mode_error_recovery();
                     break;
                 }

                 /* Call glk_set_style(normal or preformatted) */
                 INITAO(&AO);
                 AO.value = 0x0086;
                 set_constant_ot(&AO);
                 if (token_value == ON_MK)
                   AO2 = zero_operand;
                 else
                   AO2 = two_operand;
                 {   llvm_direct_value fargs[2];
                     fargs[0] = llvm_direct_constant(AO.value, 0, 0);
                     fargs[1] = llvm_direct_constant(AO2.value, 0, 0);
                     direct_operand_call(veneer_routine(Glk__Wrap_VR),
                         fargs, 2);
                 }
                 assembleg_call_2(veneer_routine(Glk__Wrap_VR),
                   AO, AO2, zero_operand);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  for (<initialisation> : <continue-condition> : <updating>) --------- */
    /*  -------------------------------------------------------------------- */

        /*  Note that it's legal for any or all of the three sections of a
            'for' specification to be empty.  This 'for' implementation
            often wastes 3 bytes with a redundant branch rather than keep
            expression parse trees for long periods (as previous versions
            of Inform did, somewhat crudely by simply storing the textual
            form of a 'for' loop).  It is adequate for now.                  */

        case FOR_CODE:
                 if (pre_unreach) llvm_direct_suspend();
                 match_open_bracket();
                 get_next_token();

                 /*  Initialisation code  */
                 AO.type = OMITTED_OT;
                 spare_debug_location1 = statement_debug_location;
                 AO2.type = OMITTED_OT; flag = 0;
                 spare_debug_location2 = statement_debug_location;

                 if (!((token_type==SEP_TT)&&(token_value==COLON_SEP)))
                 {   put_token_back();
                     if (!((token_type==SEP_TT)&&(token_value==SUPERCLASS_SEP)))
                         {   sequence_point_follows = TRUE;
                             statement_debug_location = get_token_location();
                             AO3 = parse_expression(FORINIT_CONTEXT);
                             llvm_direct_expression_statement(AO3);
                             code_generate(AO3, VOID_CONTEXT, -1);
                     }
                     get_next_token();
                     if ((token_type==SEP_TT)&&(token_value == SUPERCLASS_SEP))
                     {   get_next_token();
                         if ((token_type==SEP_TT)&&(token_value == CLOSEB_SEP))
                         {   ln = alloc_label();
                             llvm_direct_bind_label(ln);
                             assemble_label_no(ln);
                             ln2 = alloc_label();
                             parse_code_block(ln2, ln, 0);
                             sequence_point_follows = FALSE;
                             if (!execution_never_reaches_here) {
                                 llvm_direct_jump(ln);
                                 assembleg_jump(ln);
                             }
                             labelexists = assemble_forward_label_no(ln2);
                             llvm_direct_resolve_label(ln2, labelexists);
                             if (pre_unreach) llvm_direct_resume();
                             return;
                         }
                         goto ParseUpdate;
                     }
                     put_token_back();
                     if (!match_colon()) break;
                 }

                 get_next_token();
                 if (!((token_type==SEP_TT)&&(token_value==COLON_SEP)))
                 {   put_token_back();
                     spare_debug_location1 = get_token_location();
                     AO = parse_expression(CONDITION_CONTEXT);
                     if (!match_colon()) break;
                 }
                 get_next_token();

                 ParseUpdate:
                 if (!((token_type==SEP_TT)&&(token_value==CLOSEB_SEP)))
                 {   put_token_back();
                     spare_debug_location2 = get_token_location();
                     AO2 = parse_expression(VOID_CONTEXT);
                     match_close_bracket();
                     flag = test_for_incdec(AO2);
                 }

                 ln = alloc_label();
                 ln2 = alloc_label();
                 ln3 = alloc_label();

                  if ((AO2.type == OMITTED_OT) || (flag != 0))
                  {
                      llvm_direct_bind_label(ln);
                      assemble_label_no(ln);
                      if (flag==0) {
                          llvm_direct_bind_label(ln2);
                          assemble_label_no(ln2);
                      }

                     /*  The "finished yet?" condition  */

                     if (AO.type != OMITTED_OT)
                     {   sequence_point_follows = TRUE;
                         statement_debug_location = spare_debug_location1;
                         code_generate(AO, CONDITION_CONTEXT, ln3);
                     }

                 }
                 else
                 {
                     /*  This is the jump which could be avoided with the aid
                         of long-term expression storage  */

                      sequence_point_follows = FALSE;
                      llvm_direct_jump(ln2);
                      assembleg_jump(ln2);

                     /*  The "update" part  */

                      llvm_direct_bind_label(ln);
                      assemble_label_no(ln);
                      sequence_point_follows = TRUE;
                      statement_debug_location = spare_debug_location2;
                      llvm_direct_expression_statement(AO2);
                      code_generate(AO2, VOID_CONTEXT, -1);

                      llvm_direct_bind_label(ln2);
                      assemble_label_no(ln2);

                     /*  The "finished yet?" condition  */

                     if (AO.type != OMITTED_OT)
                     {   sequence_point_follows = TRUE;
                         statement_debug_location = spare_debug_location1;
                         code_generate(AO, CONDITION_CONTEXT, ln3);
                     }
                 }

                 if (flag != 0)
                 {
                     /*  In this optimised case, update code is at the end
                         of the loop block, so "continue" goes there  */

                      parse_code_block(ln3, ln2, 0);
                      llvm_direct_bind_label(ln2);
                      assemble_label_no(ln2);

                     sequence_point_follows = TRUE;
                     statement_debug_location = spare_debug_location2;
                      if (flag > 0)
                     {   INITAO(&AO3);
                         AO3.value = flag;
                         if (AO3.value >= MAX_LOCAL_VARIABLES)
                           AO3.type = GLOBALVAR_OT;
                         else
                           AO3.type = LOCALVAR_OT;
                         assembleg_3(add_gc, AO3, one_operand, AO3);
                     }
                     else
                     {   INITAO(&AO3);
                         AO3.value = -flag;
                         if (AO3.value >= MAX_LOCAL_VARIABLES)
                           AO3.type = GLOBALVAR_OT;
                         else
                           AO3.type = LOCALVAR_OT;
                         assembleg_3(sub_gc, AO3, one_operand, AO3);
                      }
                      llvm_direct_adjust_variable(flag > 0 ? flag : -flag,
                          flag > 0 ? 1 : -1);
                      llvm_direct_jump(ln);
                      assembleg_jump(ln);
                 }
                 else
                 {
                     /*  In the unoptimised case, update code is at the
                         start of the loop block, so "continue" goes there  */

                      parse_code_block(ln3, ln, 0);
                      if (!execution_never_reaches_here)
                      {   sequence_point_follows = FALSE;
                          llvm_direct_jump(ln);
                          assembleg_jump(ln);
                      }
                  }

                  labelexists = assemble_forward_label_no(ln3);
                  llvm_direct_resolve_label(ln3, labelexists);
                  if (pre_unreach) llvm_direct_resume();
                 return;

    /*  -------------------------------------------------------------------- */
    /*  give <expression> [~]attr [, [~]attr [, ...]] ---------------------- */
    /*  -------------------------------------------------------------------- */

        case GIVE_CODE:
                 {   assembly_operand gtree =
                         parse_expression(QUANTITY_CONTEXT);
                     direct_qv1 = llvm_direct_quantity(gtree);
                     AO = code_generate(gtree, QUANTITY_CONTEXT, -1);
                 }
                 check_warn_symbol_type(&AO, OBJECT_T, 0, "\"give\" statement");
                 if ((AO.type == LOCALVAR_OT) && (AO.value == 0))
                     onstack = TRUE;
                 else
                     onstack = FALSE;

                 do
                 {   get_next_token();
                     if ((token_type == SEP_TT) 
                       && (token_value == SEMICOLON_SEP)) {
                         if (onstack) {
                           assembleg_2(copy_gc, stack_pointer, zero_operand);
                         }
                         return;
                     }
                     if ((token_type == SEP_TT)&&(token_value == ARTNOT_SEP))
                         ln = 0;
                     else
                     {   ln = 1;
                         put_token_back();
                     }
                     {   assembly_operand atree =
                             parse_expression(QUANTITY_CONTEXT);
                         direct_qv2 = llvm_direct_quantity(atree);
                         AO2 = code_generate(atree, QUANTITY_CONTEXT, -1);
                     }
                     check_warn_symbol_type(&AO2, ATTRIBUTE_T, 0, "\"give\" statement");
                     if (runtime_error_checking_switch && (!veneer_mode))
                     {   llvm_direct_value gargs[2];
                         gargs[0] = direct_qv1;
                         gargs[1] = direct_qv2;
                         ln2 = (ln ? RT__ChG_VR : RT__ChGt_VR);
                         direct_operand_call(veneer_routine(ln2), gargs, 2);
                         if ((AO2.type == LOCALVAR_OT) && (AO2.value == 0)) {
                           /* already on stack */
                         }
                         else {
                           assembleg_store(stack_pointer, AO2);
                         }
                         if (onstack)
                           assembleg_2(stkpeek_gc, one_operand, stack_pointer);
                         else
                           assembleg_store(stack_pointer, AO);
                         assembleg_3(call_gc, veneer_routine(ln2), two_operand,
                           zero_operand);
                     }
                     else {
                         {   llvm_direct_value bargs[3];
                             if (is_constant_ot(AO2.type) && AO2.marker == 0)
                                 bargs[1] = llvm_direct_constant(
                                     AO2.value + 8, 0, 0);
                             else
                                 bargs[1] = llvm_direct_binary(PLUS_OP,
                                     direct_qv2,
                                     llvm_direct_constant(8, 0, 0));
                             bargs[0] = direct_qv1;
                             bargs[2] = llvm_direct_constant(ln, 0, 0);
                             if (bargs[0] && bargs[1] && bargs[2])
                                 (void)llvm_direct_glulx_op("astorebit",
                                     bargs, 3);
                         }
                         if (is_constant_ot(AO2.type) && AO2.marker == 0) {
                           AO2.value += 8;
                           set_constant_ot(&AO2);
                         }
                         else {
                           INITAOTV(&AO3, BYTECONSTANT_OT, 8);
                           assembleg_3(add_gc, AO2, AO3, stack_pointer);
                           AO2 = stack_pointer;
                         }
                         if (onstack) {
                           if ((AO2.type == LOCALVAR_OT) && (AO2.value == 0))
                             assembleg_2(stkpeek_gc, one_operand, 
                               stack_pointer);
                           else
                             assembleg_2(stkpeek_gc, zero_operand, 
                               stack_pointer);
                         }
                         if (ln) 
                           AO3 = one_operand;
                         else
                           AO3 = zero_operand;
                         assembleg_3(astorebit_gc, AO, AO2, AO3);
                     }
                 } while(TRUE);

    /*  -------------------------------------------------------------------- */
    /*  if (<condition>) <codeblock> [else <codeblock>] -------------------- */
    /*  -------------------------------------------------------------------- */

        case IF_CODE:
                 if (pre_unreach) llvm_direct_suspend();
                 flag = FALSE; /* set if there's an "else" */
                 ln2 = 0;
                 pre_unreach = execution_never_reaches_here;

                 match_open_bracket();
                 AO = parse_expression(CONDITION_CONTEXT);
                 match_close_bracket();

                 statements.enabled = TRUE;
                 get_next_token();
                 if ((token_type == STATEMENT_TT)&&(token_value == RTRUE_CODE))
                     ln = -4;
                 else
                 if ((token_type == STATEMENT_TT)&&(token_value == RFALSE_CODE))
                     ln = -3;
                 else
                 {   put_token_back();
                     ln = alloc_label();
                 }

                 /* The condition */
                 code_generate(AO, CONDITION_CONTEXT, ln);

                 if (!pre_unreach && ln >= 0 && execution_never_reaches_here) {
                     /* If the condition never falls through to here, then
                        it was an "if (0)" test. Our convention is to skip
                        the "not reached" warnings for this case. */
                     execution_never_reaches_here |= EXECSTATE_NOWARN;
                 }

                 /* The "if" block */
                 if (ln >= 0) parse_code_block(break_label, continue_label, 0);
                 else
                 {   get_next_token();
                     if ((token_type != SEP_TT)
                         || (token_value != SEMICOLON_SEP))
                     {   ebf_curtoken_error("';'");
                         put_token_back();
                     }
                 }

                 statements.enabled = TRUE;
                 get_next_token();
                 
                 /* An #if directive around the ELSE clause is legal. */
                 while ((token_type == SEP_TT) && (token_value == HASH_SEP))
                 {   parse_directive(TRUE);
                     statements.enabled = TRUE;
                     get_next_token();
                 }
                 
                 if ((token_type == STATEMENT_TT) && (token_value == ELSE_CODE))
                 {   flag = TRUE;
                      if (ln >= 0)
                      {   ln2 = alloc_label();
                          if (!execution_never_reaches_here)
                          {   sequence_point_follows = FALSE;
                              llvm_direct_jump(ln2);
                              assembleg_jump(ln2);
                          }
                     }
                 }
                 else put_token_back();

                 /* The "else" label (or end of statement, if there is no "else") */
                   labelexists = FALSE;
                   if (ln >= 0) {
                       labelexists = assemble_forward_label_no(ln);
                       if (!pre_unreach)
                           llvm_direct_resolve_label(ln, labelexists);
                  }

                 if (flag)
                 {
                     /* If labelexists is false, then we started with
                        "if (1)". In this case, we don't want a "not
                        reached" warning on the "else" block. We
                        temporarily disable the NOWARN flag, and restore it
                        afterwards. */
                     int saved_unreach = 0;
                     if (execution_never_reaches_here && !labelexists) {
                         saved_unreach = execution_never_reaches_here;
                         execution_never_reaches_here |= EXECSTATE_NOWARN;
                     }

                     /* The "else" block */
                     parse_code_block(break_label, continue_label, 0);

                     if (execution_never_reaches_here && !labelexists) {
                         if (saved_unreach & EXECSTATE_NOWARN)
                             execution_never_reaches_here |= EXECSTATE_NOWARN;
                         else
                             execution_never_reaches_here &= ~EXECSTATE_NOWARN;
                     }

                     /* The post-"else" label */
                       if (ln >= 0) {
                           labelexists = assemble_forward_label_no(ln2);
                           if (!pre_unreach)
                               llvm_direct_resolve_label(ln2, labelexists);
                      }
                 }
                 else
                 {
                     /* There was no "else". If we're unreachable, then the
                        statement returned unconditionally, which means 
                        "if (1) return". Skip warnings. */
                     if (!pre_unreach && execution_never_reaches_here) {
                         execution_never_reaches_here |= EXECSTATE_NOWARN;
                     }
                 }

                 if (pre_unreach) llvm_direct_resume();
                 return;

    /*  -------------------------------------------------------------------- */
    /*  inversion ---------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case INVERSION_CODE:
                 /* Direct form: read and stream the four version bytes
                    from the header, as classic's copyb pairs do. */
                 if (llvm_direct_can_generate()) {
                     int k;
                     for (k = 8; k <= 11; k++) {
                         llvm_direct_value byte, bargs[2];
                         bargs[0] = llvm_direct_constant(
                             GLULX_HEADER_SIZE+k, 0, 0);
                         bargs[1] = llvm_direct_constant(0, 0, 0);
                         byte = llvm_direct_glulx_op("aloadb", bargs, 2);
                         if (byte)
                             (void)llvm_direct_glulx_op("streamchar",
                                 &byte, 1);
                     }
                 }
                 INITAOTV(&AO2, DEREFERENCE_OT, GLULX_HEADER_SIZE+8);
                 assembleg_2(copyb_gc, AO2, stack_pointer);
                 assembleg_1(streamchar_gc, stack_pointer);
                 AO2.value  = GLULX_HEADER_SIZE+9; 
                 assembleg_2(copyb_gc, AO2, stack_pointer);
                 assembleg_1(streamchar_gc, stack_pointer);
                 AO2.value  = GLULX_HEADER_SIZE+10; 
                 assembleg_2(copyb_gc, AO2, stack_pointer);
                 assembleg_1(streamchar_gc, stack_pointer);
                 AO2.value  = GLULX_HEADER_SIZE+11; 
                 assembleg_2(copyb_gc, AO2, stack_pointer);
                 assembleg_1(streamchar_gc, stack_pointer);

                 if (/* DISABLES CODE */ (0)) {
                     INITAO(&AO);
                     AO.value = '(';
                     set_constant_ot(&AO);
                     assembleg_1(streamchar_gc, AO);
                     AO.value = 'G';
                     set_constant_ot(&AO);
                     assembleg_1(streamchar_gc, AO);

                     AO2.value  = GLULX_HEADER_SIZE+12; 
                     assembleg_2(copyb_gc, AO2, stack_pointer);
                     assembleg_1(streamchar_gc, stack_pointer);
                     AO2.value  = GLULX_HEADER_SIZE+13; 
                     assembleg_2(copyb_gc, AO2, stack_pointer);
                     assembleg_1(streamchar_gc, stack_pointer);
                     AO2.value  = GLULX_HEADER_SIZE+14; 
                     assembleg_2(copyb_gc, AO2, stack_pointer);
                     assembleg_1(streamchar_gc, stack_pointer);
                     AO2.value  = GLULX_HEADER_SIZE+15; 
                     assembleg_2(copyb_gc, AO2, stack_pointer);
                     assembleg_1(streamchar_gc, stack_pointer);

                     AO.marker = 0;
                     AO.value = ')';
                     set_constant_ot(&AO);
                     assembleg_1(streamchar_gc, AO);
                 }

                 break;

    /*  -------------------------------------------------------------------- */
    /*  jump <label> ------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case JUMP_CODE:
                 ln = parse_label();
                 llvm_direct_jump(ln);
                 assembleg_jump(ln);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  move <expression> to <expression> ---------------------------------- */
    /*  -------------------------------------------------------------------- */

        case MOVE_CODE:
                 misc_keywords.enabled = TRUE;
                 AO = parse_expression(QUANTITY_CONTEXT);

                 get_next_token();
                 misc_keywords.enabled = FALSE;
                 if ((token_type != MISC_KEYWORD_TT)
                     || (token_value != TO_MK))
                 {   ebf_curtoken_error("'to'");
                     panic_mode_error_recovery();
                     return;
                 }

                 {   assembly_operand dtree =
                         parse_expression(QUANTITY_CONTEXT);
                     llvm_direct_value margs[2];
                     direct_qv2 = llvm_direct_quantity(dtree);
                     AO2 = code_generate(dtree, QUANTITY_CONTEXT, -1);
                     direct_qv1 = llvm_direct_quantity(AO);
                     AO = code_generate(AO, QUANTITY_CONTEXT, -1);
                     margs[0] = direct_qv1;
                     margs[1] = direct_qv2;
                     direct_operand_call(veneer_routine(
                         (runtime_error_checking_switch && !veneer_mode)
                             ? RT__ChT_VR : OB__Move_VR), margs, 2);
                 }
                 check_warn_symbol_type(&AO, OBJECT_T, 0, "\"move\" statement");
                 check_warn_symbol_type(&AO2, OBJECT_T, CLASS_T, "\"move\" statement");
                 if ((runtime_error_checking_switch) && (veneer_mode == FALSE))
                     assembleg_call_2(veneer_routine(RT__ChT_VR), AO, AO2,
                         zero_operand);
                 else
                     assembleg_call_2(veneer_routine(OB__Move_VR), AO, AO2,
                         zero_operand);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  new_line ----------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case NEW_LINE_CODE:
              INITAOTV(&AO, BYTECONSTANT_OT, 0x0A);
              direct_stream_char(0x0A);
              assembleg_1(streamchar_gc, AO);
              break;

    /*  -------------------------------------------------------------------- */
    /*  objectloop (<initialisation>) <codeblock> -------------------------- */
    /*  -------------------------------------------------------------------- */

        case OBJECTLOOP_CODE:

                 match_open_bracket();
                 get_next_token();
                 if (token_type == LOCAL_VARIABLE_TT) {
                     INITAOTV(&AO, LOCALVAR_OT, token_value);
                 }
                 else if ((token_type == SYMBOL_TT) &&
                   (symbols[token_value].type == GLOBAL_VARIABLE_T)) {
                     INITAOTV(&AO, GLOBALVAR_OT, symbols[token_value].value);
                 }
                 else {
                     ebf_curtoken_error("'objectloop' variable");
                     panic_mode_error_recovery(); 
                     break;
                 }
                 misc_keywords.enabled = TRUE;
                 get_next_token(); flag = TRUE;
                 misc_keywords.enabled = FALSE;
                 if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP))
                     flag = FALSE;

                 ln = 0;
                 if ((token_type == MISC_KEYWORD_TT)
                     && (token_value == NEAR_MK)) ln = 1;
                 if ((token_type == MISC_KEYWORD_TT)
                     && (token_value == FROM_MK)) ln = 2;
                 if ((token_type == CND_TT) && (token_value == IN_COND))
                 {   get_next_token();
                     get_next_token();
                     if ((token_type == SEP_TT) && (token_value == CLOSEB_SEP))
                         ln = 3;
                     put_token_back();
                     put_token_back();
                 }

                 if (ln != 0) {
                   /*  Old style (Inform 5) objectloops: note that we
                       implement objectloop (a in b) in the old way since
                       this runs through objects in a different order from
                       the new way, and there may be existing Inform code
                       relying on this.                                    */
                     assembly_operand AO4, AO5, domain_tree;
                     llvm_direct_value dv, cursor;
                     INITAO(&AO5);

                     sequence_point_follows = TRUE;
                     domain_tree = parse_expression(QUANTITY_CONTEXT);
                     dv = llvm_direct_quantity(domain_tree);
                     AO2 = code_generate(domain_tree,
                         QUANTITY_CONTEXT, -1);
                     match_close_bracket();
                     if (ln == 1) {
                         if (runtime_error_checking_switch) {
                             dv = llvm_direct_check_object_operand(dv,
                                 domain_tree, OBJECTLOOP_RTE);
                             AO2 = check_nonzero_at_runtime(AO2, -1,
                                 OBJECTLOOP_RTE);
                         }
                         dv = direct_object_field(dv, GOBJFIELD_PARENT());
                         dv = direct_object_field(dv, GOBJFIELD_CHILD());
                         INITAOTV(&AO4, BYTECONSTANT_OT, GOBJFIELD_PARENT());
                         assembleg_3(aload_gc, AO2, AO4, stack_pointer);
                         INITAOTV(&AO4, BYTECONSTANT_OT, GOBJFIELD_CHILD());
                         assembleg_3(aload_gc, stack_pointer, AO4, stack_pointer);
                         AO2 = stack_pointer;
                     }
                     else if (ln == 3) {
                         if (runtime_error_checking_switch) {
                             AO5 = AO2;
                             dv = llvm_direct_check_object_operand(dv,
                                 domain_tree, CHILD_RTE);
                             AO2 = check_nonzero_at_runtime(AO2, -1,
                                 CHILD_RTE);
                         }
                         dv = direct_object_field(dv, GOBJFIELD_CHILD());
                         INITAOTV(&AO4, BYTECONSTANT_OT, GOBJFIELD_CHILD());
                         assembleg_3(aload_gc, AO2, AO4, stack_pointer);
                         AO2 = stack_pointer;
                     }
                     else {
                         /* do nothing */
                     }
                     direct_variable_store(AO, dv);
                     assembleg_store(AO, AO2);
                     ln2 = alloc_label();
                     if (dv) {
                         llvm_direct_block enter = llvm_direct_new_block();
                         llvm_direct_branch(dv, enter,
                             llvm_direct_source_block(ln2));
                         llvm_direct_bind_block(enter);
                     }
                     assembleg_1_branch(jz_gc, AO, ln2);
                     ln4 = alloc_label();
                     llvm_direct_bind_label(ln4);
                     assemble_label_no(ln4);
                     parse_code_block(ln2, ln3 = alloc_label(), 0);
                     sequence_point_follows = FALSE;
                     llvm_direct_bind_label(ln3);
                     assemble_label_no(ln3);
                     cursor = direct_variable_value(AO);
                     if (runtime_error_checking_switch) {
                         llvm_direct_check_object_branch(cursor,
                             OBJECTLOOP2_RTE, ln2);
                         AO2 = check_nonzero_at_runtime(AO, ln2,
                              OBJECTLOOP2_RTE);
                         if ((ln == 3)
                             && ((AO5.type != LOCALVAR_OT)||(AO5.value != 0))
                             && ((AO5.type != LOCALVAR_OT)||(AO5.value != AO.value)))
                         {   int label = alloc_label();
                             assembly_operand en_ao;
                             INITAO(&en_ao);
                             en_ao.value = OBJECTLOOP_BROKEN_RTE;
                             set_constant_ot(&en_ao);
                             if (llvm_direct_can_generate()) {
                                 /* AO5 is re-read each iteration, exactly
                                    as classic's jeq does. */
                                 llvm_direct_value par, cmp, eargs[2];
                                 llvm_direct_block intact =
                                     llvm_direct_new_block();
                                 llvm_direct_block broken =
                                     llvm_direct_new_block();
                                 par = direct_object_field(cursor,
                                     GOBJFIELD_PARENT());
                                 cmp = llvm_direct_compare(CONDEQUALS_OP,
                                     par, llvm_direct_quantity(AO5));
                                 llvm_direct_branch(cmp, intact, broken);
                                 llvm_direct_bind_block(broken);
                                 eargs[0] = llvm_direct_constant(
                                     OBJECTLOOP_BROKEN_RTE, 0, 0);
                                 eargs[1] = cursor;
                                 direct_operand_call(
                                     veneer_routine(RT__Err_VR), eargs, 2);
                                 llvm_direct_jump(ln2);
                                 llvm_direct_bind_block(intact);
                             }
                             INITAOTV(&AO4, BYTECONSTANT_OT, GOBJFIELD_PARENT());
                             assembleg_3(aload_gc, AO, AO4, stack_pointer);
                             assembleg_2_branch(jeq_gc, stack_pointer, AO5,
                                 label);
                             assembleg_call_2(veneer_routine(RT__Err_VR),
                                 en_ao, AO, zero_operand);
                             assembleg_jump(ln2);
                             assemble_label_no(label);
                         }
                     }
                     else {
                         AO2 = AO;
                     }
                     cursor = direct_object_field(cursor,
                         GOBJFIELD_SIBLING());
                     direct_variable_store(AO, cursor);
                     if (cursor)
                         llvm_direct_branch(cursor,
                             llvm_direct_source_block(ln4),
                             llvm_direct_source_block(ln2));
                     INITAOTV(&AO4, BYTECONSTANT_OT, GOBJFIELD_SIBLING());
                     assembleg_3(aload_gc, AO2, AO4, AO);
                     assembleg_1_branch(jnz_gc, AO, ln4);
                     llvm_direct_bind_label(ln2);
                     assemble_label_no(ln2);
                     return;
                 }

                 sequence_point_follows = TRUE;
                 ln = get_symbol_index("Class");
                 if (ln < 0) {
                     error("No 'Class' object found");
                     AO2 = zero_operand;
                 }
                 else {
                     INITAOT(&AO2, CONSTANT_OT);
                     AO2.value = symbols[ln].value;
                     AO2.marker = OBJECT_MV;
                 }
                 direct_variable_store(AO, llvm_direct_constant(AO2.value,
                     AO2.marker, AO2.symindex));
                 assembleg_store(AO, AO2);

                 ln = alloc_label();
                 llvm_direct_bind_label(ln);
                 assemble_label_no(ln);
                 ln2 = alloc_label();
                 ln3 = alloc_label();
                 if (flag)
                 {   put_token_back();
                     put_token_back();
                     sequence_point_follows = TRUE;
                     code_generate(parse_expression(CONDITION_CONTEXT),
                         CONDITION_CONTEXT, ln3);
                     match_close_bracket();
                 }
                 parse_code_block(ln2, ln3, 0);

                 sequence_point_follows = FALSE;
                 llvm_direct_bind_label(ln3);
                 assemble_label_no(ln3);
                 {   llvm_direct_value next_obj = direct_object_field(
                         direct_variable_value(AO), GOBJFIELD_CHAIN());
                     direct_variable_store(AO, next_obj);
                     if (next_obj)
                         llvm_direct_branch(next_obj,
                             llvm_direct_source_block(ln),
                             llvm_direct_source_block(ln2));
                 }
                 INITAOTV(&AO4, BYTECONSTANT_OT, GOBJFIELD_CHAIN());
                 assembleg_3(aload_gc, AO, AO4, AO);
                 assembleg_1_branch(jnz_gc, AO, ln);
                 llvm_direct_bind_label(ln2);
                 assemble_label_no(ln2);
                 return;

    /*  -------------------------------------------------------------------- */
    /*  (see routine above) ------------------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case PRINT_CODE:
            get_next_token();
            parse_print_g(FALSE); return;
        case PRINT_RET_CODE:
            get_next_token();
            parse_print_g(TRUE); return;

    /*  -------------------------------------------------------------------- */
    /*  quit --------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case QUIT_CODE:
                 (void)llvm_direct_glulx_op("quit", NULL, 0);
                 assembleg_0(quit_gc); break;

    /*  -------------------------------------------------------------------- */
    /*  remove <expression> ------------------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case REMOVE_CODE:
            {   assembly_operand rtree = parse_expression(QUANTITY_CONTEXT);
                direct_qv1 = llvm_direct_quantity(rtree);
                AO = code_generate(rtree, QUANTITY_CONTEXT, -1);
                direct_operand_call(veneer_routine(
                    (runtime_error_checking_switch && !veneer_mode)
                        ? RT__ChR_VR : OB__Remove_VR), &direct_qv1, 1);
            }
            check_warn_symbol_type(&AO, OBJECT_T, 0, "\"remove\" statement");
            if ((runtime_error_checking_switch) && (veneer_mode == FALSE))
                assembleg_call_1(veneer_routine(RT__ChR_VR), AO,
                    zero_operand);
            else
                assembleg_call_1(veneer_routine(OB__Remove_VR), AO,
                    zero_operand);
            break;

    /*  -------------------------------------------------------------------- */
    /*  return [<expression>] ---------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RETURN_CODE:
            get_next_token();
            if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) {
                llvm_direct_return_constant(1);
                assembleg_1(return_gc, one_operand); 
                return; 
            }
            put_token_back();
            AO = parse_expression(RETURN_Q_CONTEXT);
            llvm_direct_return_expression(AO);
            AO = code_generate(AO, QUANTITY_CONTEXT, -1);
            assembleg_1(return_gc, AO);
            break;
            
    /*  -------------------------------------------------------------------- */
    /*  rfalse ------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RFALSE_CODE:   
            llvm_direct_return_constant(0);
            assembleg_1(return_gc, zero_operand); 
            break;

    /*  -------------------------------------------------------------------- */
    /*  rtrue -------------------------------------------------------------- */
    /*  -------------------------------------------------------------------- */

        case RTRUE_CODE:   
            llvm_direct_return_constant(1);
            assembleg_1(return_gc, one_operand); 
            break;

    /*  -------------------------------------------------------------------- */
    /*  spaces <expression> ------------------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case SPACES_CODE:
                 {   assembly_operand stree =
                         parse_expression(QUANTITY_CONTEXT);
                     direct_qv1 = llvm_direct_quantity(stree);
                     AO = code_generate(stree, QUANTITY_CONTEXT, -1);
                 }

                 /* Direct form: a countdown phi loop printing spaces. */
                 if (direct_qv1) {
                     llvm_direct_value counter, space, decremented;
                     llvm_direct_block entry_from, loop, loop_from, done;
                     llvm_direct_value cmp = llvm_direct_compare(LESS_OP,
                         direct_qv1, llvm_direct_constant(1, 0, 0));
                     entry_from = llvm_direct_current_block();
                     loop = llvm_direct_new_block();
                     done = llvm_direct_new_block();
                     llvm_direct_branch(cmp, done, loop);
                     llvm_direct_bind_block(loop);
                     counter = llvm_direct_phi_empty();
                     llvm_direct_phi_add(counter, direct_qv1, entry_from);
                     space = llvm_direct_constant(32, 0, 0);
                     (void)llvm_direct_glulx_op("streamchar", &space, 1);
                     decremented = llvm_direct_binary(MINUS_OP, counter,
                         llvm_direct_constant(1, 0, 0));
                     loop_from = llvm_direct_current_block();
                     llvm_direct_branch(decremented, loop, done);
                     llvm_direct_phi_add(counter, decremented, loop_from);
                     llvm_direct_bind_block(done);
                 }

                 assembleg_store(temp_var1, AO);

                 INITAO(&AO);
                 AO.value = 32; set_constant_ot(&AO);

                 assembleg_2_branch(jlt_gc, temp_var1, one_operand,
                     ln = alloc_label());
                 assemble_label_no(ln2 = alloc_label());
                 assembleg_1(streamchar_gc, AO);
                 assembleg_dec(temp_var1);
                 assembleg_1_branch(jnz_gc, temp_var1, ln2);
                 assemble_label_no(ln);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  string <expression> <literal-string> ------------------------------- */
    /*  -------------------------------------------------------------------- */

        case STRING_CODE:
                 {   assembly_operand stree =
                         parse_expression(QUANTITY_CONTEXT);
                     direct_qv1 = llvm_direct_quantity(stree);
                     AO2 = code_generate(stree, QUANTITY_CONTEXT, -1);
                 }
                 if (is_constant_ot(AO2.type) && AO2.marker == 0) {
                     /* Compile-time check */
                     if (AO2.value < 0 || AO2.value >= MAX_DYNAMIC_STRINGS) {
                         error_max_dynamic_strings(AO2.value);
                     }
                 }
                 get_next_token();
                 if (token_type == DQ_TT)
                 {   INITAOT(&AO4, CONSTANT_OT);
                     /* This is not actually placed in low memory; Glulx
                        has no such concept. We use the LOWSTRING flag
                        for compatibility with older compiler behavior. */
                     AO4.value = compile_string(token_text, STRCTX_LOWSTRING);
                     AO4.marker = STRING_MV;
                 }
                 else
                 {   put_token_back();
                     AO4 = parse_expression(CONSTANT_CONTEXT);
                 }
                 {   llvm_direct_value dsargs[2];
                     dsargs[0] = direct_qv1;
                     dsargs[1] = llvm_direct_constant(AO4.value, AO4.marker,
                         AO4.symindex);
                     direct_operand_call(veneer_routine(Dynam__String_VR),
                         dsargs, 2);
                 }
                 assembleg_call_2(veneer_routine(Dynam__String_VR),
                   AO2, AO4, zero_operand);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  style roman/reverse/bold/underline/fixed --------------------------- */
    /*  -------------------------------------------------------------------- */

        case STYLE_CODE:
                 misc_keywords.enabled = TRUE;
                 get_next_token();
                 misc_keywords.enabled = FALSE;
                 if ((token_type != MISC_KEYWORD_TT)
                     || ((token_value != ROMAN_MK)
                         && (token_value != REVERSE_MK)
                         && (token_value != BOLD_MK)
                         && (token_value != UNDERLINE_MK)
                         && (token_value != FIXED_MK)))
                 {   ebf_curtoken_error(
"'roman', 'bold', 'underline', 'reverse' or 'fixed'");
                     panic_mode_error_recovery();
                     break;
                 }

                 /* Call glk_set_style() */

                 INITAO(&AO);
                 AO.value = 0x0086;
                 set_constant_ot(&AO);
                 switch(token_value)
                 {   case ROMAN_MK:
                     default: 
                         AO2 = zero_operand; /* normal */
                         break;
                     case REVERSE_MK: 
                         INITAO(&AO2);
                         AO2.value = 5; /* alert */
                         set_constant_ot(&AO2);
                         break;
                     case BOLD_MK: 
                         INITAO(&AO2);
                         AO2.value = 4; /* subheader */
                         set_constant_ot(&AO2);
                         break;
                     case UNDERLINE_MK: 
                         AO2 = one_operand; /* emphasized */
                         break;
                     case FIXED_MK:
                         AO2 = two_operand; /* preformatted */
                         break;
                 }
                 {   llvm_direct_value sargs[2];
                     sargs[0] = llvm_direct_constant(AO.value, 0, 0);
                     sargs[1] = llvm_direct_constant(AO2.value, 0, 0);
                     direct_operand_call(veneer_routine(Glk__Wrap_VR),
                         sargs, 2);
                 }
                 assembleg_call_2(veneer_routine(Glk__Wrap_VR),
                   AO, AO2, zero_operand);
                 break;

    /*  -------------------------------------------------------------------- */
    /*  switch (<expression>) <codeblock> ---------------------------------- */
    /*  -------------------------------------------------------------------- */

        case SWITCH_CODE:
                 if (pre_unreach) llvm_direct_suspend();
                 match_open_bracket();
                 AO = parse_expression(QUANTITY_CONTEXT);
                 llvm_direct_switch_begin(AO);
                 AO = code_generate(AO, QUANTITY_CONTEXT, -1);
                 match_close_bracket();

                 assembleg_store(temp_var1, AO); 

                 parse_code_block(ln = alloc_label(), continue_label, 1);
                 llvm_direct_switch_end();
                 labelexists = assemble_forward_label_no(ln);
                 llvm_direct_resolve_label(ln, labelexists);
                 if (pre_unreach) llvm_direct_resume();
                 return;

    /*  -------------------------------------------------------------------- */
    /*  while (<condition>) <codeblock> ------------------------------------ */
    /*  -------------------------------------------------------------------- */

        case WHILE_CODE:
                 if (pre_unreach) llvm_direct_suspend();
                 ln = alloc_label();
                 llvm_direct_bind_label(ln);
                 assemble_label_no(ln);
                 match_open_bracket();

                 code_generate(parse_expression(CONDITION_CONTEXT),
                     CONDITION_CONTEXT, ln2 = alloc_label());
                 match_close_bracket();

                 parse_code_block(ln2, ln, 0);
                 sequence_point_follows = FALSE;
                 if (!execution_never_reaches_here) {
                     llvm_direct_jump(ln);
                     assembleg_jump(ln);
                 }
                 labelexists = assemble_forward_label_no(ln2);
                 llvm_direct_resolve_label(ln2, labelexists);
                 if (pre_unreach) llvm_direct_resume();
                 return;

    /*  -------------------------------------------------------------------- */

        case SDEFAULT_CODE:
                 error("'default' without matching 'switch'"); break;
        case ELSE_CODE:
                 error("'else' without matching 'if'"); break;
        case UNTIL_CODE:
                 error("'until' without matching 'do'");
                 panic_mode_error_recovery(); return;

    /*  -------------------------------------------------------------------- */

    /* And a useful default, which will never be triggered in a complete
       Inform compiler, but which is important in development. */

        default:
            error("*** Statement code gen: Can't generate yet ***\n");
            panic_mode_error_recovery(); return;
    }

    StatementTerminator:

    get_next_token();
    if ((token_type != SEP_TT) || (token_value != SEMICOLON_SEP))
    {   ebf_curtoken_error("';'");
        put_token_back();
    }
}

extern void parse_statement(int break_label, int continue_label)
{
    int res;
    int saved_entire_flag;
    
    res = parse_named_label_statements();
    if (!res)
        return;

    saved_entire_flag = (execution_never_reaches_here & EXECSTATE_ENTIRE);
    if (execution_never_reaches_here)
        execution_never_reaches_here |= EXECSTATE_ENTIRE;
 
    if (!glulx_mode)
        parse_statement_z(break_label, continue_label);
    else
        parse_statement_g(break_label, continue_label);

    if (saved_entire_flag)
        execution_never_reaches_here |= EXECSTATE_ENTIRE;
    else
        execution_never_reaches_here &= ~EXECSTATE_ENTIRE;
}

/* This does the same work as parse_statement(), but it's called if you've
   already parsed an expression (in void context) and you want to generate
   it as a statement. Essentially it's a copy of parse_statement() and
   parse_statement_z/g(), except we skip straight to the "expression-with-
   side-effects" bit and omit everything else.

   The caller doesn't need to pass break_label/continue_label; they're
   not used for this code path.
*/
extern void parse_statement_singleexpr(assembly_operand AO)
{
    int res;
    int saved_entire_flag;
    
    res = parse_named_label_statements();
    if (!res)
        return;

    saved_entire_flag = (execution_never_reaches_here & EXECSTATE_ENTIRE);
    if (execution_never_reaches_here)
        execution_never_reaches_here |= EXECSTATE_ENTIRE;

    llvm_direct_expression_statement(AO);
    code_generate(AO, VOID_CONTEXT, -1);
    
    if (vivc_flag) {
        panic_mode_error_recovery();
    }
    else {
        /* StatementTerminator... */
        get_next_token();
        if ((token_type != SEP_TT) || (token_value != SEMICOLON_SEP))
        {   ebf_curtoken_error("';'");
            put_token_back();
        }
    }

    if (saved_entire_flag)
        execution_never_reaches_here |= EXECSTATE_ENTIRE;
    else
        execution_never_reaches_here &= ~EXECSTATE_ENTIRE;
}

/* ========================================================================= */
/*   Data structure management routines                                      */
/* ------------------------------------------------------------------------- */

extern void init_states_vars(void)
{
}

extern void states_begin_pass(void)
{
}

extern void states_allocate_arrays(void)
{
}

extern void states_free_arrays(void)
{
}

/* ========================================================================= */
