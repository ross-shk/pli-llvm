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
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "codegen.h"
#include "embedded_runtime.h"
#include "target.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#ifndef PLIC_RUNTIME_LIB
#define PLIC_RUNTIME_LIB ""
#endif

#ifndef PLIC_INSTALL_RUNTIME_LIB
#define PLIC_INSTALL_RUNTIME_LIB ""
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
         "  --clang <path>   clang to assemble/link the IR (default: LLVM's clang)\n"
         "  --triple <t>     target triple (default: host triple)\n"
         "  --sysparm <s>    value returned by the SYSPARM builtin (rule (123))\n"
         "  -L <dir>         add a library search path to the link step\n"
         "  -I <dir>         add a %INCLUDE search directory (repeatable; -I<dir> too)\n"
         "  -l<lib>          link a library (e.g. -lm) on the link step\n"
         "  -Wl,<flag>       pass a raw flag to the linker (repeatable)\n"
         "  --linker <ld>    select the linker via -fuse-ld=<ld>\n"
         "  -shared -static  produce a shared / static binary\n"
         "  --extra <a,b,c>  comma-separated extra backend args appended to the link\n"
         "  --explain <n>    print TR 25.084 rule (n)'s production and exit\n"
         "  --version        print plic + LLVM + host triple and exit\n"
         "  -v               show the sub-commands being run\n"
         "  -h, --help       this message\n";
}

