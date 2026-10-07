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
#include <set>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif
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
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "codegen.h"
#include "embedded_runtime.h"
#include "target.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

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

#ifndef PLIC_VERSION
#define PLIC_VERSION "unknown"
#endif

#ifndef PLIC_LLVM_VERSION
#define PLIC_LLVM_VERSION LLVM_VERSION_STRING
#endif

// In-process lld link gate (review S1): 1 when the build linked lld
// (CMake defines it); the Makefile build leaves it 0 and uses clang.
#ifndef PLIC_HAVE_LLD
#define PLIC_HAVE_LLD 0
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
         "  --no-size-checks elide FIXED overflow traps (default at -O2/-O3;\n"
         "                   cf. (NOSIZE), ADR-111)\n"
         "  --size-checks    force FIXED overflow traps (overrides -O2/-O3 default)\n"
         "  --no-zero-divide elide ZERODIVIDE traps program-wide (cf. (NOZERODIVIDE), ADR-112)\n"
         "  --no-conversion   elide CONVERSION traps program-wide (cf. (NOCONVERSION), ADR-170)\n"
         "  --no-subscript    elide SUBSCRIPTRANGE checks (default at -O2/-O3;\n"
         "                   cf. (NOSUBSCRIPTRANGE), ADR-111)\n"
         "  --subscript-checks  force SUBSCRIPTRANGE checks (overrides -O2/-O3 default)\n"
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
         "  environment:\n"
         "  $PLIC_INCLUDE_PATH  colon-separated %INCLUDE search dirs (after -I)\n"
         "  $PLIC_LIB_PATH      colon-separated library search dirs (after -L)\n"
         "  $PLIC_SYSPARM       value returned by the SYSPARM builtin ((123))\n"
         "\n"
         "  library/include discovery:\n"
         "  plic walks upward from the source root (cwd) and the plic executable\n"
         "  directory, adding lib/ and lib/pli/ to library search paths and\n"
         "  include/ and inc/ to %INCLUDE search paths at each ancestor. On\n"
         "  Linux/macOS, /usr/lib, /usr/local/lib and the matching include paths\n"
         "  are added as static fallbacks.\n"
         "\n"
         "  plic version     same as --version (Go/Zig style)\n";
}

// Go/Zig-style toolchain report: no subprocess, all baked in or in-process.
static void printVersion() {
  // Version/build info embedded at build time (Go/Zig style).
  std::cout << "plic " << PLIC_VERSION << " (LLVM " << PLIC_LLVM_VERSION << ", "
            << llvm::sys::getDefaultTargetTriple() << ", runtime " << plic::embeddedLibPLISize()
            << " bytes)\n";
}

static std::string shellQuote(const std::string& s) {
#ifdef _WIN32
  // cmd.exe quoting: only quote when needed (spaces/tabs/quotes). Always
  // quoting breaks MSVC system() -> cmd /c quote stripping for executables
  // without spaces (e.g. C:/dev/.../clang.exe).
  if (s.find_first_of(" \t\"") == std::string::npos)
    return s;
  std::string out = "\"";
  for (char c : s)
    out += c == '"' ? "\\\"" : std::string(1, c);
  return out + "\"";
#else
  std::string out = "'";
  for (char c : s)
    out += c == '\'' ? "'\\''" : std::string(1, c);
  return out + "'";
#endif
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
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  while (std::getline(dirs, dir, sep)) {
    fs::path candidate = fs::path(dir.empty() ? "." : dir) / p;
    if (fs::exists(candidate))
      return fs::absolute(candidate);
  }
  return p;
}

