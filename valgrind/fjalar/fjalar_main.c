/*
   This file is part of Fjalar, a dynamic analysis framework for C/C++
   programs.

   Copyright (C) 2004-2006 Philip Guo (pgbovine@alum.mit.edu),
   MIT CSAIL Program Analysis Group

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* fjalar_main.c:

   This file contains most of the code to interact with the Valgrind
   core.  It is called from mc_main.c since mc_main.c is the
   launching-point for Fjalar.
*/

#include "my_libc.h"


#include "pub_tool_basics.h"
#include "pub_tool_aspacemgr.h"
#include "pub_tool_hashtable.h"     // For mc_include.h
#include "pub_tool_xarray.h" //for clientstate
#include "pub_tool_options.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_machine.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_oset.h"
#include "pub_tool_replacemalloc.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_clientstate.h"

#include "generate_fjalar_entries.h"
#include "fjalar_main.h"
#include "fjalar_runtime.h"
#include "fjalar_tool.h"
#include "fjalar_select.h"
#include "disambig.h"
#include "mc_include.h"
#include "typedata.h"
#include "vex_common.h"

// Global variables that are set by command-line options
Bool fjalar_debug = False;
Bool fjalar_with_gdb = False;
Bool fjalar_ignore_globals = False;
Bool fjalar_ignore_static_vars = False;
Bool fjalar_all_static_vars = False;
Bool fjalar_default_disambig = False;
Bool fjalar_smart_disambig = False;
Bool fjalar_output_struct_vars = False;
Bool fjalar_flatten_arrays = False;
Bool fjalar_func_disambig_ptrs = False;
Bool fjalar_disambig_ptrs = False;
int  fjalar_array_length_limit = -1;

// adjustable via the --struct-depth=N option:
UInt MAX_VISIT_STRUCT_DEPTH = 4;
// adjustable via the --nesting-depth=N option:
UInt MAX_VISIT_NESTING_DEPTH = 2;

// These are used as both strings and boolean flags -
// They are initialized to 0 upon initiation so if they are
// never filled with values by the respective command-line
// options, then they can be treated as False
char* fjalar_dump_prog_pt_names_filename = 0;
char* fjalar_dump_var_names_filename = 0;
char* fjalar_trace_prog_pts_filename = 0;
char* fjalar_trace_vars_filename = 0;
char* fjalar_disambig_filename = 0;
char* fjalar_xml_output_filename = 0;

// The filename of the target executable:
char* executable_filename = 0;

// Mapping between Dwarf Register numbers and
// valgrind function to return the value
// Below comment is ripped from GCC 4.3.1

/* Define the register numbers to be used in Dwarf debugging information.
   The SVR4 reference port C compiler uses the following register numbers
   in its Dwarf output code:
	0 for %eax (gcc regno = 0)
	1 for %ecx (gcc regno = 2)
	2 for %edx (gcc regno = 1)
	3 for %ebx (gcc regno = 3)
	4 for %esp (gcc regno = 7)
	5 for %ebp (gcc regno = 6)
	6 for %esi (gcc regno = 4)
	7 for %edi (gcc regno = 5)
   The following three DWARF register numbers are never generated by
   the SVR4 C compiler or by the GNU compilers, but SDB on x86/svr4
   believes these numbers have these meanings.
	8  for %eip    (no gcc equivalent)
	9  for %eflags (gcc regno = 17)
	10 for %trapno (no gcc equivalent)

*/
Addr (*get_reg[11])( ThreadId tid ) = {
  VG_(get_xAX),
  VG_(get_xCX),
  VG_(get_xDX),
  VG_(get_xBX),
  VG_(get_SP),
  VG_(get_FP),
  VG_(get_xSI),
  VG_(get_xDI),
  VG_(get_IP),
  NULL,
  NULL
};

// For debugging purposes, a mapping between
// DWARF location atoms and their string
// representation
char* dwarf_reg_string[9] = {
  "xAX",
  "xCX",
  "xDX",
  "xBX",
  "xSP",
  "xFP",
  "xSI",
  "xDI",
  "xIP"
};


// TODO: We cannot sub-class FunctionExecutionState unless we make
// this into an array of pointers.
// Also, from the fact that this is a single global, you can see
// we only support single-threaded execution.
FunctionExecutionState FunctionExecutionStateStack[FN_STACK_SIZE];
// The first free slot in FunctionExecutionStateStack
// right above the top element:
int fn_stack_first_free_index;
// The top element of the stack is:
//   FunctionExecutionStateStack[fn_stack_first_free_index]

// "Pushes" a new entry onto the stack by returning a pointer to it
// and incrementing fn_stack_first_free_index (Notice that this has
// slightly has different semantics than a normal stack push)
__inline__ FunctionExecutionState* fnStackPush(void) {
  tl_assert(fn_stack_first_free_index < FN_STACK_SIZE);
  fn_stack_first_free_index++;
  return &(FunctionExecutionStateStack[fn_stack_first_free_index - 1]);
}

// Returns the top element of the stack and pops it off
__inline__ FunctionExecutionState* fnStackPop(void) {
  tl_assert(fn_stack_first_free_index > 0);
  fn_stack_first_free_index--;
  return &(FunctionExecutionStateStack[fn_stack_first_free_index]);
}

