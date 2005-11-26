/*
   This file is part of Kvasir, a C/C++ front end for the Daikon
   dynamic invariant detector built upon the Fjalar framework

   Copyright (C) 2004-2005 Philip Guo, MIT CSAIL Program Analysis Group

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* kvasir_main.c:
   Initialization code, command-line option handling, and file handling
*/

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE /* FOR O_LARGEFILE */

#include "tool.h"
#include "kvasir_main.h"
#include "decls-output.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/errno.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef DYNCOMP
#include "dyncomp_main.h"
#include "dyncomp_runtime.h"
#endif

#include "../fjalar_tool.h"
#include "../fjalar_main.h"
#include "../fjalar_runtime.h"

// Global variables that are set by command-line options
char* kvasir_decls_filename = 0;
char* kvasir_dtrace_filename = 0;
char* kvasir_program_stdout_filename = 0;
char* kvasir_program_stderr_filename = 0;
Bool kvasir_dtrace_append = False;
Bool kvasir_dtrace_no_decs = False;
Bool kvasir_dtrace_gzip = False;
Bool kvasir_output_fifo = False;
Bool kvasir_decls_only = False;
Bool kvasir_repair_format = False;
Bool kvasir_print_debug_info = False;
Bool actually_output_separate_decls_dtrace = 0;
Bool print_declarations = 1;

Bool kvasir_with_dyncomp = False;
Bool dyncomp_no_gc = False;
Bool dyncomp_fast_mode = False;
int  dyncomp_gc_after_n_tags = 10000000;
Bool dyncomp_without_dtrace = False;
Bool dyncomp_print_debug_info = False;
Bool dyncomp_print_incremental = False;
Bool dyncomp_separate_entry_exit_comp = False;



FILE* decls_fp = 0; // File pointer for .decls file (this will point
                    // to the same thing as dtrace_fp by default since
                    // both .decls and .dtrace are outputted to .dtrace
                    // unless otherwise noted by the user)

FILE* dtrace_fp = 0; // File pointer for dtrace file (from dtrace-output.c)
static char *dtrace_filename; /* File name to open dtrace_fp on */

char* decls_folder = "daikon-output/";
static char* decls_ext = ".decls";
static char* dtrace_ext = ".dtrace";

static int createFIFO(const char *filename);
static int openDtraceFile(const char *fname);
static char splitDirectoryAndFilename(const char* input, char** dirnamePtr, char** filenamePtr);

// Lots of boring file-handling stuff:

void openTheDtraceFile() {
  openDtraceFile(dtrace_filename);
  VG_(free)(dtrace_filename);
  dtrace_filename = 0;
}