// Zig-style upward filesystem walk: starting from `start`, walk to each ancestor
// (including start), and at each level check for `subdirs`. Directories that
// exist are added (deduped by weakly-canonical path via `seen`). Stops at the
// filesystem root where parent_path() == self.
static std::vector<fs::path> searchUpwards(const fs::path& start,
                                           const std::vector<std::string>& subdirs,
                                           std::set<std::string>& seen) {
  std::vector<fs::path> results;
  fs::path cur = fs::absolute(start);
  while (true) {
    for (const std::string& sub : subdirs) {
      fs::path candidate = cur / sub;
      std::error_code ec;
      if (fs::is_directory(candidate, ec)) {
        ec.clear();
        fs::path canon = fs::weakly_canonical(candidate, ec);
        if (ec)
          continue;
        std::string key = canon.string();
        if (seen.insert(key).second)
          results.push_back(canon);
      }
    }
    fs::path parent = cur.parent_path();
    if (cur == parent)
      break;
    cur = parent;
  }
  return results;
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
                       bool keepLL, bool verbose, bool noSizeChecks, bool noZdivChecks,
                       bool noConvChecks, bool noSubChecks, const std::string& optLevel,
                       const std::string& backendFlags, const fs::path& keepLLDir, int fileIndex,
                       std::string* outObj, const std::string& runtimeBc, bool linkBitcode,
                       bool linkRuntimeIn = false, bool forceClangPipeline = false) {
  // Default triple comes from LLVM itself, in-process (no `clang -dumpmachine`
  // subprocess since the self-contained milestone).
  // Use macosx15.0 for bitcode compatibility; SME features disabled in IRGen.
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();
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
  // (triple defaulted above: LLVM itself, in-process.)

  fs::path inPath(input);
  std::string base = inPath.stem().string();

  if (emitLLVM) {
    IRGen irgen(diags, sema, triple, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks);
    std::string ir = irgen.run(hir);
    if (!diags.ok())
      return false;
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

  // In-process codegen first (no subprocess): owned module -> TargetMachine
  // -> relocatable object. Falls back to the .ll + clang pipeline below.
  // Explicit PGO/LTO (forceClangPipeline) always uses the clang pipeline so
  // backendFlags reach the compile step (the in-process path takes none).
  if (!forceClangPipeline) {
    IRGen ipg(diags, sema, triple, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks, runtimeBc,
              linkBitcode);
    if (auto om = ipg.takeModule(hir)) {
      bool rtOk = true;
#if PLIC_HAVE_LLD
      // Single-module fast path: runtime linked at BC level (never `-c`).
      // File-based bitcode (linkBitcode) already embedded the runtime via
      // IRGen, so the embedded blob is only used when it did not.
      if (linkRuntimeIn && !linkBitcode && !compileOnly) {
        std::string rtErr;
        llvm::Triple targetTriple(triple);
        rtOk = plic::linkEmbeddedLibPLI(*om->mod, targetTriple, rtErr);
        if (!rtOk)
          std::cerr << "plic: " << rtErr << "\n";
      }
#endif
      if (rtOk) {
        // Create TargetMachine using the provided target triple.
        std::string tmErr;
        if (auto tm = plic::createTargetMachine(triple, optLevel, tmErr)) {
          // The IRGen layout is approximate; the TargetMachine owns the truth
          // (llc behaviour — the clang fallback used -Wno-override-module).
          om->mod->setDataLayout(tm->createDataLayout());
          // Also set module target triple to ensure consistent
          // feature handling during codegen (module triple affects some defaults).
          om->mod->setTargetTriple(llvm::Triple(triple));
          // Mirror the clang backend: optimize the (runtime-linked) IR at the
          // requested -O level before codegen.
          plic::optimizeModule(*om->mod, optLevel);
          std::error_code ec;
          llvm::raw_fd_ostream os(objPath.string(), ec, llvm::sys::fs::OF_None);
          std::string emErr;
          if (!ec && plic::emitObject(*om->mod, *tm, os, emErr)) {
            os.close();
            if (keepLL) {
              llvm::raw_fd_ostream llOs((keepLLDir / (base + ".ll")).string(), ec,
                                        llvm::sys::fs::OF_None);
              if (!ec) {
                llOs << "; Generated by plic (PL/I -> LLVM)\n";
                om->mod->print(llOs, nullptr);
              }
            }
            if (verbose)
              std::cerr << "plic: emitted " << objPath.string() << " in-process\n";
            if (!compileOnly)
              *outObj = objPath.string();
            return true;
          }
          if (verbose)
            std::cerr << "plic: in-process emit failed (" << emErr << "), using clang\n";
        } else if (verbose) {
          std::cerr << "plic: no target (" << tmErr << "), using clang\n";
        }
      }
    } else if (!diags.ok()) {
      return false;
    }
  }

  // Fallback: textual IR assembled by the backend clang.
  IRGen irgen(diags, sema, triple, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks, runtimeBc,
              linkBitcode);
  std::string ir = irgen.run(hir);
  if (!diags.ok())
    return false;

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
  bool noSizeChecks = false, noSizeChecksExplicit = false, noZdivChecks = false,
       noConvChecks = false, noSubChecks = false, noSubChecksExplicit = false, wantVersion = false;
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
    else if (a == "-O0" || a == "-O1" || a == "-O2" || a == "-O3" || a == "-Os")
      optLevel = a;
    else if (a == "--release")
      release = true;
    else if (a == "--no-size-checks") {
      noSizeChecks = true;
      noSizeChecksExplicit = true;
      noSubChecks = true;
      noSubChecksExplicit = true;
    } else if (a == "--size-checks") {
      noSizeChecks = false;
      noSizeChecksExplicit = true;
      // Safety checks imply each other (ADR-111): forcing SIZE traps back on
      // at -O2/-O3 brings SUBSCRIPTRANGE checks too, so condition-handling
      // tests that use --size-checks get the checks they expect.
      noSubChecks = false;
      noSubChecksExplicit = true;
    } else if (a == "--no-zero-divide")
      noZdivChecks = true;
    else if (a == "--no-conversion")
      noConvChecks = true;
    else if (a == "--no-subscript") {
      noSubChecks = true;
      noSubChecksExplicit = true;
    } else if (a == "--subscript-checks") {
      noSubChecks = false;
      noSubChecksExplicit = true;
    } else if (a == "--debug")
      debug = true;
    else if (a == "--version" || a == "-V")
      wantVersion = true;
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

  // Foreign object/archive inputs (C-interop): passed straight to the link
  // step, never preprocessed. Only .pli/.inc are PL/I sources.
  auto isPLISource = [](const std::string& f) {
    return (f.size() >= 4 &&
            (f.compare(f.size() - 4, 4, ".pli") == 0 || f.compare(f.size() - 4, 4, ".inc") == 0));
  };
  std::vector<std::string> pliInputs, foreignObjs;
  for (const std::string& in : inputs)
    (isPLISource(in) ? pliInputs : foreignObjs).push_back(in);
  if (!foreignObjs.empty() && (compileOnly || emitLLVM || syntaxOnly || print_hir)) {
    std::cerr
        << "plic: object inputs cannot be used with -c/-emit-llvm/-fsyntax-only/--print-hir\n";
    return 2;
  }
  const bool linkOnly = pliInputs.empty();

  // Upward filesystem walks (Zig-style) for include and library directories.
  // Start from the source root (cwd) and the plic executable directory; at each
  // ancestor check for lib/, lib/pli/ (libraries) and include/, inc/ (headers).
  fs::path exeDir = executablePath(argv[0]).parent_path();
  fs::path srcRoot = fs::current_path();
  std::set<std::string> seenPaths;
  std::vector<fs::path> srcIncDirs = searchUpwards(srcRoot, {"include", "inc"}, seenPaths);
  std::vector<fs::path> exeIncDirs = searchUpwards(exeDir, {"include", "inc"}, seenPaths);
  std::vector<fs::path> srcLibDirs = searchUpwards(srcRoot, {"lib", "lib/pli"}, seenPaths);
  std::vector<fs::path> exeLibDirs = searchUpwards(exeDir, {"lib", "lib/pli"}, seenPaths);
  // Discovered include dirs: source-root walk first, then exe walk.
  std::vector<fs::path> discoveredIncDirs = srcIncDirs;
  discoveredIncDirs.insert(discoveredIncDirs.end(), exeIncDirs.begin(), exeIncDirs.end());
  // Discovered library dirs (for -L): source-root walk first, then exe walk.
  std::vector<fs::path> discoveredLibDirs = srcLibDirs;
  discoveredLibDirs.insert(discoveredLibDirs.end(), exeLibDirs.begin(), exeLibDirs.end());
#if !defined(_WIN32)
  // Static system paths (Linux + macOS): deduplicated against walk results.
  const std::vector<std::string> staticIncDirs = {"/usr/include", "/usr/include/pli",
                                                  "/usr/local/include", "/usr/local/include/pli"};
  for (const std::string& d : staticIncDirs) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(d, ec);
    if (ec)
      continue;
    if (fs::is_directory(p, ec) && seenPaths.insert(p.string()).second)
      discoveredIncDirs.push_back(p);
  }
  const std::vector<std::string> staticLibDirs = {"/usr/lib", "/usr/lib/pli", "/usr/local/lib",
                                                  "/usr/local/lib/pli"};
  for (const std::string& d : staticLibDirs) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(d, ec);
    if (ec)
      continue;
    if (fs::is_directory(p, ec) && seenPaths.insert(p.string()).second)
      discoveredLibDirs.push_back(p);
  }