// Returns the top element of the stack
__inline__ FunctionExecutionState* fnStackTop(void) {
  tl_assert(fn_stack_first_free_index >= 0);
  return &(FunctionExecutionStateStack[fn_stack_first_free_index - 1]);
}

typedef VG_REGPARM(1) void entry_func(FunctionEntry *);

static void handle_possible_entry_func(MCEnv *mce, Addr64 addr,
				       struct genhashtable *table,
				       char *func_name,
				       entry_func func) {
  IRDirty  *di;
  FunctionEntry *entry;

  // For GCC 3.x we'll continue using the old entry heuristics, otherwise
  // we use the entry in the endOfBb table.
  if(fjalar_gcc3) {
    entry = gengettable(table, (void *)(Addr)addr);
  } else {
    entry = gengettable(FunctionTable_by_endOfBb, (void *)(Addr)addr); // Main special case takes priority
  }

  if(!entry) {
      return;
  }

  // If fjalar_trace_prog_pts_filename is on (we are using a ppt list
  // file), then DO NOT generate IR code to call helper functions for
  // functions whose name is NOT located in prog_pts_tree. It's faster
  // to filter them out at translation-time instead of run-time
  if (entry && (!fjalar_trace_prog_pts_filename ||
		prog_pts_tree_entry_found(entry))) {
    UWord entry_w = (UWord)entry;
    di = unsafeIRDirty_0_N(1/*regparms*/, func_name, func,
			 mkIRExprVec_1(IRExpr_Const(IRConst_UWord(entry_w))));

    entry->entryIP = addr;

    // For function entry, we are interested in observing the stack
    // and frame pointers so make sure that they're updated by setting
    // the proper annotations:

    FJALAR_DPRINTF("Found a valid entry point at %x for\n", (UInt)addr);

    // We need essentially all registers.
    di->nFxState = 2;
    di->fxState[0].fx     = Ifx_Read;
    di->fxState[0].offset = mce->layout->offset_SP;
    di->fxState[0].size   = mce->layout->sizeof_SP;
    di->fxState[1].fx     = Ifx_Read;
    di->fxState[1].offset = mce->layout->offset_FP;
    di->fxState[1].size   = mce->layout->sizeof_FP;
    di->fxState[2].fx     = Ifx_Read;
    di->fxState[2].offset = mce->layout->offset_IP;
    di->fxState[2].size   = mce->layout->sizeof_IP;

    di->fxState[2].fx     = Ifx_Read;
    di->fxState[2].offset = mce->layout->offset_xAX;
    di->fxState[2].size   = mce->layout->sizeof_xAX;
    di->fxState[3].fx     = Ifx_Read;
    di->fxState[3].offset = mce->layout->offset_xBX;
    di->fxState[3].size   = mce->layout->sizeof_xBX;
    di->fxState[4].fx     = Ifx_Read;
    di->fxState[4].offset = mce->layout->offset_xCX;
    di->fxState[4].size   = mce->layout->sizeof_xCX;
    di->fxState[5].fx     = Ifx_Read;
    di->fxState[5].offset = mce->layout->offset_xDX;
    di->fxState[5].size   = mce->layout->sizeof_xDX;

    stmt('V',  mce, IRStmt_Dirty(di) );
  }
}

// This gets updated whenever we encounter a Ist_IMark instruction.
// It is required to track function exits because the address does not
// come with the Ist_Exit IR instruction:
static Addr currentAddr = 0;

// This hash table simply ensures do not calculate the entry point for
// a function multiple times. Due to the way entry is handled, the
// handle_possble_entry function is called once for every instruction
// in the original program. We should only calculate the entry point
// once at the first instruction of the function.
static struct  genhashtable *funcs_handled = NULL;

  // HANDLING FUNCTION ENTRY
  // For most functions the debugging information is essentially
  // useless before the prolog. This was mitigated in earlier versions
  // by entering functions at the instruction corresponding to the
  // first line in the source code (entryPC in Fjalar terms). This
  // causes a problem in some cases when the body of a function
  // consists only of a loop and the compiler decides to be off
  // about presenting line information. For Example:

  // (pseduo x86 assembly)
  // jmp Test
  // Body:
  // add
  // ... Body of loop
  // Test:
  // cmp eax ecx
  // jne Test

  // The issue occurs because some versions of GCC will choose to
  // correlate the first line of the function with the first instruction
  // after "jmp test". This makes it impossible for Fjalar to detect
  // function entry for functions whose loop body is never executed.
  // It also causes problems relating to detecting entry to a function
  // multiple times, though this has already been mitigated - see comment
  // above prime_functoin

  // This causes a problem as we can't enter at the first instruction due
  // to invalid debugging information, nor can we enter at the entryPC
  // due to loops (and possibly any function that begins with a conditional)

  // We solve this by applying the following heuristic to determine
  // the entry point to a function:

  // If the first basic block of a function ends before entryPC, use
  // the last instruction of that basic block as our entrypoint. Otherwise
  // use entryPC