// if (actually_output_separate_decls_dtrace):
//   Create a decls file with the name "daikon-output/x.decls"
//   where x is the application name (by default)
//   and initializes the file pointer decls_fp.
//   Also creates a corresponding .dtrace file, but doesn't open it yet.
// else: --- (DEFAULT)
//   Create a dtrace file and initialize both decls_fp and dtrace_fp
//   to point to it
char createDeclsAndDtraceFiles(char* appname)
{
  char* dirname = 0;
  char* filename = 0;
  char* newpath_decls = 0;
  char* newpath_dtrace;
  int success = 0;
  int ret;

  // Free VisitedStructsTable if it has been allocated
  if (VisitedStructsTable)
    {
      genfreehashtable(VisitedStructsTable);
    }
  VisitedStructsTable = 0;

  // Step 1: Make a path to .decls and .dtrace files
  //         relative to daikon-output/ folder

  if (!splitDirectoryAndFilename(appname, &dirname, &filename))
    {
      VG_(printf)( "Failed to parse path: %s\n", appname);
      return 0;
    }

  DPRINTF("**************\ndirname=%s, filename=%s\n***********\n",
	  dirname, filename);

  if (actually_output_separate_decls_dtrace) {
    if (kvasir_decls_filename) {
      newpath_decls = VG_(strdup)(kvasir_decls_filename);
    }
    else {
      newpath_decls = (char*)VG_(malloc)((VG_(strlen)(decls_folder) +
					  VG_(strlen)(filename) +
					  VG_(strlen)(decls_ext) + 1) *
					 sizeof(char));

      VG_(strcpy)(newpath_decls, decls_folder);
      VG_(strcat)(newpath_decls, filename);
      VG_(strcat)(newpath_decls, decls_ext);
    }

    if (kvasir_dtrace_filename) {
      newpath_dtrace = VG_(strdup)(kvasir_dtrace_filename);
    }
    else {
      newpath_dtrace = (char*)VG_(malloc)((VG_(strlen)(decls_folder) +
					   VG_(strlen)(filename) +
					   VG_(strlen)(dtrace_ext) + 1) *
					  sizeof(char));

      VG_(strcpy)(newpath_dtrace, decls_folder);
      VG_(strcat)(newpath_dtrace, filename);
      VG_(strcat)(newpath_dtrace, dtrace_ext);
    }
  }
  else { // DEFAULT - just .dtrace
    if (kvasir_dtrace_filename) {
      newpath_dtrace = VG_(strdup)(kvasir_dtrace_filename);
    }
    else {
      newpath_dtrace = (char*)VG_(malloc)((VG_(strlen)(decls_folder) +
					   VG_(strlen)(filename) +
					   VG_(strlen)(dtrace_ext) + 1) *
					  sizeof(char));

      VG_(strcpy)(newpath_dtrace, decls_folder);
      VG_(strcat)(newpath_dtrace, filename);
      VG_(strcat)(newpath_dtrace, dtrace_ext);
    }
  }

  // Step 2: Make the daikon-output/ directory
  ret = mkdir(decls_folder, 0777); // more abbreviated UNIX form
  if (ret == -1 && errno != EEXIST)
    VG_(printf)( "Couldn't create %s: %s\n", decls_folder, strerror(errno));

  // ASSUME mkdir succeeded! (or that the directory already exists)

  // Step 3: Make the .decls and .dtrace FIFOs, if requested
  if (kvasir_output_fifo) {
    if (actually_output_separate_decls_dtrace) {
      if (!createFIFO(newpath_decls))
	VG_(printf)( "Trying as a regular file instead.\n");
      if (!createFIFO(newpath_dtrace))
	VG_(printf)( "Trying as a regular file instead.\n");
    }
    else {
      if (!createFIFO(newpath_dtrace))
	VG_(printf)( "Trying as a regular file instead.\n");
    }
  }

  dtrace_filename = VG_(strdup)(newpath_dtrace); /* But don't open it til later */

  // Step 4: Open the .decls file for writing
  if (actually_output_separate_decls_dtrace) {
    success = (decls_fp = fopen(newpath_decls, "w")) != 0;

    if (!success)
      VG_(printf)( "Failed to open %s for declarations: %s\n",
		   newpath_decls, strerror(errno));
  }
  else { // Default
    openTheDtraceFile();

    // decls_fp and dtrace_fp both point to the .dtrace file
    if (print_declarations) {
      decls_fp = dtrace_fp;
    }
    else {
      decls_fp = 0;
    }
  }

  VG_(free)(filename);
  VG_(free)(dirname);

  if (actually_output_separate_decls_dtrace) {
    if (!kvasir_decls_filename) {
      VG_(free)(newpath_decls);
    }
    if (!kvasir_dtrace_filename) {
      VG_(free)(newpath_dtrace);
    }
  }
  else {
    if (!kvasir_dtrace_filename) {
      VG_(free)(newpath_dtrace);
    }
  }

  return success;
}