#endif
  // PLIC_LIB_PATH (colon-separated, like LIBRARY_PATH): searched after user -L,
  // before the upward-walk and static system library dirs.
  if (const char* env = std::getenv("PLIC_LIB_PATH")) {
    std::stringstream ss(env);
    std::string dir;
    while (std::getline(ss, dir, ':'))
      if (!dir.empty())
        linkArgs.push_back("-L" + dir);
  }
  // Append discovered library dirs as -L entries so they feed both the
  // in-process lld path and the clang fallback path.
  for (const fs::path& d : discoveredLibDirs)
    linkArgs.push_back("-L" + d.string());

  if (!runtimeExplicit && !runtimeLib.empty() && !fs::exists(runtimeLib)) {
    fs::path installed = PLIC_INSTALL_RUNTIME_LIB;
    if (installed.empty() || !fs::exists(installed)) {
      // Search the exe-dir upward walk for the runtime archive before the
      // hardcoded fallback. The archive name is platform-specific
      // (pli.lib on Windows, libpli.a elsewhere).
#ifdef _WIN32
      static const char* kRtLibNames[] = {"pli.lib", "libpli.a"};
#else
      static const char* kRtLibNames[] = {"libpli.a", "pli.lib"};
#endif
      bool found = false;
      for (const fs::path& d : exeLibDirs) {
        for (const char* n : kRtLibNames) {
          fs::path cand = d / n;
          if (fs::exists(cand)) {
            installed = cand;
            found = true;
            break;
          }
        }
        if (found)
          break;
      }
      if (!found)
        installed = executablePath(argv[0]).parent_path().parent_path() / "lib" / kRtLibNames[0];
    }
    if (fs::exists(installed))
      runtimeLib = installed.string();
  }
  // Bitcode runtime (OPTIMIZATION.md §10, P3): by default IRGen merges the
  // pli_* definitions out of runtime.bc into the MAIN unit (lib units keep
  // external references resolved by that one shared copy). An explicitly
  // named runtime.bc must exist (a typo is a user error); the baked-in
  // default falls back to the sectioned archive when missing (e.g. an older
  // build tree). --no-bitcode-runtime always selects the archive path.
  // Only search for runtime.bc when the bitcode runtime is actually used.
  if (useBitcode && !runtimeBcExplicit && !runtimeBc.empty() && !fs::exists(runtimeBc)) {
    fs::path installed = PLIC_INSTALL_RUNTIME_BC;
    if (installed.empty() || !fs::exists(installed)) {
      // Search the exe-dir upward walk for runtime.bc before the fallback.
      bool found = false;
      for (const fs::path& d : exeLibDirs) {
        fs::path cand = d / "runtime.bc";
        if (fs::exists(cand)) {
          installed = cand;
          found = true;
          break;
        }
      }
      if (!found)
        installed = executablePath(argv[0]).parent_path().parent_path() / "lib/runtime.bc";
    }
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
                << "; falling back to the embedded runtime\n";
    runtimeBc.clear();
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
  // Discovered include dirs from the upward walks and static system paths,
  // searched before the executable-relative default.
  for (const fs::path& d : discoveredIncDirs)
    preprocessor.addIncludeDir(d);
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
    for (const fs::path& d : discoveredIncDirs)
      std::cerr << "plic:   " << d << "\n";
    std::cerr << "plic:   " << defaultInc << "\n";
    std::cerr << "plic: library search dirs:\n";
    for (const std::string& la : linkArgs)
      if (la.rfind("-L", 0) == 0)
        std::cerr << "plic:   " << la << "\n";
  }

  // clang-matching guards: a single -o cannot name more than one output.
  const bool multi = pliInputs.size() > 1;
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
  // P1, Task 7: at -O2/-O3, FIXED overflow traps add a branch+trap per
  // arithmetic op, blocking the optimizer's overflow-narrowing in tight loops.
  // Disable them by default unless the user explicitly set --no-size-checks.
  if (!noSizeChecksExplicit && (optLevel == "-O2" || optLevel == "-O3"))
    noSizeChecks = true;
  // SUBSCRIPTRANGE bounds checks add branch + clamp + offset per array access;
  // disable by default at -O2/-O3 for performance (ADR-111).
  if (!noSubChecksExplicit && (optLevel == "-O2" || optLevel == "-O3"))
    noSubChecks = true;
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
  const bool semaCompileOnly = compileOnly || (pliInputs.size() + foreignObjs.size() > 1);
  // --keep-ll places each .ll next to the output (or the cwd with no -o).
  fs::path keepLLDir = output.empty() ? fs::path(".") : fs::path(output).parent_path();
  if (keepLLDir.empty())
    keepLLDir = ".";
  // Runtime-once fast path: one PL/I input becoming the executable directly.
  // Bitcode runtime: the MAIN unit embeds the pli_* definitions; the archive
  // always stays on the link line, where it is never pulled once the symbols
  // are defined by that embedded copy (runtime globals must stay shared).
  const bool linkBitcode = useBitcode && !emitLLVM && !runtimeBc.empty();
  // Explicit PGO/LTO must reach the backend clang verbatim on both steps, so
  // those builds always take the clang pipeline, never the in-process paths
  // (which take no backend flags).
  const bool needClangPipeline = pgoGenerate || !pgoUse.empty() || !ltoKind.empty();
  // Runtime-once fast path: one PL/I input becoming the executable directly.
  // Disabled for the archive path (!useBitcode: --no-bitcode-runtime links
  // libpli.a instead) and for explicit PGO/LTO (clang pipeline above).
  const bool singleModuleFinalLink = !compileOnly && !terminalMode && pliInputs.size() == 1 &&
                                     foreignObjs.empty() && useBitcode && !needClangPipeline;
  std::vector<std::string> objs = foreignObjs;
  for (size_t i = 0; i < pliInputs.size(); ++i) {
    std::string outObj;
    if (!compileOne(preprocessor, pliInputs[i], output, triple, clangPath, sysparm, sysparmExplicit,
                    compileOnly, semaCompileOnly, emitLLVM, syntaxOnly, print_hir, keepLL, verbose,
                    noSizeChecks, noZdivChecks, noConvChecks, noSubChecks, optLevel, backendFlags,
                    keepLLDir, (int)i, &outObj, runtimeBc, linkBitcode, singleModuleFinalLink,
                    needClangPipeline))
      return 1;
    if (!compileOnly)
      objs.push_back(outObj);
  }
  if (terminalMode)
    return 0;
  if (linkOnly && objs.empty()) {
    std::cerr << "plic: no input objects to link\n";
    return 2;
  }

  // --- link step ----------------------------------------------------------
  // In-process link first (no subprocess); the clang pipeline below is the
  // fallback until lld ships everywhere (review §1: brew llvm has no lld).
  // Link the per-file objects with libpli. With the bitcode runtime the MAIN
  // object already defines the pli_* symbols, so the archive's members are
  // simply never pulled (no duplicates); --no-bitcode-runtime compiles every
  // unit against the archive as before. Drop unreferenced runtime sections
  // (the archive is sectioned, ADR-079); multitasking (QR2.8) runs on pthreads.
  if (output.empty())
    output = "a.out";
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();