static void find_entry_pt(MCEnv* mce, Addr64 addr, IRSB* bb_orig, FunctionEntry *f) {
  int i;
  Addr entry_pt = 0;
  // Silence warnings
  (void)mce; (void)addr;

  if(gencontains(funcs_handled, f))
    return;

  FJALAR_DPRINTF("[find_entry_pt] Searching for main entry instruction for %s\n", f->fjalar_name);
  for(i=0 ; i <  bb_orig->stmts_used; i++) {
    IRStmt *st = bb_orig->stmts[i];
    if(st->tag == Ist_IMark) {
      FJALAR_DPRINTF("\tEncountered IMark for address %x\n", (UInt)st->Ist.IMark.addr);
      if(st->Ist.IMark.addr <= f->entryPC) {
	entry_pt = st->Ist.IMark.addr;
      }
    }
  }
  tl_assert( entry_pt );

  FJALAR_DPRINTF("\t%x chosen for entry\n", (UInt)entry_pt);

  genputtable(funcs_handled, (void *)f, (void *)1);

  genputtable(FunctionTable_by_endOfBb,
	      (void *)entry_pt,
	      (void *)f);

}

// This is called whenever we encounter an IMark statement.  From the
// IR documentation (Copyright (c) 2004-2005 OpenWorks LLP):
//
// IMark(literal guest address, length)
//
// Semantically a no-op.  However, indicates that the IR statements
// which follow it originally came from a guest instruction of the
// stated length at the stated guest address.  This information is
// needed by some kinds of profiling tools.

// We will utilize this information to pause the target program at
// function entrances.  This is called from mc_translate.c.

// TODO: In-depth explanation of the main issue
void handle_possible_entry(MCEnv* mce, Addr64 addr, IRSB* bb_orig) {
  // Right now, for x86, we only care about 32-bit instructions

/*   IRDirty  *di; */
/*   UWord entry_w = (UWord)0; */
/*   // RUDD DEBUG Dump Vex State at every IMark */
/*   di = unsafeIRDirty_0_N(0/\*regparms*\/, "Dump state", VG_(dump_state), */
/*                          mkIRExprVec_1(IRExpr_Const(IRConst_UWord(entry_w)))); */

/*     // We need essentially all registers. */
/*     di->nFxState = 3; */
/*     di->fxState[0].fx     = Ifx_Read; */
/*     di->fxState[0].offset = mce->layout->offset_SP; */
/*     di->fxState[0].size   = mce->layout->sizeof_SP; */
/*     di->fxState[1].fx     = Ifx_Read; */
/*     di->fxState[1].offset = mce->layout->offset_FP; */
/*     di->fxState[1].size   = mce->layout->sizeof_FP; */
/*     di->fxState[2].fx     = Ifx_Read; */
/*     di->fxState[2].offset = mce->layout->offset_IP; */
/*     di->fxState[2].size   = mce->layout->sizeof_IP; */

/*     stmt('V',  mce, IRStmt_Dirty(di) ); */

  // REMEMBER TO ALWAYS UPDATE THIS regardless of whether this is
  // truly a function entry so that handle_possible_exit() can work
  // properly:
  currentAddr = (Addr)addr;

  if(!fjalar_gcc3) {
    FunctionEntry *entry = gengettable(FunctionTable, (void *)(Addr)addr);
    if(entry) {
      find_entry_pt(mce, addr, bb_orig, entry);
    }
  }



  // We're not splitting entry handling based on GCC version.
  // for GCC 3.x we're going to enter at the instruction
  // corresponding to the first line of code in a function
  // (f->entryPC in Fjalar's terms)

  // For GCC 4.x we're going to use a special heuristic for
  // determining the instruction to enter at. See comment
  // "HANDLING FUNCTION ENTRY" above find_entry_pt()

  if(fjalar_gcc3) {

    /* If this is the very first instruction in the function, add a call
       to the prime_function helper. */
    handle_possible_entry_func(mce, addr, FunctionTable,
			       "prime_function",
			       &prime_function);

    /* If this is the first instruction in the function after the prolog
       (not exclusive with the condition above), add a call to the
       enter_function helper. */
    handle_possible_entry_func(mce, addr, FunctionTable_by_entryPC,
			       "enter_function",
			       &enter_function);
  } else {
    handle_possible_entry_func(mce, addr, FunctionTable,
			       "enter_function",
			       &enter_function);
  }

}