// Splits up the input string (passed as char* input)
// into two strings (dirname and filename) that are separated
// by the first '/' that is recognized parsing from right
// to left - this breaks up a full path into a filename
// and a directory:
// Before: input = "../tests/IntTest/IntTest"
// After:  *dirnamePtr = "../tests/IntTest/" *filenamePtr = "IntTest"
// Postcondition - *dirname and *filename are malloc'ed - must be FREE'd!!!
// Return 1 on success, 0 on failure
// TODO: This can be replaced with calls to the glibc dirname() and basename() functions
static char splitDirectoryAndFilename(const char* input, char** dirnamePtr, char** filenamePtr)
{
  int len = VG_(strlen)(input);
  int i, j;

  // We need this to be static or else dirname and filename
  // will dereference to junk
  static char* filename = 0;
  static char* dirname = 0;

  if (len <= 0)
    return 0;

  for (i = len - 1; i >= 0; i--)
    {
      if ((input[i] == '/') && ((i + 1) < len))
        {
          //          printf("i=%d, len=%d\n", i, len);
          filename = VG_(malloc)((len - i) * sizeof(char));
          dirname = VG_(malloc)((i + 2) * sizeof(char));

          // I didn't get the regular strncpy to work properly ...
          //          strncpy(dirname, input, i + 1);
          //          strncpy(filename, input + i + 1, len - i - 1);

          // Make my own strncpy:
          for (j = 0; j <= i; j++)
            {
              dirname[j] = input[j];
              //              printf("dirname[%d]=%c\n", j, dirname[j]);
            }
          dirname[i + 1] = '\0';
          //          printf("dirname[%d]=%c\n", i + 1, dirname[i + 1]);
          for (j = i + 1; j < len; j++)
            {
              filename[j - i - 1] = input[j];
              //              printf("filename[%d]=%c\n", j - i - 1, filename[j - i - 1]);
            }
          filename[len - i - 1] = '\0';
          //          printf("filename[%d]=%c\n", len - i - 1, filename[len - i - 1]);


          *filenamePtr = filename;
          *dirnamePtr = dirname;

          return 1;
        }
    }
  // If we don't find a '/' anywhere, just set filename to equal input
  filename = VG_(strdup)(input);
  *filenamePtr = filename;
  return 1;
}

static int createFIFO(const char *filename) {
  int ret;
  ret = remove(filename);
  if (ret == -1 && errno != ENOENT) {
    VG_(printf)( "Couldn't replace old file %s: %s\n", filename,
	    strerror(errno));
    return 0;
  }
  ret = mkfifo(filename, 0666);
  if (ret == -1) {
    VG_(printf)( "Couldn't make %s as a FIFO: %s\n", filename,
	    strerror(errno));
    return 0;
  }
  return 1;
}


/* Return a file descriptor for a stream with similar semantics to
   what you'd get in a Unix shell by saying ">fname". Prints an error
   and returns -1 if something goes wrong. */
static int openRedirectFile(const char *fname) {
  int new_fd;
  if (fname[0] == '&') {
    new_fd = dup(atoi(fname + 1));
    if (new_fd == -1) {
      VG_(printf)( "Couldn't duplicate FD `%s': %s\n",
	      fname+1, strerror(errno));
      return -1;
    }
  } else {
    new_fd = open(fname, O_WRONLY|O_CREAT|O_LARGEFILE|O_TRUNC, 0666);
    if (new_fd == -1) {
      VG_(printf)( "Couldn't open %s for writing: %s\n",
	      fname, strerror(errno));
      return -1;
    }
  }
  return new_fd;
}

static int gzip_pid = 0;

static int openDtraceFile(const char *fname) {
  const char *mode_str;
  char *stdout_redir = kvasir_program_stdout_filename;
  char *stderr_redir = kvasir_program_stderr_filename;

  char *env_val = getenv("DTRACEAPPEND");
  if (env_val || kvasir_dtrace_append) {
    // If we are appending and not printing out separate decls and
    // dtrace files, do NOT print out decls again since we assume that
    // our existing dtrace file already has the decls info in it and
    // we don't want to confuse Daikon (or bloat up the file size) by
    // repeating this information
    if (!actually_output_separate_decls_dtrace) {
      print_declarations = 0;
    }
    mode_str = "a";
  }
  else {
    mode_str = "w";
  }

  // If we're sending trace data to stdout, we definitely don't want the
  // program's output going to the same place.
  if (VG_STREQ(fname, "-") && !stdout_redir) {
    stdout_redir = "/dev/tty";
  }

  if (kvasir_dtrace_gzip || getenv("DTRACEGZIP")) {
    int fds[2]; /* fds[0] for reading (child), fds[1] for writing (parent) */
    pid_t pid;
    int fd;
    int mode;
    char *new_fname = VG_(malloc)(strlen(fname) + 4);
    VG_(strcpy)(new_fname, fname);
    VG_(strcat)(new_fname, ".gz");

    if (pipe(fds) < 0)
      return 0;

    if (!(dtrace_fp = fdopen(fds[1], "w")) || (pid = fork()) < 0) {
      close(fds[0]);
      close(fds[1]);
      return 0;
    }
    fixBuffering(dtrace_fp);

    if (!pid) {
      /* In child */
      char *const argv[] = {"gzip", "-c", 0};
      close(fds[1]);

      /* Redirect stdin from the pipe */
      close(0);
      dup2(fds[0], 0);
      close(fds[0]);

      if (!VG_STREQ(fname, "-")) {
	/* Redirect stdout to the dtrace.gz file */
	mode = O_CREAT | O_LARGEFILE | O_TRUNC |
	  (*mode_str == 'a' ? O_APPEND : O_WRONLY);
	fd = open(new_fname, mode, 0666);
	if (fd == -1) {
	  VG_(printf)( "Couldn't open %s for writing\n", fname);
	}
	close(1);
	dup2(fd, 1);
	close(fd);
      }

      execv("/bin/gzip", argv);
      _exit(127);
    }

    close(fds[0]);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    gzip_pid = pid;
  } else if VG_STREQ(fname, "-") {
    int dtrace_fd = dup(1);
    dtrace_fp = fdopen(dtrace_fd, mode_str);
    if (!dtrace_fp) {
      return 0;
    }
    fixBuffering(dtrace_fp);
  } else {
    dtrace_fp = fopen(fname, mode_str);
    if (!dtrace_fp) {
      return 0;
    }
    fixBuffering(dtrace_fp);
  }

  if (stdout_redir) {
    int new_stdout = openRedirectFile(stdout_redir);
    if (new_stdout == -1)
      return 0;
    close(1);
    dup2(new_stdout, 1);
    if (stderr_redir && VG_STREQ(stdout_redir, stderr_redir)) {
      /* If the same name was supplied for stdout and stderr, do the
	 equivalent of the shell's 2>&1, rather than having them overwrite
	 each other */
      close(2);
      dup2(new_stdout, 2);
      stderr_redir = 0;
    }
    close(new_stdout);
  }

  if (stderr_redir) {
    int new_stderr = openRedirectFile(stderr_redir);
    if (new_stderr == -1)
      return 0;
    close(2);
    dup2(new_stderr, 2);
    close(new_stderr);
  }

  return 1;
}