#if PLIC_HAVE_LLD
  // Explicit PGO/LTO (needClangPipeline) and the archive path (!useBitcode)
  // always use the clang pipeline below so backendFlags reach the link step.
  if (!needClangPipeline && useBitcode) {
    llvm::Triple llt(triple);
    const plic::TargetDesc& desc = plic::getTargetDesc(llt);
    std::string tmErr;
    if (auto tm = plic::createTargetMachine(triple, optLevel, tmErr)) {
      // With the file-based bitcode runtime (linkBitcode) the MAIN object
      // already defines every pli_* symbol, so no separate runtime object
      // is emitted (it would duplicate them). Otherwise multi-input links
      // carry the runtime as its own object (runtime-once); single-module
      // links already carry it at BC level (singleModuleFinalLink above).
      std::string rtObj;
      bool haveRtObj = false;
      if (!singleModuleFinalLink && !linkBitcode) {
        rtObj =
            (fs::temp_directory_path() / ("libpli-" + std::to_string(getpid()) + ".o")).string();
        std::error_code ec;
        llvm::raw_fd_ostream os(rtObj, ec, llvm::sys::fs::OF_None);
        std::string rtErr;
        haveRtObj = !ec && plic::emitRuntimeObject(*tm, llt, os, rtErr);
        if (!haveRtObj && verbose)
          std::cerr << "plic: runtime object failed (" << rtErr << "), using clang\n";
      }
      if (singleModuleFinalLink || haveRtObj || linkBitcode) {
        // Host startup/sysroot bridge (transitional; bundling is follow-up).
        auto runCap = [](const std::string& cmd) {
          std::string out;
          if (FILE* f = popen(cmd.c_str(), "r")) {
            char buf[256];
            while (fgets(buf, sizeof buf, f))
              out += buf;
            pclose(f);
          }
          while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
          return out;
        };
        std::vector<std::string> argStore = {"ld.lld"};
        if (llt.getObjectFormat() == llvm::Triple::MachO) {
          if (const char* sdk = nullptr; true) {
            std::string sdkPath = runCap("xcrun --show-sdk-path 2>/dev/null");
            argStore.push_back("-arch");
            argStore.push_back(std::string(llt.getArchName()));
            if (!sdkPath.empty()) {
              argStore.push_back("-syslibroot");
              argStore.push_back(sdkPath);
            }
            (void)sdk;
          }
        } else if (llt.getObjectFormat() == llvm::Triple::ELF) {
          // PIE startup (matches clang): Scrt1.o + crtbeginS.o/crtendS.o with
          // -pie and an explicit interpreter, else a non-PIE EXEC segfaults.
          bool wantPIE = true;
          for (const std::string& la : linkArgs) {
            std::string f = la.rfind("-Wl,", 0) == 0 ? la.substr(4) : la;
            if (f == "-static" || f == "-static-pie" || f == "-shared" || f == "-no-pie" ||
                f == "-nopie" || f == "-no-pic")
              wantPIE = false;
          }
          auto resolveCrt = [&](const char* primary, const char* fallback) {
            std::string p =
                runCap(shellQuote(clangPath) + " -print-file-name=" + primary + " 2>/dev/null");
            if (!p.empty() && p != primary)
              return p;
            if (fallback) {
              p = runCap(shellQuote(clangPath) + " -print-file-name=" + fallback + " 2>/dev/null");
              if (!p.empty() && p != fallback)
                return p;
            }
            return std::string(primary);
          };
          if (wantPIE) {
            // Dynamic linker per arch (lld needs it explicit or no INTERP).
            std::string interp;
            switch (llt.getArch()) {
            case llvm::Triple::aarch64:
              interp = "/lib/ld-linux-aarch64.so.1";
              break;
            case llvm::Triple::x86_64:
              interp = "/lib64/ld-linux-x86-64.so.2";
              break;
            case llvm::Triple::arm:
            case llvm::Triple::armeb:
            case llvm::Triple::thumb:
              interp = "/lib/ld-linux-armhf.so.3";
              break;
            case llvm::Triple::riscv64:
              interp = "/lib/ld-linux-riscv64-lp64d.so.1";
              break;
            default:
              break;
            }
            if (!interp.empty() && !fs::exists(interp)) {
              if (llt.getArch() == llvm::Triple::x86_64 && fs::exists("/lib/ld-linux-x86-64.so.2"))
                interp = "/lib/ld-linux-x86-64.so.2";
              else if (verbose)
                std::cerr << "plic: interpreter " << interp << " not found\n";
            }
            argStore.push_back("-pie");
            argStore.push_back("--eh-frame-hdr");
            argStore.push_back("--hash-style=gnu");
            argStore.push_back("--build-id");
            if (!interp.empty()) {
              argStore.push_back("-dynamic-linker");
              argStore.push_back(interp);
            }
          }
          argStore.push_back(resolveCrt(wantPIE ? "Scrt1.o" : "crt1.o", "crt1.o"));
          argStore.push_back(resolveCrt("crti.o", nullptr));
          argStore.push_back(resolveCrt(wantPIE ? "crtbeginS.o" : "crtbegin.o", "crtbegin.o"));
          // Bridge clang's library search dirs so lld can find -lm, -lc, etc.
          std::string sd = runCap(shellQuote(clangPath) + " -print-search-dirs 2>/dev/null");
          if (auto pos = sd.find("libraries:"); pos != std::string::npos) {
            std::string line = sd.substr(pos);
            if (auto nl = line.find('\n'); nl != std::string::npos)
              line = line.substr(0, nl);
            if (auto eq = line.find('='); eq != std::string::npos)
              line = line.substr(eq + 1);
            std::stringstream ss(line);
            std::string d;
            while (std::getline(ss, d, ':')) {
              if (!d.empty())
                argStore.push_back("-L" + d);
            }
          }
          std::string sysroot = runCap(shellQuote(clangPath) + " -print-sysroot 2>/dev/null");
          if (!sysroot.empty() && sysroot != "/" &&
              sysroot.find("unknown argument") == std::string::npos &&
              sysroot.find("no input files") == std::string::npos) {
            argStore.push_back("--sysroot");
            argStore.push_back(sysroot);
          }
        }
        for (const std::string& o : objs)
          argStore.push_back(o);
        if (haveRtObj)
          argStore.push_back(rtObj);
        if (llt.getObjectFormat() == llvm::Triple::ELF) {
          bool wantPIE = true;
          for (const std::string& la : linkArgs) {
            std::string f = la.rfind("-Wl,", 0) == 0 ? la.substr(4) : la;
            if (f == "-static" || f == "-static-pie" || f == "-shared" || f == "-no-pie" ||
                f == "-nopie" || f == "-no-pic")
              wantPIE = false;
          }
          auto resolveCrtTail = [&](const char* primary, const char* fallback) {
            std::string p =
                runCap(shellQuote(clangPath) + " -print-file-name=" + primary + " 2>/dev/null");
            if (!p.empty() && p != primary)
              return p;
            if (fallback) {
              p = runCap(shellQuote(clangPath) + " -print-file-name=" + fallback + " 2>/dev/null");
              if (!p.empty() && p != fallback)
                return p;
            }
            return std::string(primary);
          };
          argStore.push_back(resolveCrtTail(wantPIE ? "crtendS.o" : "crtend.o", "crtend.o"));
          argStore.push_back(resolveCrtTail("crtn.o", nullptr));
          // GCC unwinding helpers (matches clang: -lgcc --as-needed -lgcc_s).
          argStore.push_back("-lgcc");
          argStore.push_back("--as-needed");
          argStore.push_back("-lgcc_s");
          argStore.push_back("--no-as-needed");
        }
        for (const std::string& s : desc.systemLibs)
          argStore.push_back(s);
        if (llt.getObjectFormat() == llvm::Triple::ELF) {
          // Trailing helpers (matches clang: -lc -lgcc --as-needed -lgcc_s).
          argStore.push_back("-lgcc");
          argStore.push_back("--as-needed");
          argStore.push_back("-lgcc_s");
          argStore.push_back("--no-as-needed");
        }
        for (const std::string& f : desc.linkerFlags)
          argStore.push_back(f);
        for (const std::string& la : linkArgs) {
          if (la.rfind("-Wl,", 0) == 0) {
            // lld takes the flag directly, but ELF lld uses -no-pie (hyphen),
            // not Mach-O's -no_pie (underscore).
            std::string f = la.substr(4);
            if (llt.getObjectFormat() == llvm::Triple::ELF && f == "-no_pie")
              f = "-no-pie";
            argStore.push_back(f);
          } else if (la.rfind("-fuse-ld=", 0) == 0)
            continue; // clang-driver-only
          else
            argStore.push_back(la);
        }
        if (llt.getObjectFormat() == llvm::Triple::COFF) {
          argStore.push_back("/out:" + output);
        } else {
          argStore.push_back("-o");
          argStore.push_back(output);
        }
        std::vector<const char*> argv;
        for (const std::string& a : argStore)
          argv.push_back(a.c_str());
        std::string linkErr;
        if (verbose) {
          std::cerr << "+";
          for (const std::string& a : argStore)
            std::cerr << " " << a;
          std::cerr << "\n";
        }
        if (plic::linkExecutable(argv, (int)llt.getObjectFormat(), linkErr)) {
          for (size_t i = foreignObjs.size(); i < objs.size(); ++i) {
            std::error_code ec;
            fs::remove(objs[i], ec);
          }
          if (haveRtObj) {
            std::error_code ec;
            fs::remove(rtObj, ec);
          }
          return 0;
        }
        if (verbose)
          std::cerr << "plic: in-process link failed (" << linkErr << "), using clang\n";
        if (haveRtObj) {
          std::error_code ec;
          fs::remove(rtObj, ec);
        }
      }
    } else if (verbose) {
      std::cerr << "plic: no target (" << tmErr << "), using clang\n";
    }
  }