// Handle a function exit statement, which contains a jump kind of
// 'Ret'.  It seems pretty accurate to cue off of currentAddr, a value
// that is updated every time an Ist_IMark statement is translated,
// which is quite often
void handle_possible_exit(MCEnv* mce, IRJumpKind jk) {
  if (Ijk_Ret == jk) {
    IRDirty  *di;

    FunctionEntry* curFuncPtr = getFunctionEntryFromAddr(currentAddr);

    if (curFuncPtr &&
	// Also, if fjalar_trace_prog_pts_filename is on (we are
	// reading in a ppt list file), then DO NOT generate IR code
	// to call helper functions for functions whose names are NOT
	// located in prog_pts_tree.  This will greatly speed up
	// processing because these functions are filtered out at
	// translation-time, not at run-time
	(!fjalar_trace_prog_pts_filename ||
	 prog_pts_tree_entry_found(curFuncPtr))) {

      FJALAR_DPRINTF("[handle_possible_exit] %s - %x\n", curFuncPtr->fjalar_name, (UInt)currentAddr);

      // The only argument to exit_function() is a pointer to the
      // FunctionEntry for the function that we are exiting
      di = unsafeIRDirty_0_N(1/*regparms*/,
			     "exit_function",
			     &exit_function,
			     mkIRExprVec_1(IRExpr_Const(IRConst_UWord((Addr)curFuncPtr))));

      // For function exit, we are interested in observing the ESP,
      // xAX, xDX, FTOP, and FPREG[], so make sure that they are
      // updated by setting the proper annotations.

      // For completeness let's also state the intention to observe
      // xBX and xCX since they can always be referenced by the
      // DWARF information - rudd
      di->nFxState = 1;
      di->fxState[0].fx     = Ifx_Read;
      di->fxState[0].offset = mce->layout->offset_SP;
      di->fxState[0].size   = mce->layout->sizeof_SP;

      di->fxState[1].fx     = Ifx_Read;
      di->fxState[1].offset = mce->layout->offset_xAX;
      di->fxState[1].size   = mce->layout->sizeof_xAX;

      di->fxState[2].fx     = Ifx_Read;
      di->fxState[2].offset = mce->layout->offset_xDX;
      di->fxState[2].size   = mce->layout->sizeof_xDX;

      di->fxState[3].fx     = Ifx_Read;
      di->fxState[3].offset = mce->layout->offset_xBX;
      di->fxState[3].size   = mce->layout->sizeof_xBX;

      di->fxState[4].fx     = Ifx_Read;
      di->fxState[4].offset = mce->layout->offset_xCX;
      di->fxState[4].size   = mce->layout->sizeof_xCX;

      di->fxState[5].fx     = Ifx_Read;
      di->fxState[5].offset = offsetof(VexGuestArchState, guest_FTOP);
      di->fxState[5].size   = sizeof(UInt); /* FTOP is 4 bytes even on x64 */

      di->fxState[6].fx     = Ifx_Read;
      di->fxState[6].offset = offsetof(VexGuestArchState, guest_FPREG);
      di->fxState[6].size   = 8 * sizeof(ULong);

      stmt('V',  mce, IRStmt_Dirty(di) );
    }
  }
}

/* A disadvantage of putting the call to enter_function after the
   prolog is that it occasionally ends up at a label that the compiler
   jumps back to in the middle of executing a function, say if the
   whole function is a single loop. If we were to do all the stuff in
   enter_function() again in this case, things would get very
   confused. Instead, we want to only do enter_function() once per
   invocation of the function, where we define an invocation to be an
   execution of the very first instruction. To accomplish that, we put
   a call to the prime_function() hook there; it initializes a global
   to point to the current function. In enter_function(), we check
   that pointer before doing anything, and then clear it. */
static FunctionEntry* primed_function = 0;
VG_REGPARM(1) void prime_function(FunctionEntry *f)
{
  primed_function = f;
  return;
}