// Close the stream and finish writing the .dtrace file
// as well as all other open file streams
void finishDtraceFile()
{
  if (dtrace_fp) /* If something goes wrong, we can be called with this null */
    fclose(dtrace_fp);
  if (gzip_pid) {
    int status;
    waitpid(gzip_pid, &status, 0);
    /* Perhaps check return value? */
    gzip_pid = 0;
  }
}



void fjalar_tool_pre_clo_init()
{
  // Nothing to do here
}

// Initialize kvasir after processing command-line options
void fjalar_tool_post_clo_init()
{
  // Special-case .dtrace handling if kvasir_dtrace_filename ends in ".gz"
  if (kvasir_dtrace_filename) {
    int filename_len = VG_(strlen)(kvasir_dtrace_filename);
    if VG_STREQ(kvasir_dtrace_filename + filename_len - 3, ".gz") {
      DPRINTF("\nFilename ends in .gz\n");
      // Chop off '.gz' from the end of the filename
      kvasir_dtrace_filename[filename_len - 3] = '\0';
      // Activate kvasir_dtrace_gzip
      kvasir_dtrace_gzip = True;
    }
  }

  // Output separate .decls and .dtrace files if:
  // --decls-only is on OR --decls-file=<filename> is on
  // OR kvasir_with_dyncomp is ON (since DynComp needs to create .decls
  // at the END of the target program's execution so that it can include
  // the comparability info)
  if (kvasir_decls_only || kvasir_decls_filename || kvasir_with_dyncomp) {
    DPRINTF("\nSeparate .decls\n\n");
    actually_output_separate_decls_dtrace = True;
  }

  // Special handling for BOTH kvasir_with_dyncomp and kvasir_decls_only
  // We need to actually do a full .dtrace run but just not output anything
  // to the .dtrace file
  if (kvasir_decls_only && kvasir_with_dyncomp) {
     kvasir_decls_only = False;
     dyncomp_without_dtrace = True;
  }

  // If we are only printing .dtrace and have --dtrace-no-decs,
  // then do not print out declarations
  if (!actually_output_separate_decls_dtrace && kvasir_dtrace_no_decs) {
     print_declarations = 0;
  }

  // If we are using DynComp with the garbage collector, initialize
  // g_oldToNewMap:
#ifdef DYNCOMP
  extern UInt* g_oldToNewMap;
  if (kvasir_with_dyncomp && !dyncomp_no_gc) {
     g_oldToNewMap = VG_(shadow_alloc)((dyncomp_gc_after_n_tags + 1) * sizeof(*g_oldToNewMap));
  }
#endif

  createDeclsAndDtraceFiles(executable_filename);

  // Remember to not actually output the .decls right now when we're
  // running DynComp.  We need to wait until the end to actually
  // output .decls, but we need to make a fake run in order to set up
  // the proper data structures
  outputDeclsFile(kvasir_with_dyncomp);

  // TODO: Re-factor this
  if (actually_output_separate_decls_dtrace && !dyncomp_without_dtrace) {
    openTheDtraceFile();
  }
}

