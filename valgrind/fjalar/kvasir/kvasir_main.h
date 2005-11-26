/*
   This file is part of Kvasir, a C/C++ front end for the Daikon
   dynamic invariant detector built upon the Fjalar framework

   Copyright (C) 2004-2005 Philip Guo, MIT CSAIL Program Analysis Group

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* kvasir_main.h:
   Initialization code and command-line option handling
*/

#ifndef KVASIR_MAIN_H
#define KVASIR_MAIN_H

#include "../fjalar_main.h"

FILE* decls_fp; // File pointer for .decls file (this will point
                // to the same thing as dtrace_fp by default since
                // both .decls and .dtrace are outputted to .dtrace
                // unless otherwise noted by the user)

FILE* dtrace_fp; // File pointer for dtrace file (from dtrace-output.c)


// Kvasir/DynComp-specific global variables that are set by
// command-line options
char* kvasir_decls_filename;
char* kvasir_dtrace_filename;
char* kvasir_program_stdout_filename;
char* kvasir_program_stderr_filename;
Bool kvasir_dtrace_append;
Bool kvasir_dtrace_no_decs;
Bool kvasir_dtrace_gzip;
Bool kvasir_output_fifo;
Bool kvasir_decls_only;
Bool kvasir_repair_format;
Bool kvasir_print_debug_info;
Bool actually_output_separate_decls_dtrace;
Bool print_declarations;

Bool kvasir_with_dyncomp;
Bool dyncomp_no_gc;
Bool dyncomp_fast_mode;
int  dyncomp_gc_after_n_tags;
Bool dyncomp_without_dtrace;
Bool dyncomp_print_debug_info;
Bool dyncomp_print_incremental;
Bool dyncomp_separate_entry_exit_comp;

#define DPRINTF(...) do { if (kvasir_print_debug_info) \
      VG_(printf)(__VA_ARGS__); } while (0)

#define DYNCOMP_DPRINTF(...) do { if (kvasir_with_dyncomp && dyncomp_print_debug_info) \
      VG_(printf)(__VA_ARGS__); } while (0)

#endif