/*
This is the hook into Valgrind that is called whenever the target
program enters a function.  Pushes an entry onto the top of
FunctionExecutionStateStack and calls out to a handler function
implemented by the Fjalar tool.
*/
VG_REGPARM(1)
void enter_function(FunctionEntry* f)
{
  FunctionExecutionState* newEntry;
  extern FunctionExecutionState* curFunctionExecutionStatePtr;

  ThreadId tid = VG_(get_running_tid)();
  Addr stack_ptr= VG_(get_SP)(tid);
  Addr frame_ptr = 0; /* E.g., %ebp */
  int local_stack, size;

  FJALAR_DPRINTF("[enter_function] startPC is: %x\n, entryPC is: %x\ncu_base: %x\n",  (UInt)f->startPC, (UInt)f->entryPC, f->cuBase);

  if(f->locList) {
    Addr eip = f->entryIP;
    location_list *ll;
    eip =  eip - f->cuBase;

    FJALAR_DPRINTF("\tCurrent EIP is: %x\n", (UInt)eip);
    FJALAR_DPRINTF("\tLocation list based function(offset from base: %x). offset is %lu\n",(UInt)eip, f->locListOffset);

    if (gencontains(loc_list_map, (void *)f->locListOffset)) {
      ll = gengettable(loc_list_map, (void *)f->locListOffset);

      // HACK. g++ and GCC handle location lists differently. GCC puts lists offsets
      // relative to the compilation unit, g++ uses the actual address. I'm going to
      // compare the location list ranges both to the cu_base offset, as well as
      // the function's entry point. This might break if there's every a case
      // where the compilation unit offset is a valid address in the program
      while(ll &&
	    !(((ll->begin <= eip) && (ll->end >= eip)) ||
	      ((ll->begin <= f->entryPC) && (ll->end >= f->entryPC)))) {
        FJALAR_DPRINTF("\tExamining loc list entry: %x - %x - %x\n", (UInt)ll->offset, (UInt)ll->begin, (UInt)ll->end);
        ll = ll->next;
      }

      if(ll) {
        FJALAR_DPRINTF("\tFound location list entry, finding register corresponding to dwarf #: %d with offset: %lu\n", ll->atom, ll->atom_offset);

        if(get_reg[ll->atom - DW_OP_breg0]) {
          frame_ptr = (*get_reg[ll->atom - DW_OP_breg0])(tid) + ll->atom_offset;
        }
      }

    }
  }


  // This is the old code to determine the frame this. Fallback to it if we don't
  // have a frame_base from the location_list path. This should keep GCC 3 working
  // fine.

  if(frame_ptr == 0) {
    if (f != primed_function)
        return;
    primed_function = 0;

    if (f->entryPC != f->startPC) {
      /* Prolog has run, so just use the real %ebp */
      frame_ptr = VG_(get_FP)(VG_(get_running_tid)());
    } else {
      FJALAR_DPRINTF("Faking prolog\n");
      /* Don't know about prolog, so fake its effects, given we know that
         ESP hasn't yet been modified: */
      frame_ptr = stack_ptr - 4;
    }
  }

  FJALAR_DPRINTF("\tEnter function: %s - StartPC: %p, EntryPC: %p, frame_ptr: %p\n",
		 f->fjalar_name, (void*)f->startPC, (void*)f->entryPC, frame_ptr);

  newEntry  = fnStackPush();
  newEntry->func = f;
  newEntry->func->FP = frame_ptr;
  newEntry->func->lowestSP = stack_ptr;
  newEntry->FP = frame_ptr;
  newEntry->lowestSP = stack_ptr;
  newEntry->xAX = 0;
  newEntry->xDX = 0;
  newEntry->FPU = 0;

  // Initialize virtual stack and copy parts of the Valgrind stack
  // into that virtual stack
  local_stack = frame_ptr - stack_ptr + VG_STACK_REDZONE_SZB; /* in our frame */
  tl_assert(local_stack >= 0);
  FJALAR_DPRINTF("frame_ptr: %x, stack_ptr: %x, VG_STACK_REDZONE: %d\n", frame_ptr, stack_ptr, VG_STACK_REDZONE_SZB);

  // The virtual stack consists of:
  // (1) local_stack: the entirety of the function's local stack (the
  // memory between the frame pointer and the stack pointer (including the extra
  // redzone)
  // (2) The return pointer, which is sizeof(Addr) bytes
  // (3) The saved base pointer, which is sizeof(Addr) bytes
  // (4) All formal parameters passed on the stack, which is
  //     f->formalParamStackByteSize bytes
 
  // Let's be conservative in how much we copy over to the Virtual stack. Due to the
  // stack alignment operations in main, we may need  as much as 16 bytes over the above.
  size = local_stack + f->formalParamStackByteSize + sizeof(Addr)*2 + 16;/* plus stuff in caller's*/

  tl_assert(size >= 0);
  if (size != 0) {
    newEntry->virtualStack = VG_(calloc)("fjalar_main.c: enter_func",  size, sizeof(char));
    newEntry->virtualStackByteSize = size;
    newEntry->virtualStackFPOffset = local_stack;

    VG_(memcpy)(newEntry->virtualStack,
		(char*)stack_ptr - VG_STACK_REDZONE_SZB, size);

    // VERY IMPORTANT!!! Copy all the A & V bits over the real stack to
    // virtualStack!!!  (As a consequence, this copies over the tags
    // as well - look in mc_main.c). Note that the way do this means
    // that the copy is now guest-accessible, if they guessed the
    // VG_(calloc)ed address, which is a bit weird. It would be more
    // elegant to copy the metadata to an inaccessible place, but that
    // would be more work.
    FJALAR_DPRINTF("Copying over stack [%x] -> [%x] %d bytes\n",stack_ptr - VG_STACK_REDZONE_SZB,  newEntry->virtualStack, size);
    mc_copy_address_range_state(stack_ptr - VG_STACK_REDZONE_SZB,
				(Addr)(newEntry->virtualStack), size);


    newEntry->func->guestStackStart = stack_ptr - VG_STACK_REDZONE_SZB;
    newEntry->func->guestStackEnd = newEntry->func->guestStackStart + size;
    newEntry->func->lowestVirtSP = (Addr)newEntry->virtualStack;

  }
  else {
    // Watch out for null pointer segfaults here:
    newEntry->virtualStack = 0;
    newEntry->virtualStackByteSize = 0;
  }


  // Do this AFTER initializing virtual stack and lowestSP
  curFunctionExecutionStatePtr = newEntry;
  fjalar_tool_handle_function_entrance(newEntry);
}

