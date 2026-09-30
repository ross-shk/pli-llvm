// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// main.cpp — plic driver.
//
// Pipeline: source -> preprocessor -> lexer -> parser -> sema -> LLVM IR ->
// clang (assemble, optimize, link with libpli). See docs/ARCHITECTURE.md.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "diag.h"
#include "explain.h"
#include "hir.h"
#include "irgen.h"
#include "lexer.h"
#include "parser.h"
#include "preprocessor.h"
#include "sema.h"

#include "llvm/Config/llvm-config.h"
// Host.h moved from Support to TargetParser in LLVM 19; support both.
#if __has_include("llvm/TargetParser/Host.h")
#include "llvm/TargetParser/Host.h"
#else
#include "llvm/Support/Host.h"
#endif

#ifndef PLIC_RUNTIME_LIB
#define PLIC_RUNTIME_LIB ""
#endif

#ifndef PLIC_INSTALL_RUNTIME_LIB
#define PLIC_INSTALL_RUNTIME_LIB ""
#endif

// Path to the bitcode runtime (OPTIMIZATION.md §10, P3): build/runtime.bc,
// baked in at build time like PLIC_RUNTIME_LIB. IRGen merges the pli_*
// definitions from it into the MAIN unit so clang sees real bodies;
// --no-bitcode-runtime restores the sectioned-archive path and --runtime-bc
// overrides the location.
#ifndef PLIC_RUNTIME_BC
#define PLIC_RUNTIME_BC ""
#endif

#ifndef PLIC_INSTALL_RUNTIME_BC
#define PLIC_INSTALL_RUNTIME_BC ""
#endif

// Version and LLVM identity baked in at build time (self-contained milestone:
// `plic version` reports the toolchain without invoking sub-tools).
#ifndef PLIC_VERSION
#define PLIC_VERSION "unknown"
#endif

#ifndef PLIC_LLVM_VERSION
#define PLIC_LLVM_VERSION LLVM_VERSION_STRING
#endif

// The clang used to assemble/optimize/link the emitted IR. plic emits IR in the
// syntax of the LLVM it was built against (e.g. the `memory(none)` attribute),
// so the matching clang must be used; the build bakes its path in. Overridable
// with --clang. Falls back to `clang` on PATH.
#ifndef PLIC_CLANG
#define PLIC_CLANG "clang"
#endif

namespace fs = std::filesystem;

static void usage() {
  std::cout
      << "plic — PL/I compiler (LLVM backend)\n"
         "\n"
         "usage: plic [options] file.pli...\n"
         "\n"
         "Multiple inputs compile like cc: each file becomes an independent\n"
         "relocatable object and they are linked together with libpli into one\n"
         "output (-o names it). -c writes <base>.o per input and does not link.\n"
         "\n"
         "options:\n"
         "  -o <file>        output file (default: a.out, or <base>.ll with -emit-llvm)\n"
         "                   (single input only with -emit-llvm/--print-hir/-fsyntax-only,\n"
         "                   and with -c when more than one input is given)\n"
         "  -c               compile each input to a relocatable object (no linking)\n"
         "  -emit-llvm       write LLVM IR per input and stop\n"
         "  --print-hir      lower to HIR and print it per input, then stop\n"
         "  -fsyntax-only    parse and analyse each input only\n"
         "  -O0 -O1 -O2 -O3  optimization level passed to the LLVM pipeline (default -O2)\n"
         "  --no-size-checks elide FIXED overflow traps program-wide (cf. (NOSIZE), ADR-111)\n"
         "  --release        maximum optimization + stripped binary (minimal size)\n"
         "  --debug          no optimization + debug info (-O0 -g)\n"
         "  --keep-ll        keep the intermediate .ll next to the output\n"
         "  --runtime <lib>  path to libpli.a (default: baked in at build time)\n"
         "  --no-bitcode-runtime\n"
         "                   link the sectioned libpli.a instead of merging the\n"
         "                   pli_* definitions from runtime.bc into the MAIN unit\n"
         "  --runtime-bc <f> path to runtime.bc (default: baked in at build time)\n"
         "  --flto=thin|full explicit ThinLTO / full LTO on the compile+link steps\n"
         "  --fprofile-generate  instrument the build for PGO (writes a profile)\n"
         "  --fprofile-use=<d>   rebuild using a merged profile (llvm-profdata out)\n"
         "  --clang <path>   clang to assemble/link the IR (default: LLVM's clang)\n"
         "  --triple <t>     target triple (default: host triple, in-process)\n"
         "  --sysparm <s>    value returned by the SYSPARM builtin (rule (123))\n"
         "  -L <dir>         add a library search path to the link step\n"
         "  -I <dir>         add a %INCLUDE search directory (repeatable; -I<dir> too)\n"
         "  -l<lib>          link a library (e.g. -lm) on the link step\n"
         "  -Wl,<flag>       pass a raw flag to the linker (repeatable)\n"
         "  --linker <ld>    select the linker via -fuse-ld=<ld>\n"
         "  -shared -static  produce a shared / static binary\n"
         "  --extra <a,b,c>  comma-separated extra backend args appended to the link\n"
         "  --explain <n>    print TR 25.084 rule (n)'s production and exit\n"
         "  --version        print plic + LLVM versions and the host triple, then exit\n"
         "  -v               show the sub-commands being run\n"
         "  -h, --help       this message\n"
         "\n"
         "  plic version     same as --version (Go/Zig style)\n";
}