static void printVersion() {
  // Version/build info embedded at build time (Go/Zig style).
  std::cout << "plic " << PLIC_VERSION << " (llvm " << PLIC_LLVM_VERSION << ", "
            << llvm::sys::getDefaultTargetTriple() << ", runtime " << plic::embeddedLibPLISize()
            << " bytes)\n";
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
                       std::string* outObj, bool linkRuntimeIn = false) {
  // Use macosx15.0 for bitcode compatibility; SME features disabled in IRGen.
  if (triple.empty())
    triple = "arm64-apple-macosx15.0";
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
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();

  fs::path inPath(input);
  std::string base = inPath.stem().string();

  if (emitLLVM) {
    IRGen irgen(diags, sema, triple, noSizeChecks);
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
  {
    IRGen ipg(diags, sema, triple, noSizeChecks);
    if (auto om = ipg.takeModule(hir)) {
      bool rtOk = true;
#if PLIC_HAVE_LLD
      // Single-module fast path: runtime linked at BC level (never `-c`).
      if (linkRuntimeIn && !compileOnly) {
        std::string rtErr;
        rtOk = plic::linkEmbeddedLibPLI(*om->mod, rtErr);
        if (!rtOk)
          std::cerr << "plic: " << rtErr << "\n";
      }
#endif
      if (rtOk) {
        // Create TargetMachine using darwin triple (macOS 15 = darwin 24)
        // to avoid SME features (zcm/zcz) which are enabled by default for
        // macosx15 in LLVM 23 but not actually supported.
        std::string tmErr;
        std::string tmTriple = "arm64-apple-darwin24.0";
        if (auto tm = plic::createTargetMachine(tmTriple, optLevel, tmErr)) {
          // The IRGen layout is approximate; the TargetMachine owns the truth
          // (llc behaviour — the clang fallback used -Wno-override-module).
          om->mod->setDataLayout(tm->createDataLayout());
          // Also set module target triple to darwin24 to ensure consistent
          // feature handling during codegen (module triple affects some defaults).
          om->mod->setTargetTriple(llvm::Triple("arm64-apple-darwin24.0"));
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
  IRGen irgen(diags, sema, triple, noSizeChecks);
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
  std::string output, runtimeLib = PLIC_RUNTIME_LIB, triple;
  std::string clangPath = PLIC_CLANG;
  std::string sysparm;
  bool sysparmExplicit = false;
  std::string optLevel = "-O2";
  std::vector<std::string> linkArgs;    // extra args appended to the link step
  std::vector<std::string> includeDirs; // %INCLUDE search dirs (-I, repeatable)
  bool emitLLVM = false, syntaxOnly = false, keepLL = false, verbose = false, compileOnly = false;
  bool runtimeExplicit = false, print_hir = false, release = false, debug = false;
  bool noSizeChecks = false, wantVersion = false;
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
    } else if (a == "--clang")
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
    else if (a == "--no-size-checks")
      noSizeChecks = true;
    else if (a == "--debug")
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

  // `plic version` / `--version`: no input file needed.
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

  if (!runtimeExplicit && !runtimeLib.empty() && !fs::exists(runtimeLib)) {
    fs::path installed = PLIC_INSTALL_RUNTIME_LIB;
    if (installed.empty() || !fs::exists(installed))
      installed = executablePath(argv[0]).parent_path().parent_path() / "lib/libpli.a";
    if (fs::exists(installed))
      runtimeLib = installed.string();
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
  const bool singleModuleFinalLink =
      !compileOnly && !terminalMode && pliInputs.size() == 1 && foreignObjs.empty();
  std::vector<std::string> objs = foreignObjs;
  for (size_t i = 0; i < pliInputs.size(); ++i) {
    std::string outObj;
    if (!compileOne(preprocessor, pliInputs[i], output, triple, clangPath, sysparm, sysparmExplicit,
                    compileOnly, semaCompileOnly, emitLLVM, syntaxOnly, print_hir, keepLL, verbose,
                    noSizeChecks, optLevel, backendFlags, keepLLDir, (int)i, &outObj,
                    singleModuleFinalLink))
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
  if (output.empty())
    output = "a.out";
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();

#if PLIC_HAVE_LLD
  {
    llvm::Triple llt(triple);
    const plic::TargetDesc& desc = plic::getTargetDesc(llt);
    std::string tmErr;
    if (auto tm = plic::createTargetMachine(triple, optLevel, tmErr)) {
      // Multi-input: runtime as its own object (runtime-once). Single-module
      // links already carry it at BC level (singleModuleFinalLink above).
      std::string rtObj;
      bool haveRtObj = false;
      if (!singleModuleFinalLink) {
        rtObj =
            (fs::temp_directory_path() / ("libpli-" + std::to_string(getpid()) + ".o")).string();
        std::error_code ec;
        llvm::raw_fd_ostream os(rtObj, ec, llvm::sys::fs::OF_None);
        std::string rtErr;
        haveRtObj = !ec && plic::emitRuntimeObject(*tm, os, rtErr);
        if (!haveRtObj && verbose)
          std::cerr << "plic: runtime object failed (" << rtErr << "), using clang\n";
      }
      if (singleModuleFinalLink || haveRtObj) {
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
          for (const char* crt : {"crt1.o", "crti.o"}) {
            std::string p =
                runCap(shellQuote(clangPath) + " -print-file-name=" + crt + " 2>/dev/null");
            if (!p.empty() && p != crt)
              argStore.push_back(p);
          }
          std::string sysroot = runCap(shellQuote(clangPath) + " -print-sysroot 2>/dev/null");
          if (!sysroot.empty() && sysroot != "/") {
            argStore.push_back("-sysroot");
            argStore.push_back(sysroot);
          }
        }
        for (const std::string& o : objs)
          argStore.push_back(o);
        if (haveRtObj)
          argStore.push_back(rtObj);
        if (llt.getObjectFormat() == llvm::Triple::ELF)
          argStore.push_back("crtn.o");
        for (const std::string& s : desc.systemLibs)
          argStore.push_back(s);
        for (const std::string& f : desc.linkerFlags)
          argStore.push_back(f);
        for (const std::string& la : linkArgs) {
          if (la.rfind("-Wl,", 0) == 0)
            argStore.push_back(la.substr(4)); // lld takes the flag directly
          else if (la.rfind("-fuse-ld=", 0) == 0)
            continue; // clang-driver-only
          else
            argStore.push_back(la);
        }
        argStore.push_back("-o");
        argStore.push_back(output);
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