/*
This is the hook into Valgrind that is called whenever the target
program exits a function.  Initializes the top entry of
FunctionExecutionStateStack with return values from EAX, EDX, and FPU,
then calls out to a handler function implemented by the Fjalar tool.
*/
VG_REGPARM(1)
void exit_function(FunctionEntry* f)
{
  FunctionExecutionState* top = fnStackTop();
  extern FunctionExecutionState* curFunctionExecutionStatePtr;
  int i;
  ThreadId currentTID = VG_(get_running_tid)();

  // Ok, in Valgrind 2.X, we needed to directly code some assembly to
  // grab the top of the floating-point stack, but Valgrind 3.0
  // provides a virtual FPU stack, so we can just grab that.  Plus, we
  // now have shadow V-bits for the FPU stack.
  double fpuReturnVal = VG_(get_FPU_stack_top)(currentTID);


  // Get the value at the simulated %EAX (integer and pointer return
  // values are stored here upon function exit)
  Addr xAX = VG_(get_xAX)(currentTID);

  // Get the value of the simulated %EDX (the high 32-bits of the long
  // long int return value is stored here upon function exit)
  Addr xDX = VG_(get_xDX)(currentTID);


  // 64 bits
  // Use SHADOW values of Valgrind simulated registers to get V-bits
  UWord xAXshadow = VG_(get_shadow_xAX)(currentTID);
  UWord xDXshadow = VG_(get_shadow_xDX)(currentTID);
  ULong FPUshadow = VG_(get_shadow_FPU_stack_top)(currentTID);

  FJALAR_DPRINTF("Value of eax: %d, edx: %d\n",(int)xAX, (int)xDX);
  FJALAR_DPRINTF("Exit function: %s\n", f->fjalar_name);

  // s is null if an "unwind" is popped off the stack (WHAT?)
  // Only do something if top->func matches func
  if (!top->func) {
    VG_(printf)("More exit_function()s than entry_function()s!\n");
    return;
  } else if (!(top->func->fjalar_name) || (top->func != f)) {
    VG_(printf)("MISMATCHED on exit_function! %s != f: %s\n",
		top->func->fjalar_name,
		f->fjalar_name);
    return;
  }

  top->xAX = xAX;
  top->xDX = xDX;
  top->FPU = fpuReturnVal;

  // Very important!  Set the A and V bits of the appropriate
  // FunctionExecutionState object and the tags from the (x86) guest
  // state as well:
  /* XXX word size */
  for (i = 0; i < 4; i++) {
    set_abit_and_vbyte((Addr)(&top->xAX) + (Addr)i, VGM_BIT_VALID,
                      (xAXshadow & 0xff) << (i * 8));
    set_abit_and_vbyte((Addr)(&top->xDX) + (Addr)i, VGM_BIT_VALID,
                      (xDXshadow & 0xff) << (i * 8));
    set_abit_and_vbyte((Addr)(&top->FPU) + (Addr)i, VGM_BIT_VALID,
                      (FPUshadow & 0xff) << (i * 8));
  }

  for (i = 4; i < 8; i++) {
    set_abit_and_vbyte((Addr)(&top->FPU) + (Addr)i, VGM_BIT_VALID,
                       (FPUshadow & 0xff) << (i * 8));
  }

  curFunctionExecutionStatePtr = top;
  fjalar_tool_handle_function_exit(top);

  // Destroy the memory allocated by virtualStack
  // AFTER the tool has handled the exit
  if (top->virtualStack) {
    /* We were previously using the V bits associated with the area to
       store guest V bits, but Memcheck doesn't normally expect
       VG_(malloc)'ed memory to be client accessible, so we have to
       make it inaccessible again before allowing Valgrind's malloc to
       use it, lest assertions fail later. */
    mc_make_noaccess((Addr)top->virtualStack, top->virtualStackByteSize);
    VG_(free)(top->virtualStack);
  }

  // Pop at the VERY end after the tool is done handling the exit.
  // This is subtle but important:
  fnStackPop();
}


// Opens the appropriate files and loads data to handle selective
// program point tracing, selective variable tracing, and pointer type
// disambiguation.  Call this before initializeAllFjalarData() because
// it might depend on the vars_tree being initialized.
// Handles the following command-line options:
//   --ppt-list-file
//   --var-list-file
static void loadAuxiliaryFileData(void) {

  if (fjalar_trace_prog_pts_filename) {
    if ((trace_prog_pts_input_fp =
	 fopen(fjalar_trace_prog_pts_filename, "r"))) {
      VG_(printf)( "\nBegin processing program point list file \"%s\" ...\n",
		   fjalar_trace_prog_pts_filename);
      initializeProgramPointsTree();
      VG_(printf)( "Done processing program point list file \"%s\"\n",
		   fjalar_trace_prog_pts_filename);
    }
    else {
      VG_(printf)( "\nError: \"%s\" is an invalid filename for the program point list file specified by the --ppt-list-file option.\n\nExiting.\n\n",
		   fjalar_trace_prog_pts_filename);

      VG_(exit)(1);
    }
  }

  if (fjalar_trace_vars_filename) {
    if ((trace_vars_input_fp
	 = fopen(fjalar_trace_vars_filename, "r"))) {
      VG_(printf)( "\nBegin processing variable list file \"%s\" ...\n",
		   fjalar_trace_vars_filename);
      initializeVarsTree();
      VG_(printf)( "Done processing variable list file \"%s\"\n",
		   fjalar_trace_vars_filename);
    }
    else {
      VG_(printf)( "\nError: \"%s\" is an invalid filename for the variable list file specified by the --var-list-file option.\n\nExiting.\n\n",
		   fjalar_trace_vars_filename);

      VG_(exit)(1);
    }
  }
}