// Go/Zig-style toolchain report: no subprocess, all baked in or in-process.
static void printVersion() {
  std::cout << "plic " << PLIC_VERSION << " (LLVM " << PLIC_LLVM_VERSION << ", "
            << llvm::sys::getDefaultTargetTriple() << ")\n";
}

static std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s)
    out += c == '\'' ? "'\\''" : std::string(1, c);
  return out + "'";
}

static fs::path executablePath(const char* arg0) {
  fs::path p(arg0);
  if (p.has_parent_path())
    return fs::absolute(p);
  const char* path = std::getenv("PATH");
  if (!path)
    return p;
  std::stringstream dirs(path);
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    fs::path candidate = fs::path(dir.empty() ? "." : dir) / p;
    if (fs::exists(candidate))
      return fs::absolute(candidate);
  }
  return p;
}

// Compile one translation unit through the full pipeline (preprocessor ->
// lexer -> parser -> sema -> HIR -> IRGen) and assemble it to a relocatable
// object with the backend clang. Multi-input drivers call this once per file.
// Per-file terminal modes (-emit-llvm, --print-hir, -fsyntax-only) apply to
// this unit and return without linking. In link mode `outObj` receives the
// object path for the caller to link. Returns false on any failure.
static bool compileOne(Preprocessor& preprocessor, const std::string& input, std::string& output,
                       std::string& triple, const std::string& clangPath,
                       const std::string& sysparm, bool sysparmExplicit, bool compileOnly,
                       bool semaCompileOnly, bool emitLLVM, bool syntaxOnly, bool print_hir,
                       bool keepLL, bool verbose, bool noSizeChecks, const std::string& optLevel,
                       const std::string& backendFlags, const fs::path& keepLLDir, int fileIndex,
                       std::string* outObj, const std::string& runtimeBc, bool linkBitcode) {
  std::string src;
  if (!preprocessor.run(input, src))
    return false;

  Diags diags(input);
  diags.setSource(&src);

  // --- front end ---------------------------------------------------------
  Lexer lexer(src, diags);
  std::vector<Token> toks = lexer.run();
  if (!diags.ok())
    return false;

  Parser parser(std::move(toks), diags);
  std::unique_ptr<Program> prog = parser.parse();
  if (!diags.ok())
    return false;

  Sema sema(diags);
  // SYSPARM: explicit --sysparm wins over $PLIC_SYSPARM (rule (123)).
  std::string effSysparm = sysparm;
  if (!sysparmExplicit)
    if (const char* env = std::getenv("PLIC_SYSPARM"))
      effSysparm = env;
  sema.setSysparm(effSysparm);
  // In multi-input link mode every unit is a relocatable object like `-c`:
  // no MAIN fallback promotion, so a library module with no OPTIONS(MAIN)
  // compiles cleanly and only its external procedures are emitted.
  sema.run(*prog, semaCompileOnly);
  if (!diags.ok())
    return false;

  if (syntaxOnly)
    return true;

  // Lower the typed AST to HIR (ADR-005). `--print-hir` shows it and stops.
  HProgram hir = lower(*prog);
  if (print_hir) {
    printHIR(hir, std::cout);
    return true;
  }

  // --- code generation ---------------------------------------------------
  // Default triple comes from LLVM itself, in-process (no `clang -dumpmachine`
  // subprocess since the self-contained milestone).
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();
  IRGen irgen(diags, sema, triple, noSizeChecks, runtimeBc, linkBitcode);
  std::string ir = irgen.run(hir);
  if (!diags.ok())
    return false;

  fs::path inPath(input);
  std::string base = inPath.stem().string();

  if (emitLLVM) {
    // -o is restricted to a single input (checked in main); otherwise each
    // input writes its own <base>.ll.
    std::string dest = output.empty() ? base + ".ll" : output;
    std::ofstream os(dest, std::ios::binary);
    if (!os) {
      std::cerr << "plic: cannot write " << dest << "\n";
      return false;
    }
    os << ir;
    return true;
  }

  // Object destination, mirroring cc: -c single with -o names the object, -c
  // without -o writes <base>.o in the cwd; link mode uses a per-input temp
  // object (the index keeps distinct stems apart across directories).
  fs::path objPath;
  if (compileOnly && !output.empty())
    objPath = output;
  else if (compileOnly)
    objPath = fs::path(base) += ".o";
  else
    objPath = fs::temp_directory_path() /
              (base + "-" + std::to_string(getpid()) + "-" + std::to_string(fileIndex) + ".o");

  fs::path llPath = keepLL ? keepLLDir / (base + ".ll")
                           : fs::temp_directory_path() / (base + "-" + std::to_string(getpid()) +
                                                          "-" + std::to_string(fileIndex) + ".ll");
  {
    std::ofstream os(llPath, std::ios::binary);
    if (!os) {
      std::cerr << "plic: cannot write " << llPath << "\n";
      return false;
    }
    os << ir;
  }

  std::string cmd = shellQuote(clangPath) + " -Wno-override-module " + optLevel + backendFlags +
                    " " + shellQuote(llPath.string()) + " -c -o " + shellQuote(objPath.string());
  if (verbose)
    std::cerr << "+ " << cmd << "\n";
  int rc = system(cmd.c_str());
  if (!keepLL) {
    std::error_code ec;
    fs::remove(llPath, ec);
  }
  if (rc != 0) {
    std::cerr << "plic: backend failed\n";
    return false;
  }

  if (!compileOnly)
    *outObj = objPath.string();
  return true;
}

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string output, runtimeLib = PLIC_RUNTIME_LIB, runtimeBc = PLIC_RUNTIME_BC, triple;
  std::string clangPath = PLIC_CLANG;
  std::string sysparm;
  bool sysparmExplicit = false;
  std::string optLevel = "-O2";
  std::vector<std::string> linkArgs;    // extra args appended to the link step
  std::vector<std::string> includeDirs; // %INCLUDE search dirs (-I, repeatable)
  bool emitLLVM = false, syntaxOnly = false, keepLL = false, verbose = false, compileOnly = false;
  bool runtimeExplicit = false, print_hir = false, release = false, debug = false;
  bool noSizeChecks = false, wantVersion = false;
  bool runtimeBcExplicit = false, useBitcode = true, pgoGenerate = false;
  std::string ltoKind, pgoUse;
  int explain = 0;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "plic: missing argument for " << what << "\n";
        exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "-o")
      output = next("-o");
    else if (a == "-c")
      compileOnly = true;
    else if (a == "-emit-llvm" || a == "--emit-llvm")
      emitLLVM = true;
    else if (a == "--print-hir")
      print_hir = true;
    else if (a == "-fsyntax-only")
      syntaxOnly = true;
    else if (a == "--keep-ll")
      keepLL = true;
    else if (a == "--runtime") {
      runtimeLib = next("--runtime");
      runtimeExplicit = true;
    } else if (a == "--no-bitcode-runtime")
      useBitcode = false;
    else if (a == "--runtime-bc") {
      runtimeBc = next("--runtime-bc");
      runtimeBcExplicit = true;
    } else if (a == "--flto=thin" || a == "--flto=full")
      ltoKind = a.substr(7);
    else if (a == "--fprofile-generate")
      pgoGenerate = true;
    else if (a.rfind("--fprofile-use=", 0) == 0)
      pgoUse = a.substr(15);
    else if (a == "--fprofile-use")
      pgoUse = next("--fprofile-use");
    else if (a == "--clang")
      clangPath = next("--clang");
    else if (a == "--triple")
      triple = next("--triple");
    else if (a == "--sysparm") {
      sysparm = next("--sysparm");
      sysparmExplicit = true;
    } else if (a.rfind("--sysparm=", 0) == 0) {
      sysparm = a.substr(10);
      sysparmExplicit = true;
    } else if (a == "-L")
      linkArgs.push_back("-L" + next("-L"));
    else if (a == "-I")
      includeDirs.push_back(next("-I"));
    else if (a.rfind("-I", 0) == 0)
      includeDirs.push_back(a.substr(2));
    else if (a.rfind("-l", 0) == 0)
      linkArgs.push_back(a);
    else if (a.rfind("-Wl,", 0) == 0)
      linkArgs.push_back(a);
    else if (a == "--linker")
      linkArgs.push_back("-fuse-ld=" + next("--linker"));
    else if (a == "-shared" || a == "-static")
      linkArgs.push_back(a);
    else if (a == "--extra") {
      // Comma-separated extra backend args, e.g. --extra -mllvm,-print-after-all.
      std::stringstream ss(next("--extra"));
      std::string tok;
      while (std::getline(ss, tok, ','))
        if (!tok.empty())
          linkArgs.push_back(tok);
    } else if (a == "--explain") {
      const std::string n = next("--explain");
      char* end = nullptr;
      long v = strtol(n.c_str(), &end, 10);
      if (end == n.c_str() || *end != '\0' || v < 1 || v > 151) {
        std::cerr << "plic: --explain needs a rule number 1..151\n";
        return 2;
      }
      explain = (int)v;
    } else if (a == "-v")
      verbose = true;
    else if (a == "--version")
      wantVersion = true;
    else if (a == "-O0" || a == "-O1" || a == "-O2" || a == "-O3" || a == "-Os")
      optLevel = a;
    else if (a == "--release")
      release = true;
    else if (a == "--no-size-checks")
      noSizeChecks = true;
    else if (a == "--debug")
      debug = true;
    else if (!a.empty() && a[0] == '-') {
      std::cerr << "plic: unknown option " << a << "\n";
      return 2;
    } else
      inputs.push_back(a);
  }

  // `--explain` needs no input file: print the production and exit.
  if (explain) {
    if (!explainRule(explain)) {
      std::cerr << "plic: no TR 25.084 rule (" << explain << ")\n";
      return 1;
    }
    return 0;
  }

  // `plic version` / `plic --version`: no input file needed.
  if (wantVersion || (inputs.size() == 1 && inputs[0] == "version")) {
    printVersion();
    return 0;
  }

  if (inputs.empty()) {
    usage();
    return 2;
  }

  if (!runtimeExplicit && !runtimeLib.empty() && !fs::exists(runtimeLib)) {
    fs::path installed = PLIC_INSTALL_RUNTIME_LIB;
    if (installed.empty() || !fs::exists(installed))
      installed = executablePath(argv[0]).parent_path().parent_path() / "lib/libpli.a";
    if (fs::exists(installed))
      runtimeLib = installed.string();
  }
  // Bitcode runtime (OPTIMIZATION.md §10, P3): by default IRGen merges the
  // pli_* definitions out of runtime.bc into the MAIN unit (lib units keep
  // external references resolved by that one shared copy). An explicitly
  // named runtime.bc must exist (a typo is a user error); the baked-in
  // default falls back to the sectioned archive when missing (e.g. an older
  // build tree). --no-bitcode-runtime always selects the archive path.
  if (!runtimeBcExplicit && !runtimeBc.empty() && !fs::exists(runtimeBc)) {
    fs::path installed = PLIC_INSTALL_RUNTIME_BC;
    if (installed.empty() || !fs::exists(installed))
      installed = executablePath(argv[0]).parent_path().parent_path() / "lib/runtime.bc";
    if (fs::exists(installed))
      runtimeBc = installed.string();
  }
  if (useBitcode && !runtimeBc.empty() && !fs::exists(runtimeBc)) {
    if (runtimeBcExplicit) {
      std::cerr << "plic: no such runtime bitcode: " << runtimeBc << "\n";
      return 2;
    }
    if (verbose)
      std::cerr << "plic: runtime.bc not found at " << runtimeBc
                << "; linking the sectioned archive instead\n";
    useBitcode = false;
  }

  Preprocessor preprocessor;
  for (const std::string& d : includeDirs)
    preprocessor.addIncludeDir(d);
  // Colon-separated like CPATH: searched after -I, before the default dir.
  bool hasEnvPath = false;
  if (const char* env = std::getenv("PLIC_INCLUDE_PATH")) {
    std::stringstream ss(env);
    std::string dir;
    while (std::getline(ss, dir, ':'))
      if (!dir.empty()) {
        preprocessor.addIncludeDir(dir);
        hasEnvPath = true;
      }
  }
  // Executable-relative default (mirrors the runtime-lib fallback below).
  std::string defaultInc =
      (executablePath(argv[0]).parent_path().parent_path() / "share/plic/include").string();
  preprocessor.addIncludeDir(defaultInc);
  if (verbose) {
    std::cerr << "plic: include search dirs:\n";
    for (const std::string& d : includeDirs)
      std::cerr << "plic:   " << d << "\n";
    if (hasEnvPath)
      std::cerr << "plic:   $PLIC_INCLUDE_PATH\n";
    std::cerr << "plic:   " << defaultInc << "\n";
  }

  // clang-matching guards: a single -o cannot name more than one output.
  const bool multi = inputs.size() > 1;
  if (compileOnly && multi && !output.empty()) {
    std::cerr << "plic: cannot specify -o when generating multiple output files\n";
    return 2;
  }
  if (multi && !output.empty() && (emitLLVM || syntaxOnly || print_hir)) {
    std::cerr << "plic: -o is ambiguous with a per-file mode and multiple inputs\n";
    return 2;
  }

  // --release / --debug are overarching presets that select the underlying
  // optimization and debug-info knobs: release = -O3 + minimal size
  // (dead-strip + strip symbol table); debug = -O0 + DWARF debug info.
  std::string backendFlags;
  if (release) {
    optLevel = "-O3";
    backendFlags = " -Wl,-dead_strip -Wl,-S";
  } else if (debug) {
    optLevel = "-O0";
    backendFlags = " -g";
  }
  // Explicit opt-in PGO/LTO (OPTIMIZATION.md §10, P3): the flags reach both
  // the per-file compile and the final link, so instrumented objects link
  // their profiling runtime and -flto objects meet a -flto link step.
  if (!ltoKind.empty())
    backendFlags += " -flto=" + ltoKind;
  if (pgoGenerate)
    backendFlags += " -fprofile-generate";
  if (!pgoUse.empty())
    backendFlags += " -fprofile-use=" + shellQuote(pgoUse);

  // Compile each input to its own object; per-file modes (-c, -emit-llvm,
  // --print-hir, -fsyntax-only) stop after all inputs are handled.
  const bool terminalMode = compileOnly || emitLLVM || syntaxOnly || print_hir;
  // Every unit of a multi-input link is a relocatable object (like -c): only
  // units that actually declare OPTIONS(MAIN) emit a `main` shim.
  const bool semaCompileOnly = compileOnly || multi;
  // --keep-ll places each .ll next to the output (or the cwd with no -o).
  fs::path keepLLDir = output.empty() ? fs::path(".") : fs::path(output).parent_path();
  if (keepLLDir.empty())
    keepLLDir = ".";
  std::vector<std::string> objs;
  // Bitcode runtime: the MAIN unit embeds the pli_* definitions; the archive
  // always stays on the link line, where it is never pulled once the symbols
  // are defined by that embedded copy (runtime globals must stay shared).
  const bool linkBitcode = useBitcode && !emitLLVM;
  for (size_t i = 0; i < inputs.size(); ++i) {
    std::string outObj;
    if (!compileOne(preprocessor, inputs[i], output, triple, clangPath, sysparm, sysparmExplicit,
                    compileOnly, semaCompileOnly, emitLLVM, syntaxOnly, print_hir, keepLL, verbose,
                    noSizeChecks, optLevel, backendFlags, keepLLDir, (int)i, &outObj, runtimeBc,
                    linkBitcode))
      return 1;
    if (!compileOnly)
      objs.push_back(outObj);
  }
  if (terminalMode)
    return 0;

  // --- link step ----------------------------------------------------------
  // Link the per-file objects with libpli. With the bitcode runtime the MAIN
  // object already defines the pli_* symbols, so the archive's members are
  // simply never pulled (no duplicates); --no-bitcode-runtime compiles every
  // unit against the archive as before. Drop unreferenced runtime sections
  // (the archive is sectioned, ADR-079); multitasking (QR2.8) runs on pthreads.
  if (output.empty())
    output = "a.out";
  std::string cmd = shellQuote(clangPath) + " -Wno-override-module " + optLevel + backendFlags;
#ifdef __APPLE__
  cmd += " -Wl,-dead_strip";
#else
  cmd += " -Wl,--gc-sections";
#endif
  cmd += " -pthread";
  for (const std::string& o : objs)
    cmd += " " + shellQuote(o);
  if (!runtimeLib.empty())
    cmd += " " + shellQuote(runtimeLib);
  for (const std::string& la : linkArgs)
    cmd += " " + la; // link flags
  cmd += " -o " + shellQuote(output);
  if (verbose)
    std::cerr << "+ " << cmd << "\n";
  int rc = system(cmd.c_str());
  // Remove the temp objects produced for this link.
  for (const std::string& o : objs) {
    std::error_code ec;
    fs::remove(o, ec);
  }
  if (rc != 0) {
    std::cerr << "plic: backend failed\n";
    return 1;
  }
  return 0;
}