void fjalar_tool_print_usage()
{
   VG_(printf)("\n  User options for Kvasir and DynComp:\n");

   VG_(printf)(
"\n  Output file format:\n"
"    --decls-file=<string>    The output .decls file location\n"
"                             (forces generation of separate .decls file)\n"
"    --decls-only             Exit after creating .decls file [--no-decls-only]\n"
"    --dtrace-file=<string>   The output .dtrace file location\n"
"                             [daikon-output/PROGRAM_NAME.dtrace]\n"
"    --dtrace-no-decs         Do not include declarations in .dtrace file\n"
"                             [--no-dtrace-no-decs]\n"
"    --dtrace-append          Appends .dtrace data to the end of an existing .dtrace file\n"
"                             [--no-dtrace-append]\n"
"    --dtrace-gzip            Compresses .dtrace data [--no-dtrace-gzip]\n"
"                             (Automatically ON if --dtrace-file string ends in '.gz')\n"
"    --output-fifo            Create output files as named pipes [--no-output-fifo]\n"
"    --program-stdout=<file>  Redirect instrumented program stdout to file\n"
"                             [Kvasir's stdout, or /dev/tty if --dtrace-file=-]\n"
"    --program-stderr=<file>  Redirect instrumented program stderr to file\n"

"\n  DynComp dynamic comparability analysis\n"
"    --with-dyncomp           Enables DynComp comparability analysis\n"
"    --gc-num-tags            The number of tags that get assigned between successive runs\n"
"                             of the garbage collector (between 1 and INT_MAX)\n"
"                             (The default is to garbage collect every 10,000,000 tags created)\n"
"    --no-dyncomp-gc          Do NOT use the tag garbage collector for DynComp.  (Faster\n"
"                             but may run out of memory for long-running programs)\n"
"    --dyncomp-fast-mode      Approximates the handling of literals for comparability.\n"
"                             (Loses some precision but faster and takes less memory)\n"
"    --separate-entry-exit-comp  Allows variables to have distinct comparability\n"
"                                numbers at function entrance/exit when run with\n"
"                                DynComp.  This provides more accuracy, but may\n"
"                                sometimes lead to output that Daikon cannot accept.\n"

"\n  Misc. options:\n"
"    --repair-format          Output format for data structure repair tool (internal use)\n"

"\n  Debugging:\n"
"    --asserts-aborts         Turn on safety asserts and aborts (OFF BY DEFAULT)\n"
"                             [--no-asserts-aborts]\n"
"    --kvasir-debug           Print Kvasir-internal debug messages [--no-debug]\n"
"    --dyncomp-debug          Print DynComp debug messages (--with-dyncomp must also be on)\n"
"                             [--no-dyncomp-debug]\n"
"    --dyncomp-print-inc      Print DynComp comp. numbers at the execution\n"
"                             of every program point (for debug only)\n"
   );
}

/* Like VG_BOOL_CLO, but of the form "--foo", "--no-foo" rather than
   "--foo=yes", "--foo=no". Note that qq_option should not have a
   leading "--". */

#define VG_YESNO_CLO(qq_option, qq_var) \
   if (VG_CLO_STREQ(arg, "--"qq_option)) { (qq_var) = True; } \
   else if (VG_CLO_STREQ(arg, "--no-"qq_option))  { (qq_var) = False; }