// If we want to dump program point list, variable list, or XML to
// output files, do it here, close the appropriate files, and then
// exit (notice that this supports writing to more than 1 kind of file
// before exiting).  We want to do this after
// initializeAllFjalarData() so that all of the data from
// generate_fjalar_entries.h will be available.
// This function has no effect if we are not using any of the options
// to output auxiliary files.
// Handles the following command-line options:
//   --dump-ppt-file
//   --dump-var-file
//   --xml-output-file
static void outputAuxiliaryFilesAndExit(void) {
  if (fjalar_dump_prog_pt_names_filename ||
      fjalar_dump_var_names_filename ||
      fjalar_xml_output_filename) {
    if (fjalar_dump_prog_pt_names_filename) {
      prog_pt_dump_fp = fopen(fjalar_dump_prog_pt_names_filename, "w");
      tl_assert(prog_pt_dump_fp);
      outputProgramPointsToFile();
      VG_(printf)("\nDone generating program point list (ppt-list) file %s\n",
                  fjalar_dump_prog_pt_names_filename);
      fclose(prog_pt_dump_fp);
      prog_pt_dump_fp = 0;
    }

    if (fjalar_dump_var_names_filename) {
      var_dump_fp = fopen(fjalar_dump_var_names_filename, "w");
      tl_assert(var_dump_fp);
      outputVariableNamesToFile();
      VG_(printf)("\nDone generating variable list (var-list) file %s\n",
                  fjalar_dump_var_names_filename);
      fclose(var_dump_fp);
      var_dump_fp = 0;
    }

    // Output the declarations in XML format if desired, and then exit:
    if (fjalar_xml_output_filename) {
      xml_output_fp = fopen(fjalar_xml_output_filename, "w");
      outputAllXMLDeclarations();
      VG_(printf)("\nDone generating XML file %s\n",
                  fjalar_xml_output_filename);
      fclose(xml_output_fp);
      xml_output_fp = 0;
    }

    VG_(exit)(0);
  }
}


// This is called before command-line options are processed
void fjalar_pre_clo_init()
{
  // Clear FunctionExecutionStateStack
  VG_(memset)(FunctionExecutionStateStack, 0,
	      FN_STACK_SIZE * sizeof(*FunctionExecutionStateStack));

  // TODO: Do we need to clear all global variables before processing
  // command-line options?  We don't need to as long as this function
  // is only run once at the beginning of program execution.

  // Make sure to execute this last!
  fjalar_tool_pre_clo_init();
}

// Initialize Fjalar after command-line options are processed
void fjalar_post_clo_init()
{
  char* DISAMBIG = ".disambig";
  int DISAMBIG_LEN = VG_(strlen)(DISAMBIG);

  // We need to turn off some VEX IR optimizations (primarily the one which
  // causes separate basic blocks to be stitched together) for the purpose of
  // detecting entry in main. see "HANDLING FUNCTION ENTRY" in find_entry_pt()
  VG_(clo_vex_control).iropt_unroll_thresh = 0;
  VG_(clo_vex_control).guest_chase_thresh = 0;

  executable_filename = VG_(args_the_exename);

  if (fjalar_with_gdb) {
    int x = 0;
    while (!x) {} /* In GDB, say "p x=1" and then "c" to continue */
  }


  funcs_handled= genallocatehashtable(0,(int (*)(void *,void *)) &equivalentIDs);

  // Handle variables set by command-line options:

  FJALAR_DPRINTF("\nReading binary file \"%s\" [0x%x] (Assumes that filename is first argument in client_argv)\n\n",
	  executable_filename, (unsigned int)executable_filename);

  // --disambig results in the disambig filename being ${executable_filename}.disambig
  // (overrides --disambig-file option)
  if (fjalar_default_disambig) {
    char* disambig_filename =
      VG_(calloc)("fjalar_main.c: fj_po_clo_init", VG_(strlen)(executable_filename) + DISAMBIG_LEN + 1,
	     sizeof(*disambig_filename));

    VG_(strcpy)(disambig_filename, executable_filename);
    VG_(strcat)(disambig_filename, DISAMBIG);
    fjalar_disambig_filename = disambig_filename;
  }



  FJALAR_DPRINTF("\n%s\n\n", fjalar_disambig_filename);

  // Calls into typedata.c:
  initialize_typedata_structures();

  FJALAR_DPRINTF("Typedata structures completed\n");

  // Calls into readelf.c:
  process_elf_binary_data(executable_filename);

  FJALAR_DPRINTF("Process elf binary completed\n");
  // Call this BEFORE initializeAllFjalarData() so that the vars_tree
  // objects can be initialized for the --var-list-file option:
  loadAuxiliaryFileData();

  // Calls into generate_fjalar_entries.c:
  initializeAllFjalarData();
  FJALAR_DPRINTF("Fjalar data initialized\n");
  if (fjalar_disambig_filename) {
    handleDisambigFile();
  }

  // Call this AFTER initializeAllFjalarData() so that all of the
  // proper data is ready:
  outputAuxiliaryFilesAndExit();

  FJALAR_DPRINTF("Files output\n");
  // Make sure to execute this last!
  fjalar_tool_post_clo_init();
  FJALAR_DPRINTF("Tool clo initialized\n");
}