#endif

  // Fallback: link the per-file objects with libpli via the backend clang.
  // Drop unreferenced runtime sections (the archive is sectioned, ADR-079);
  // multitasking (QR2.8) runs on pthreads.
  // On Windows the driver defaults to the static CRT (LIBCMT) while pli.lib
  // is built with the dynamic CRT (/MD): drop the static default lib and use
  // the dynamic one, otherwise the link fails with LNK4098/LNK2019.
#ifdef _WIN32
  const char* linkCrtFlag = "-Xlinker /NODEFAULTLIB:libcmt.lib -Xlinker /DEFAULTLIB:msvcrt.lib ";
#else
  const char* linkCrtFlag = "";
#endif
#ifdef _WIN32
  // LTO objects are LLVM bitcode, which MSVC link.exe cannot read (LNK1107):
  // route LTO links through lld-link, which handles bitcode natively.
  // Non-LTO links keep the default (MSVC link) behavior.
  const char* linkLldFlag = !ltoKind.empty() ? "-fuse-ld=lld " : "";
#else
  const char* linkLldFlag = "";
#endif
  std::string cmd = shellQuote(clangPath) + " " + linkCrtFlag + linkLldFlag +
                    "-Wno-override-module " + optLevel + backendFlags;
#ifdef __APPLE__
  cmd += " -Wl,-dead_strip";
#elif defined(_WIN32)
  // COFF: no dead-strip / pthread flags via clang driver on Windows.
#else
  cmd += " -Wl,--gc-sections";
#endif
#ifndef _WIN32
  cmd += " -pthread";
#endif
  for (const std::string& o : objs)
    cmd += " " + shellQuote(o);
  if (!runtimeLib.empty())
    cmd += " " + shellQuote(runtimeLib);
  for (const std::string& la : linkArgs)
    cmd += " " + la; // link flags
#if !defined(_WIN32) && !defined(__APPLE__)
  // libpli's mathport needs libm (after the archive: ld resolves left to
  // right); macOS bundles it in libSystem.
  cmd += " -lm";
#endif
  cmd += " -o " + shellQuote(output);
  if (verbose)
    std::cerr << "+ " << cmd << "\n";
  int rc = system(cmd.c_str());
  // Remove the temp objects produced for this link (never foreign inputs).
  for (size_t i = foreignObjs.size(); i < objs.size(); ++i) {
    std::error_code ec;
    fs::remove(objs[i], ec);
  }
  if (rc != 0) {
    std::cerr << "plic: backend failed\n";
    return 1;
  }
  return 0;
}