// Processes command-line options
Bool fjalar_tool_process_cmd_line_option(Char* arg)
{
  VG_STR_CLO(arg, "--decls-file", kvasir_decls_filename)
  else VG_STR_CLO(arg, "--dtrace-file",    kvasir_dtrace_filename)
  else VG_YESNO_CLO("dtrace-append",  kvasir_dtrace_append)
  else VG_YESNO_CLO("dtrace-no-decs",  kvasir_dtrace_no_decs)
  else VG_YESNO_CLO("dtrace-gzip",    kvasir_dtrace_gzip)
  else VG_YESNO_CLO("output-fifo",    kvasir_output_fifo)
  else VG_YESNO_CLO("decls-only",     kvasir_decls_only)
  else VG_YESNO_CLO("repair-format", kvasir_repair_format)
  else VG_YESNO_CLO("kvasir-debug",      kvasir_print_debug_info)
  else VG_STR_CLO(arg, "--program-stdout", kvasir_program_stdout_filename)
  else VG_STR_CLO(arg, "--program-stderr", kvasir_program_stderr_filename)

  else VG_YESNO_CLO("with-dyncomp",   kvasir_with_dyncomp)
  else VG_YESNO_CLO("no-dyncomp-gc",     dyncomp_no_gc)
  else VG_YESNO_CLO("dyncomp-fast-mode", dyncomp_fast_mode)
  else VG_BNUM_CLO(arg, "--gc-num-tags", dyncomp_gc_after_n_tags,
		   1, 0x7fffffff)
  else VG_YESNO_CLO("dyncomp-debug",  dyncomp_print_debug_info)
  else VG_YESNO_CLO("dyncomp-print-inc",  dyncomp_print_incremental)
  else VG_YESNO_CLO("separate-entry-exit-comp",  dyncomp_separate_entry_exit_comp)
  else
    return False;   // If no options match, return False so that an error
                    // message can be reported by the Valgrind core.

  // Return True if at least one option has been matched to indicate success:
  return True;
}


void fjalar_tool_finish() {
  extern UInt nextTag;

#ifdef DYNCOMP
  if (kvasir_with_dyncomp) {
     // Do one extra propagation of variable comparability at the end
     // of execution once all of the value comparability sets have
     // been properly updated:
     DC_extra_propagate_val_to_var_sets();

     // Now print out the .decls file at the very end of execution:
     DC_outputDeclsAtEnd();
  }

  DYNCOMP_DPRINTF("\n*** nextTag: %u ***\n\n", nextTag);
#endif

  if (!dyncomp_without_dtrace) {
     finishDtraceFile();
  }
}


void fjalar_tool_handle_first_function_entrance() {
}

void fjalar_tool_handle_function_entrance(FunctionExecutionState* f_state) {

  // TODO: Call out to kvasir_runtime.c
}

void fjalar_tool_handle_function_exit(FunctionExecutionState* f_state) {

#ifdef DYNCOMP
  if (kvasir_with_dyncomp) {
    // For DynComp, update tags of saved register values
    int i;

    UInt EAXtag = 0;
    UInt EDXtag = 0;
    UInt FPUtag = 0;

    EAXtag = VG_(get_EAX_tag)(currentTID);
    EDXtag = VG_(get_EDX_tag)(currentTID);
    FPUtag = VG_(get_FPU_stack_top_tag)(currentTID);

    for (i = 0; i < 4; i++) {
      set_tag((Addr)(&(f_state->EAX)) + (Addr)i, EAXtag);
      set_tag((Addr)(&(f_state->EDX)) + (Addr)i, EDXtag);
      set_tag((Addr)(&(f_state->FPU)) + (Addr)i, FPUtag);
    }

    for (i = 4; i < 8; i++) {
      set_tag((Addr)(&(top->FPU)) + (Addr)i, FPUtag);
    }
  }
#endif

  // TODO: Call out to kvasir_runtime.c
}




// Constructors and destructors for classes that can be sub-classed:

// Default constructor that should return a particular sub-class of an
// object.  This should call VG_(calloc) the proper amount of space
// for the object and initialize it with whatever initial state is
// necessary.
VariableEntry* constructVariableEntry() {
  return (VariableEntry*)(VG_(calloc)(1, sizeof(VariableEntry)));
}

TypeEntry* constructTypeEntry() {
  return (TypeEntry*)(VG_(calloc)(1, sizeof(TypeEntry)));
}

FunctionEntry* constructFunctionEntry() {
  return (FunctionEntry*)(VG_(calloc)(1, sizeof(FunctionEntry)));
}

// Destructors that should clean-up and then call VG_(free) on the
// respective entries.
void destroyVariableEntry(VariableEntry* v) {
  VG_(free)(v);
}

void destroyTypeEntry(TypeEntry* t) {
  VG_(free)(t);
}

void destroyFunctionEntry(FunctionEntry* f) {
  VG_(free)(f);
}