// Prints the help message when Fjalar is invoked with the --help option
void fjalar_print_usage()
{
   VG_(printf)("\n  User options for Fjalar framework:\n");

   VG_(printf)(
"\n  Selective program point and variable tracing:\n"
"    --ppt-list-file=<string> Trace only the program points listed in this file\n"
"    --var-list-file=<string> Trace only the variables listed in this file\n"
"    --dump-ppt-file=<string> Outputs all program point names to a file\n"
"    --dump-var-file=<string> Outputs all variable names to a file\n"
"    --ignore-globals         Ignores all global variables [--no-ignore-globals]\n"
"    --ignore-static-vars     Ignores all static variables [--no-ignore-static-vars]\n"
"    --all-static-vars        Output all static vars [--no-all-static-vars]\n"

"\n  Pointer type disambiguation:\n"
"    --disambig-file=<string> Reads in disambig file if exists; otherwise creates one\n"
"    --disambig               Uses <program name>.disambig as the disambig file\n"
"    --smart-disambig         Infers sensible values for each entry in .disambig file\n"
"                             generated using the --disambig or --disambig-file options\n"
"    --func-disambig-ptrs     Treats function parameter and return value pointer\n"
"                             variables as pointing to a single element\n"
"    --disambig-ptrs          Treats all pointer vars. as pointing to a single element\n"

"\n  Misc. options:\n"
"    --flatten-arrays         Force flattening of all statically-sized arrays\n"
"    --array-length-limit=N   Only visit at most the first N elements of arrays\n"
"    --nesting-depth=N        Limits the maximum number of dereferences of any\n"
"                             structure to N (default is 2)\n"
"    --struct-depth=N         Limits the maximum number of dereferences of recursively\n"
"                             defined structures (i.e. linked lists) to N (default is 4)\n"
"                             (N must be an integer between 0 and 100)\n"
"    --output-struct-vars     Outputs struct variables along with their contents\n"

"\n  Debugging:\n"
"    --xml-output-file=<string>  Output declarations in XML format to a file\n"
"    --with-gdb               Hang during init. so that GDB can attach to it\n"
"    --fjalar-debug           Print internal Fjalar debug messages\n"
   );

   // Make sure to execute this last!
   fjalar_tool_print_usage();
}


// Processes command-line options and sets the values of the
// appropriate global variables (Called from
// MAC_(process_common_cmd_line_option)() in mac_shared.c)
Bool fjalar_process_cmd_line_option(Char* arg)
{

  if VG_YESNO_CLO(arg, "fjalar-debug", fjalar_debug) {}
  else if VG_YESNO_CLO(arg, "gcc4", fjalar_gcc4) {}
  else if VG_YESNO_CLO(arg, "with-gdb", fjalar_with_gdb) {}
  else if VG_YESNO_CLO(arg, "ignore-globals", fjalar_ignore_globals) {}
  else if VG_YESNO_CLO(arg, "ignore-static-vars", fjalar_ignore_static_vars) {}
  else if VG_YESNO_CLO(arg, "all-static-vars", fjalar_all_static_vars) {}
  else if VG_YESNO_CLO(arg, "gcc3", fjalar_gcc3) {}
  else if VG_YESNO_CLO(arg, "disambig", fjalar_default_disambig) {}
  else if VG_YESNO_CLO(arg, "smart-disambig", fjalar_smart_disambig) {}
  else if VG_YESNO_CLO(arg, "output-struct-vars", fjalar_output_struct_vars) {}
  else if VG_YESNO_CLO(arg, "flatten-arrays", fjalar_flatten_arrays) {}
  else if VG_YESNO_CLO(arg, "func-disambig-ptrs", fjalar_func_disambig_ptrs) {}
  else if VG_YESNO_CLO(arg, "disambig-ptrs", fjalar_disambig_ptrs) {}
  else if VG_BINT_CLO(arg, "--array-length-limit", fjalar_array_length_limit,
		      -1, 0x7fffffff) {}

  /* else if VG_BINT_CLO(arg, "--struct-depth",  MAX_VISIT_STRUCT_DEPTH, 0, 100)  {} // [0 to 100]
     else if VG_BINT_CLO(arg, "--nesting-depth", MAX_VISIT_NESTING_DEPTH, 0, 100) {} // [0 to 100] */
  else if VG_INT_CLO(arg, "--struct-depth",  MAX_VISIT_STRUCT_DEPTH) {}
  else if VG_INT_CLO(arg, "--nesting-depth", MAX_VISIT_NESTING_DEPTH) {}

  else if VG_STR_CLO(arg, "--dump-ppt-file",
		     fjalar_dump_prog_pt_names_filename) {}
  else if VG_STR_CLO(arg, "--dump-var-file",  fjalar_dump_var_names_filename){}
  else if VG_STR_CLO(arg, "--ppt-list-file",  fjalar_trace_prog_pts_filename){}
  else if VG_STR_CLO(arg, "--var-list-file",  fjalar_trace_vars_filename) {}
  else if VG_STR_CLO(arg, "--disambig-file",  fjalar_disambig_filename) {}
  else if VG_STR_CLO(arg, "--xml-output-file", fjalar_xml_output_filename) {}
  else
    return fjalar_tool_process_cmd_line_option(arg);

  return True;
}


// This runs after the target program exits
void fjalar_finish(void) {

  // If fjalar_smart_disambig is on, then
  // we must create the .disambig file at the very end after
  // Fjalar has run though the entire program so that it can
  // determine whether each pointer variable has only referenced
  // one element or multiple elements throughout this particular execution
  if (disambig_writing && fjalar_smart_disambig) {
    generateDisambigFile();
    VG_(printf)("\nDone generating .disambig file %s\n",
                fjalar_disambig_filename);
    fclose(disambig_fp);
    disambig_fp = 0;
  }

  // Make sure to execute this last!
  fjalar_tool_finish();
}
