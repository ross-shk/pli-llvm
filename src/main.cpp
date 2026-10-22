// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// main.cpp — pli driver.
//
// Pipeline: source -> preprocessor -> lexer -> parser -> sema -> LLVM IR ->
// in-process object (integrated assembler) -> in-process lld link with the
// embedded runtime. No subprocesses (ADR-225). See docs/ARCHITECTURE.md.
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
#if defined(PLI_ENABLE_MLIR)
#include "mlir/PliDialect.h"
#endif
#include "sema.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "codegen.h"
#include "embedded_runtime.h"
#include "embedded_sysroot.h"
#include "hostcrt.h"
#include "pgo.h"
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

#ifndef PLI_RUNTIME_LIB
#define PLI_RUNTIME_LIB ""
#endif

// Bitcode runtime source (OPTIMIZATION.md §10, P3): always the embedded
// blob. IRGen merges the pli_* definitions from it into the MAIN unit so
// clang sees real bodies; --no-bitcode-runtime keeps every unit's references
// external for the link step instead.

// Version and LLVM identity baked in at build time (self-contained milestone:
// `pli version` reports the toolchain without invoking sub-tools).
#ifndef PLI_VERSION
#define PLI_VERSION "unknown"
#endif

#ifndef PLI_LLVM_VERSION
#define PLI_LLVM_VERSION LLVM_VERSION_STRING
#endif

// Hidden escape hatch (undocumented, ADR-225): --clang is still parsed into
// `clangPath` but nothing invokes it — compile and link are fully in-process.
// Reserved for debugging only.

#ifndef PLI_VERSION
#define PLI_VERSION "unknown"
#endif

#ifndef PLI_LLVM_VERSION
#define PLI_LLVM_VERSION LLVM_VERSION_STRING
#endif

// In-process lld link gate (review S1): 1 when the build linked lld
// (CMake defines it); the Makefile build leaves it 0 and uses clang.
#ifndef PLI_HAVE_LLD
#define PLI_HAVE_LLD 0
#endif

namespace fs = std::filesystem;

static void usage() {
  std::cout
      << "pli — PL/I compiler (LLVM backend)\n"
         "\n"
         "usage: pli [options] file.pli...\n"
         "\n"
         "Multiple inputs compile like cc: each file becomes an independent\n"
         "relocatable object and they are linked together with the embedded\n"
         "runtime into one\n"
         "output (-o names it). -c writes <base>.o per input and does not link.\n"
         "\n"
         "options:\n"
         "  -o <file>        output file (default: a.out, or <base>.ll with -emit-llvm)\n"
         "                   (single input only with -emit-llvm/--print-hir/-fsyntax-only,\n"
         "                   and with -c when more than one input is given)\n"
         "  -c               compile each input to a relocatable object (no linking)\n"
         "  -emit-llvm       write LLVM IR per input and stop\n"
         "  --print-hir      lower to HIR and print it per input, then stop\n"
         "  --print-facts    print the compiler's PL/I-specific decisions\n"
         "                   (lowering policy, check state, built-in paths)\n"
         "                   and stop (ADR-260)\n"
         "  -fsyntax-only    parse and analyse each input only\n"
         "  -O0 -O1 -O2 -O3  optimization level passed to the LLVM pipeline (default -O2)\n"
         "  --no-memory-effects  do not infer procedure memory effects (ADR-264);\n"
         "                   every call keeps LLVM's default effects\n"
         "  --warn-unset     warn when an AUTOMATIC variable may be used before\n"
         "                   it is set (ADR-266; off by default)\n"
         "  --remark-library-calls  remark when a built-in hands a conversion to\n"
         "                   the library routine (ADR-254; off by default)\n"
         "  --emit-effects=FILE  publish the effects this module's external\n"
         "                   procedures promise, for another module to use\n"
         "  --effects=FILE  use the effects another module published (ADR-267)\n"
         "  --no-size-checks elide FIXED overflow traps (default at -O2/-O3;\n"
         "                   cf. (NOSIZE), ADR-111)\n"
         "  --size-checks    force FIXED overflow traps (overrides -O2/-O3 default)\n"
         "  --no-zero-divide elide ZERODIVIDE traps program-wide (cf. (NOZERODIVIDE), ADR-112)\n"
         "  --no-conversion   elide CONVERSION traps program-wide (cf. (NOCONVERSION), ADR-170)\n"
         "  --no-subscript    elide SUBSCRIPTRANGE checks (default at -O2/-O3;\n"
         "                   cf. (NOSUBSCRIPTRANGE), ADR-111)\n"
         "  --subscript-checks  force SUBSCRIPTRANGE checks (overrides -O2/-O3 default)\n"
  "  --release        maximum optimization + stripped binary (minimal size)\n"
  "  --debug          no optimization (-O0; no DWARF emitted yet)\n"
         "  --keep-ll        keep the intermediate .ll next to the output\n"
         "  --runtime <lib>  path to an external runtime archive (default: none;\n"
         "                   the embedded runtime is used instead)\n"
         "  --no-bitcode-runtime\n"
         "                   keep pli_* references external and resolve them at link\n"
         "                   from the embedded runtime instead of merging the\n"
         "                   definitions into the MAIN unit\n"
  "  --rt-platform <f>\n"
  "                   path to a custom platform bitcode file implementing\n"
  "                   runtime/pli_rt_plat.h (default: none; the embedded\n"
  "                   pli_rtplat.bc is merged instead)\n"
  "  --flto=thin|full   whole-program optimization at link (in-process lld)\n"
  "  --fprofile-generate  instrument for PGO (run the binary, it writes a profile)\n"
  "  --fprofile-use=<f>   rebuild using that profile (.profraw or .profdata)\n"
  "  --triple <t>     target triple (default: host triple, in-process)\n"
          "  --march=native   opt-in: use host CPU features (default: generic;\n"
          "                   CI/baselines always use generic; local hot runs only)\n"
         "  --sysparm <s>    value returned by the SYSPARM builtin (rule (123))\n"
         "  -L <dir>         add a library search path to the link step\n"
         "  -I <dir>         add a %INCLUDE search directory (repeatable; -I<dir> too)\n"
         "  -l<lib>          link a library (e.g. -lm) on the link step\n"
         "  -Wl,<flag>       pass a raw flag to the linker (repeatable)\n"
         "  --linker <ld>    select the linker via -fuse-ld=<ld>\n"
         "  -shared -static  produce a shared / static binary\n"
         "  --extra <a,b,c>  comma-separated extra backend args appended to the link\n"
         "  --explain <n>    print TR 25.084 rule (n)'s production and exit\n"
         "  --version        print pli + LLVM versions and the host triple, then exit\n"
         "  -v               show the sub-commands being run\n"
         "  -h, --help       this message\n"
         "\n"
         "  experimental (design-docs/mlir-runtime-migration-plan.md):\n"
         "  --experimental-lowering=<mode>\n"
         "                   select lowering strategy for pli_* operations:\n"
         "                   auto (default), runtime, llvm, or mlir\n"
         "  --experimental-lowering=<op>:<mode>\n"
         "                   override one operation, leave others on auto\n"
         "  -emit-mlir       emit the pli MLIR helper module (MLIR-enabled builds only)\n"
         "\n"
         "  environment:\n"
         "  $PLI_INCLUDE_PATH  colon-separated %INCLUDE search dirs (after -I)\n"
         "  $PLI_LIB_PATH      colon-separated library search dirs (after -L)\n"
         "  $PLI_SYSPARM       value returned by the SYSPARM builtin ((123))\n"
         "\n"
         "  library/include discovery:\n"
         "  pli walks upward from the source root (cwd) and the pli executable\n"
         "  directory, adding lib/ and lib/pli/ to library search paths and\n"
         "  include/ and inc/ to %INCLUDE search paths at each ancestor. On\n"
         "  Linux/macOS, /usr/lib, /usr/local/lib and the matching include paths\n"
         "  are added as static fallbacks.\n"
         "\n"
         "  pli version     same as --version (Go/Zig style)\n";
}

// Go/Zig-style toolchain report: no subprocess, all baked in or in-process.
static void printVersion() {
  // Version/build info embedded at build time (Go/Zig style).
  std::cout << "pli " << PLI_VERSION << " (LLVM " << PLI_LLVM_VERSION << ", "
            << llvm::sys::getDefaultTargetTriple() << ", runtime " << pli::embeddedLibPLISize()
            << " bytes, sysroot " << pli::embeddedSysrootSize() << " bytes)\n";
}

// The platform's path-list separator: ';' on Windows, ':' elsewhere (the
// PATH convention). A hard-coded ':' splits a Windows path list into
// fragments that name no directory.
static char pathListSeparator() {
#ifdef _WIN32
  return ';';
#else
  return ':';
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
  const char sep = pathListSeparator();
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

// --- WS-A2 (ADR-267): the published-effects file ---------------------------
//
// A module publishes what its external procedures promise, so a module that
// calls them can attach that promise to its declarations: the caller's
// optimizer cannot see the callee's body, so this description is all it has.

// Read the effects another module published. A malformed line is an error, not
// a silent skip: a promise that is dropped must not look like one that was
// never made. The header names the format version; a reader that does not
// know it refuses the file rather than guess at it.
static bool readPublishedEffects(const std::string& path,
                                  std::map<std::string, pli::PublishedEffect>& out) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "pli: cannot read " << path << "\n";
    return false;
  }
  std::string line;
  int lineno = 0;
  bool sawHeader = false;
  while (std::getline(in, line)) {
    ++lineno;
    size_t start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos)
      continue; // blank
    if (!sawHeader) {
      sawHeader = true;
      if (line.compare(0, std::string(pli::kPublishedEffectsHeader).size(),
                       pli::kPublishedEffectsHeader) != 0) {
        std::cerr << "pli: " << path << ": not a published-effects file (expected the header \""
                  << pli::kPublishedEffectsHeader << "\")\n";
        return false;
      }
      continue;
    }
    if (line[start] == '#')
      continue; // comment
    std::string name, digest;
    pli::ProcEffects e;
    if (!pli::parseEffectLine(line, name, e, digest)) {
      std::cerr << "pli: " << path << ":" << lineno << ": malformed effect line\n";
      return false;
    }
    pli::PublishedEffect pub;
    pub.effects = e;
    pub.digest = digest;
    out[name] = pub;
  }
  if (!sawHeader) {
    std::cerr << "pli: " << path << ": empty published-effects file\n";
    return false;
  }
  return true;
}

// Write what this module's external procedures promise.
static bool writePublishedEffects(const std::string& path, const HProgram& hir,
                                  const std::map<const HProc*, pli::ProcEffects>& effects,
                                  const IRGen& irgen) {
  std::ofstream os(path);
  if (!os) {
    std::cerr << "pli: cannot write " << path << "\n";
    return false;
  }
  os << pli::kPublishedEffectsHeader << "\n";
  for (const auto& p : hir.procs) {
    if (!p || !p->isExternal)
      continue; // only what another module can call
    auto it = effects.find(p.get());
    if (it == effects.end())
      continue;
    os << pli::formatEffectLine(p->name, it->second, irgen.definitionDigest(p.get())) << "\n";
  }
  return true;
}

// Compile one translation unit through the full pipeline (preprocessor ->
// lexer -> parser -> sema -> HIR -> IRGen) and assemble it to a relocatable
// object with the backend clang. Multi-input drivers call this once per file.
// Per-file terminal modes (-emit-llvm, --print-hir, -fsyntax-only) apply to
// this unit and return without linking. In link mode `outObj` receives the
// object path for the caller to link. Returns false on any failure.
static bool compileOne(Preprocessor& preprocessor, const std::string& input, std::string& output,
                       std::string& triple, const std::string& sysparm, bool sysparmExplicit,
                       bool compileOnly, bool semaCompileOnly, bool emitLLVM, bool emitMlir,
                       bool syntaxOnly, bool print_hir, bool print_facts, bool keepLL, bool verbose,
                       bool noSizeChecks, bool noZdivChecks, bool noConvChecks, bool noSubChecks,
                       const std::string& optLevel, const fs::path& keepLLDir, int fileIndex,
                       std::string* outObj, bool linkBitcode, LowerMode lowerMode = LowerMode::Auto,
                       const std::map<std::string, LowerMode>& perOpOverrides = {},
                       bool linkRuntimeIn = false, const std::string& ltoKind = "",
                       pli::PgoMode pgoMode = pli::PgoMode::None, const std::string& pgoUse = "",
                       bool marchNative = false, const std::string& platformBc = "",
                       bool noMemoryEffects = false, bool warnUnset = false,
                       const std::string& effectsFile = "",
                       const std::string& emitEffectsFile = "",
                       bool remarkLibraryCalls = false) {
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
  // Map flat (post-preprocessor) lines back to the original source file:line so
  // diagnostics stay accurate across %include (ADR-219).
  diags.setLineMap(&preprocessor.lineMap());
  diags.setFileSources(&preprocessor.fileSrc());
  diags.setRootPath(preprocessor.rootCanonical());

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
  // SYSPARM: explicit --sysparm wins over $PLI_SYSPARM (rule (123)).
  std::string effSysparm = sysparm;
  if (!sysparmExplicit)
    if (const char* env = std::getenv("PLI_SYSPARM"))
      effSysparm = env;
  sema.setSysparm(effSysparm);
  sema.setWarnUnset(warnUnset);
  // WS-A2 (ADR-267): the effects another module published for the external
  // procedures this unit calls.
  std::map<std::string, pli::PublishedEffect> published;
  if (!effectsFile.empty() && !readPublishedEffects(effectsFile, published))
    return false;
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

  // `--print-facts` lowers once and reports the decisions the compiler made
  // (ADR-260), then stops — no object, no link.
  if (print_facts) {
    IRGen irgen(diags, sema, triple, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks,
                false, lowerMode, perOpOverrides, "", noMemoryEffects);
    irgen.setPublishedEffects(published);
    irgen.setRemarkLibraryCalls(remarkLibraryCalls);
    irgen.run(hir);
    if (!diags.ok())
      return false;
    std::cout << irgen.facts().report();
    return true;
  }

  // --- code generation ---------------------------------------------------
  // (triple defaulted above: LLVM itself, in-process.)

  fs::path inPath(input);
  std::string base = inPath.stem().string();

  if (emitLLVM || emitMlir) {
    IRGen irgen(diags, sema, triple, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks,
                false, lowerMode, perOpOverrides, "", noMemoryEffects);
    irgen.setPublishedEffects(published);
    irgen.setRemarkLibraryCalls(remarkLibraryCalls);
    std::string ir = irgen.run(hir);
    if (!diags.ok())
      return false;
    // -o is restricted to a single input (checked in main); otherwise each
    // input writes its own <base>.ll.
    std::string dest = output.empty() ? base + ".ll" : output;
    if (emitMlir) {
#if defined(PLI_ENABLE_MLIR)
      // P2: -emit-mlir prints the canary MLIR helper module, not PL/I IR.
      // The canary exercises the full MLIR pipeline without lowering W1-W5 ops.
      if (!mlir::pli::runMlirCanary(llvm::outs()))
        return false;
      if (verbose)
        std::cerr << "pli: -emit-mlir emitted canary module (synthetic)\n";
      return true;
#else
      std::cerr << "pli: -emit-mlir requires an MLIR-enabled build (-DPLI_ENABLE_MLIR=ON)\n";
      return false;
#endif
    }
    std::ofstream os(dest, std::ios::binary);
    if (!os) {
      std::cerr << "pli: cannot write " << dest << "\n";
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
  else {
    // Link mode: a unique temp object. The name is random and the file is
    // created exclusively (O_EXCL), so a name planted in the temp dir cannot
    // be mistaken for ours and redirect the write. A predictable
    // "<tmp>/<base>-<pid>-<n>.o" could.
    llvm::SmallString<128> tmpObj;
    std::error_code fec = llvm::sys::fs::createUniqueFile(
        (fs::temp_directory_path() /
         (base + "-" + std::to_string(fileIndex) + "-%%%%%%.o")).string(),
        tmpObj);
    if (fec) {
      std::cerr << "pli: cannot create a temporary object\n";
      return false;
    }
    objPath = fs::path(tmpObj.str().str());
  }

  // In-process codegen only (no subprocess, ADR-225): owned module ->
  // TargetMachine -> relocatable object, or LTO bitcode with --flto (in-process
  // lld optimizes at link). PGO rides the PassBuilder; LTO skips the
  // per-module pipeline (PGO+LTO combined diagnoses, it needs link-time PGO).
  if (!ltoKind.empty() && pgoMode != pli::PgoMode::None) {
    std::cerr << "pli: --flto cannot be combined with --fprofile-generate/--fprofile-use\n";
    return false;
  }
  {
    IRGen ipg(diags, sema, triple, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks,
              linkBitcode, lowerMode, perOpOverrides, platformBc, noMemoryEffects);
    ipg.setPublishedEffects(published);
    ipg.setRemarkLibraryCalls(remarkLibraryCalls);
    auto om = ipg.takeModule(hir);
    if (!om) {
      if (diags.ok())
        std::cerr << "pli: codegen failed for " << input << "\n";
      return false;
    }
    // WS-A2 (ADR-267): publish what this module's external procedures promise,
    // so a module that calls them can attach the promise to its declarations.
    if (!emitEffectsFile.empty() &&
        !writePublishedEffects(emitEffectsFile, hir, ipg.procEffects(), ipg))
      return false;
    // Single-module fast path: runtime linked at BC level (never `-c`).
    // Bitcode (linkBitcode) already merged the runtime via IRGen, so the
    // embedded blob is only linked when it did not.
    if (linkRuntimeIn && !linkBitcode && !compileOnly) {
      std::string rtErr;
      llvm::Triple targetTriple(triple);
      if (!pli::linkEmbeddedLibPLI(*om->mod, targetTriple, rtErr, platformBc)) {
        std::cerr << "pli: " << rtErr << "\n";
        return false;
      }
    }
    // Create TargetMachine using the provided target triple.
    std::string tmErr;
    auto tm = pli::createTargetMachine(triple, optLevel, tmErr, marchNative);
    if (!tm) {
      std::cerr << "pli: no target (" << tmErr << ")\n";
      return false;
    }
    // The IRGen layout is approximate; the TargetMachine owns the truth.
    om->mod->setDataLayout(tm->createDataLayout());
    // Also set module target triple to ensure consistent
    // feature handling during codegen (module triple affects some defaults).
    om->mod->setTargetTriple(llvm::Triple(triple));
    std::error_code ec;
    llvm::raw_fd_ostream os(objPath.string(), ec, llvm::sys::fs::OF_None);
    if (ec) {
      std::cerr << "pli: cannot write " << objPath.string() << "\n";
      return false;
    }
    std::string emErr;
    bool emitted = false;
    if (!ltoKind.empty()) {
      // LTO object: pre-link pipeline + summarised bitcode; in-process lld
      // runs the link-time optimizer.
      emitted = pli::emitLTOObject(*om->mod, *tm, os, ltoKind == "thin", optLevel, emErr);
    } else {
      // Mirror the old clang backend: optimize the (runtime-linked) IR at the
      // requested -O level before codegen, with in-process PGO when asked.
      auto pgo = pli::makePgoOptions(pgoMode, pgoUse);
      if (pgoMode == pli::PgoMode::Use) {
        // Runtime overlap must not validate a stale profile (the blob is
        // merged into every MAIN module); user functions must match.
        std::set<std::string> rtNames =
            pli::runtimeFuncNames(llvm::Triple(om->mod->getTargetTriple()));
        if (!pli::validateUseProfile(pgoUse, *om->mod, rtNames, emErr)) {
          std::cerr << "pli: " << emErr << "\n";
          return false;
        }
      }
      pli::optimizeModule(*om->mod, *tm, optLevel, pgo);
      emitted = pli::emitObject(*om->mod, *tm, os, emErr);
    }
    os.close();
    if (!emitted) {
      std::cerr << "pli: emit failed (" << emErr << ")\n";
      return false;
    }
    if (keepLL) {
      llvm::raw_fd_ostream llOs((keepLLDir / (base + ".ll")).string(), ec, llvm::sys::fs::OF_None);
      if (!ec) {
        llOs << "; Generated by pli (PL/I -> LLVM)\n";
        om->mod->print(llOs, nullptr);
      }
    }
    if (verbose)
      std::cerr << "pli: emitted " << objPath.string() << " in-process\n";
    if (!compileOnly)
      *outObj = objPath.string();
    return true;
  }
}

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string output, runtimeLib = PLI_RUNTIME_LIB, triple;
  // Split runtime: custom platform TU (--rt-platform); empty selects the
  // embedded pli_rtplat.bc. Always explicit (no baked-in default path).
  std::string platformBc;
  std::string clangPath; // hidden --clang override (parsed, never invoked)
  std::string sysparm;
  bool sysparmExplicit = false;
  std::string optLevel = "-O2";
  std::vector<std::string> linkArgs;    // extra args appended to the link step
  std::vector<std::string> includeDirs; // %INCLUDE search dirs (-I, repeatable)
  bool emitLLVM = false, syntaxOnly = false, keepLL = false, verbose = false, compileOnly = false;
  bool runtimeExplicit = false, print_hir = false, print_facts = false, release = false, debug = false;
  bool marchNative = false;
  bool noSizeChecks = false, noSizeChecksExplicit = false, noZdivChecks = false,
       noConvChecks = false, noSubChecks = false, noSubChecksExplicit = false, wantVersion = false;
  bool noMemoryEffects = false;
  bool warnUnset = false; // --warn-unset: WS-A5 "used before set" (ADR-266)
  // --remark-library-calls: WS-K4, the library-call conversion remark (ADR-254).
  bool remarkLibraryCalls = false;
  // WS-A2 (ADR-267): the published-effects file, in both directions.
  std::string effectsFile, emitEffectsFile;
  bool useBitcode = true, pgoGenerate = false;
  std::string ltoKind, pgoUse;
  int explain = 0;
  // Experimental lowering mode (design-docs/mlir-runtime-migration-plan.md §5.2).
  LowerMode lowerMode = LowerMode::Auto;
  std::map<std::string, LowerMode> perOpOverrides;
  bool emitMlir = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "pli: missing argument for " << what << "\n";
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
    else if (a == "--print-facts")
      print_facts = true;
    else if (a == "--remark-library-calls")
      remarkLibraryCalls = true;
    else if (a == "-fsyntax-only")
      syntaxOnly = true;
    else if (a == "--keep-ll")
      keepLL = true;
    else if (a == "--runtime") {
      runtimeLib = next("--runtime");
      runtimeExplicit = true;
    } else if (a == "--no-bitcode-runtime")
      useBitcode = false;
    else if (a == "--rt-platform") {
      platformBc = next("--rt-platform");
    } else if (a.rfind("--rt-platform=", 0) == 0) {
      platformBc = a.substr(14);
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
    else if (a == "--march=native")
      marchNative = true;
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
    else if (a == "--linker") {
      // ADR-225: in-process lld is the only linker; --linker names nothing
      // to select. Warn (don't error: old build scripts pass it) and drop it.
      std::cerr << "pli: warning: --linker '" + next("--linker") +
                       "' ignored (in-process lld is the only linker)\n";
    }
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
        std::cerr << "pli: --explain needs a rule number 1..151\n";
        return 2;
      }
      explain = (int)v;
    } else if (a.rfind("--experimental-lowering=", 0) == 0) {
      std::string val = a.substr(24);
      std::string opName;
      std::string modeStr;
      auto colon = val.find(':');
      if (colon != std::string::npos) {
        opName = val.substr(0, colon);
        modeStr = val.substr(colon + 1);
      } else {
        modeStr = val;
      }
      LowerMode parsed;
      if (modeStr == "auto")
        parsed = LowerMode::Auto;
      else if (modeStr == "runtime")
        parsed = LowerMode::Runtime;
      else if (modeStr == "llvm")
        parsed = LowerMode::LLVM;
      else if (modeStr == "mlir")
        parsed = LowerMode::MLIR;
      else {
        std::cerr << "pli: --experimental-lowering expects auto|runtime|llvm|mlir, or op:mode\n";
        return 2;
      }
#ifndef PLI_ENABLE_MLIR
      if (parsed == LowerMode::MLIR) {
        std::cerr << "pli: --experimental-lowering=mlir requires an MLIR-enabled build (-DPLI_ENABLE_MLIR=ON)\n";
        return 2;
      }
#endif
      if (colon != std::string::npos)
        perOpOverrides[opName] = parsed;
      else
        lowerMode = parsed;
    } else if (a == "-emit-mlir") {
      emitMlir = true;
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
    } else if (a == "--no-memory-effects") {
      noMemoryEffects = true;
    } else if (a.rfind("--emit-effects=", 0) == 0) {
      emitEffectsFile = a.substr(15);
    } else if (a.rfind("--effects=", 0) == 0) {
      effectsFile = a.substr(10);
    } else if (a == "--warn-unset") {
      warnUnset = true;
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
      std::cerr << "pli: unknown option " << a << "\n";
      return 2;
    } else
      inputs.push_back(a);
  }

  // `--explain` needs no input file: print the production and exit.
  if (explain) {
    if (!explainRule(explain)) {
      std::cerr << "pli: no TR 25.084 rule (" << explain << ")\n";
      return 1;
    }
    return 0;
  }

  // `pli version` / `pli --version`: no input file needed.
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
  if (!foreignObjs.empty() &&
      (compileOnly || emitLLVM || emitMlir || syntaxOnly || print_hir || print_facts)) {
    std::cerr << "pli: object inputs cannot be used with "
                 "-c/-emit-llvm/-emit-mlir/-fsyntax-only/--print-hir/--print-facts\n";
    return 2;
  }
  const bool linkOnly = pliInputs.empty();

  // Upward filesystem walks (Zig-style) for include and library directories.
  // Start from the source root (cwd) and the pli executable directory; at each
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
  // PLI_LIB_PATH (path-list separated, like LIBRARY_PATH): searched after
  // user -L, before the upward-walk and static system library dirs.
  if (const char* env = std::getenv("PLI_LIB_PATH")) {
    std::stringstream ss(env);
    std::string dir;
    while (std::getline(ss, dir, pathListSeparator()))
      if (!dir.empty())
        linkArgs.push_back("-L" + dir);
  }
  // Append discovered library dirs as -L entries so they feed both the
  // in-process lld path and the clang fallback path.
  for (const fs::path& d : discoveredLibDirs)
    linkArgs.push_back("-L" + d.string());

  // Single-binary distribution: the runtime ships embedded in the binary,
  // so there is no installed-runtime lookup. runtimeLib is either the
  // baked-in build-tree default (development) or an explicit --runtime path
  // (external option). Bitcode runtime (OPTIMIZATION.md §10, P3): by default
  // IRGen merges the pli_* definitions out of the embedded blob into the
  // MAIN unit (lib units keep external references resolved by that one
  // shared copy).
  // --no-bitcode-runtime always selects the archive path.
  // Split runtime: a custom platform file must exist; there is no baked-in
  // a user error. The embedded pli_rtplat.bc covers the empty case.
  if (!platformBc.empty() && !fs::exists(platformBc)) {
    std::cerr << "pli: no such platform runtime: " << platformBc << "\n";
    return 2;
  }

  Preprocessor preprocessor;
  for (const std::string& d : includeDirs)
    preprocessor.addIncludeDir(d);
  // Path-list separated like CPATH: searched after -I, before the default dir.
  bool hasEnvPath = false;
  if (const char* env = std::getenv("PLI_INCLUDE_PATH")) {
    std::stringstream ss(env);
    std::string dir;
    while (std::getline(ss, dir, pathListSeparator()))
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
      (executablePath(argv[0]).parent_path().parent_path() / "share/pli/include").string();
  preprocessor.addIncludeDir(defaultInc);
  if (verbose) {
    std::cerr << "pli: include search dirs:\n";
    for (const std::string& d : includeDirs)
      std::cerr << "pli:   " << d << "\n";
    if (hasEnvPath)
      std::cerr << "pli:   $PLI_INCLUDE_PATH\n";
    for (const fs::path& d : discoveredIncDirs)
      std::cerr << "pli:   " << d << "\n";
    std::cerr << "pli:   " << defaultInc << "\n";
    std::cerr << "pli: library search dirs:\n";
    for (const std::string& la : linkArgs)
      if (la.rfind("-L", 0) == 0)
        std::cerr << "pli:   " << la << "\n";
  }

  // clang-matching guards: a single -o cannot name more than one output.
  const bool multi = pliInputs.size() > 1;
  if (compileOnly && multi && !output.empty()) {
    std::cerr << "pli: cannot specify -o when generating multiple output files\n";
    return 2;
  }
  if (multi && !output.empty() && (emitLLVM || emitMlir || syntaxOnly || print_hir || print_facts)) {
    std::cerr << "pli: -o is ambiguous with a per-file mode and multiple inputs\n";
    return 2;
  }

  // --release / --debug are overarching presets that select the underlying
  // optimization knobs: release = -O3 + minimal size (dead-strip + strip
  // symbol table, threaded into the in-process lld link as -Wl, flags);
  // debug = -O0 (no DWARF is emitted yet, so there is no -g to thread).
  if (release) {
    optLevel = "-O3";
    linkArgs.push_back("-Wl,-dead_strip");
    linkArgs.push_back("-Wl,-S");
  } else if (debug) {
    optLevel = "-O0";
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
  // Explicit opt-in PGO/LTO (OPTIMIZATION.md §10, P3), all in-process
  // (ADR-225): --flto emits LTO bitcode objects that in-process lld optimizes
  // at link; --fprofile-generate instruments via PassBuilder and links the
  // profile runtime; --fprofile-use consumes a raw or indexed profile through
  // InstrProfReader. No clang pipeline remains.
  pli::PgoMode pgoMode = pli::PgoMode::None;
  if (pgoGenerate)
    pgoMode = pli::PgoMode::Generate;
  else if (!pgoUse.empty())
    pgoMode = pli::PgoMode::Use;

  // Compile each input to its own object; per-file modes (-c, -emit-llvm,
  // --print-hir, -fsyntax-only) stop after all inputs are handled.
  const bool terminalMode = compileOnly || emitLLVM || emitMlir || syntaxOnly || print_hir || print_facts;
  // -emit-mlir conflicts with explicit runtime/llvm modes and is a terminal
  // output mode (design-docs/mlir-runtime-migration-plan.md §5.2). It requires
  // an MLIR-enabled build (P2); in a normal build it is a driver-level error.
  if (emitMlir && (lowerMode == LowerMode::Runtime || lowerMode == LowerMode::LLVM)) {
    std::cerr << "pli: -emit-mlir conflicts with --experimental-lowering=runtime/llvm\n";
    return 2;
  }
  if (emitMlir)
    emitLLVM = true; // -emit-mlir also stops after emission
  // Every unit of a multi-input link is a relocatable object (like -c): only
  // units that actually declare OPTIONS(MAIN) emit a `main` shim.
  const bool semaCompileOnly = compileOnly || (pliInputs.size() + foreignObjs.size() > 1);
  // --keep-ll places each .ll next to the output (or the cwd with no -o).
  fs::path keepLLDir = output.empty() ? fs::path(".") : fs::path(output).parent_path();
  if (keepLLDir.empty())
    keepLLDir = ".";
  // Runtime-once fast path: one PL/I input becoming the executable directly.
  // Bitcode runtime: the MAIN unit embeds the pli_* definitions from the
  // embedded blob; an explicit --runtime
  // archive stays on the link line, where it is never pulled once the symbols
  // are defined by that embedded copy (runtime globals must stay shared).
  // The embedded blob is always selected, so the merge needs no
  // runtime files on disk. An empty platformBc likewise selects the embedded
  // pli_rtplat.bc; --rt-platform names a custom platform TU instead.
  const bool linkBitcode = useBitcode && !emitLLVM;
  // Runtime-once fast path: one PL/I input becoming the executable directly.
  // Disabled for the archive path (!useBitcode: --no-bitcode-runtime keeps
  // external references for the link step). PGO/LTO stay on the in-process
  // path (ADR-225); only PGO+LTO combined is rejected (in compileOne).
  const bool singleModuleFinalLink =
      !compileOnly && !terminalMode && pliInputs.size() == 1 && foreignObjs.empty() && useBitcode;
  std::vector<std::string> objs = foreignObjs;
  for (size_t i = 0; i < pliInputs.size(); ++i) {
    std::string outObj;
    if (!compileOne(preprocessor, pliInputs[i], output, triple, sysparm, sysparmExplicit,
                    compileOnly, semaCompileOnly, emitLLVM, emitMlir, syntaxOnly, print_hir, print_facts,
                    keepLL, verbose, noSizeChecks, noZdivChecks, noConvChecks, noSubChecks, optLevel,
                    keepLLDir, (int)i, &outObj, linkBitcode, lowerMode, perOpOverrides,
                    singleModuleFinalLink, ltoKind, pgoMode, pgoUse,
                    false /* marchNative: compileOne keeps its default (unchanged) */, platformBc,
                    noMemoryEffects, warnUnset, effectsFile, emitEffectsFile,
                    remarkLibraryCalls))
      return 1;
    if (!compileOnly)
      objs.push_back(outObj);
  }
  if (terminalMode)
    return 0;
  if (linkOnly && objs.empty()) {
    std::cerr << "pli: no input objects to link\n";
    return 2;
  }

  // --- link step ----------------------------------------------------------
  // In-process lld link only (no subprocess, ADR-225; PLI_HAVE_LLD is a hard
  // configure requirement now). With the bitcode runtime the MAIN object
  // already defines the pli_* symbols (an explicit --runtime archive's members
  // are then simply never pulled: no duplicates); --no-bitcode-runtime
  // compiles every unit with external references resolved at link from the
  // embedded blob. Drop unreferenced runtime sections (function sections,
  // ADR-079); multitasking (QR2.8) runs on pthreads.
  if (output.empty())
    output = "a.out";
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();

  {
    llvm::Triple llt(triple);
    const pli::TargetDesc& desc = pli::getTargetDesc(llt);
    std::string tmErr;
    if (auto tm = pli::createTargetMachine(triple, optLevel, tmErr, marchNative)) {
      // Link-mode scan (once): PIE unless -static/-shared/-no-pie, and whether
      // this is a shared-library link (no startup objects, no interpreter).
      bool wantPIE = true, wantShared = false;
      for (const std::string& la : linkArgs) {
        std::string f = la.rfind("-Wl,", 0) == 0 ? la.substr(4) : la;
        if (f == "-static" || f == "-static-pie" || f == "-shared" || f == "-no-pie" ||
            f == "-nopie" || f == "-no-pic")
          wantPIE = false;
        if (f == "-shared" || la == "-shared")
          wantShared = true;
      }
      if (wantShared)
        wantPIE = false;
      // With the bitcode runtime (linkBitcode) the
      // MAIN object already defines every pli_* symbol, so no separate
      // runtime object is emitted (it would duplicate them). Otherwise
      // multi-input links carry the runtime as its own object (runtime-once);
      // single-module links already carry it at BC level
      // (singleModuleFinalLink above).
      std::string rtObj;
      bool haveRtObj = false;
      if (!singleModuleFinalLink && !linkBitcode) {
        // A unique temp name, created exclusively (see the per-input object
        // above): a predictable "<tmp>/pli_runtime-<pid>.o" could be planted.
        llvm::SmallString<128> rtPath;
        std::error_code fec = llvm::sys::fs::createUniqueFile(
            (fs::temp_directory_path() / "pli_runtime-%%%%%%.o").string(), rtPath);
        rtObj = rtPath.str().str();
        std::error_code ec;
        llvm::raw_fd_ostream os(rtObj, ec, llvm::sys::fs::OF_None);
        std::string rtErr;
        haveRtObj = !ec && pli::emitRuntimeObject(*tm, llt, os, rtErr, platformBc);
        if (!haveRtObj && verbose)
          std::cerr << "pli: runtime object failed (" << rtErr << "), using clang\n";
      }
      if (singleModuleFinalLink || haveRtObj || linkBitcode) {
        // Single-file self-containment (ADR-227): the link sysroot is
        // EMBEDDED in pli — release archives hold only the binary. An
        // exe-relative musl//mingw/ dir is a dev override (iterate on
        // sysroot files without rebuilding) and wins when complete; the
        // embedded set is next. $PLI_SYSROOT never redirects either
        // (self-contained already).
        // -shared/-static-pie stay on the host recipe (a static sysroot
        // cannot link them).
        pli::VendoredSysroot vsys = pli::resolveVendoredSysroot(exeDir.string());
        const bool muslOverride = vsys.haveMusl;
        const bool mingwOverride = vsys.haveMingw;
        // Lazily materialized below (once): avoided entirely when the
        // override already satisfies the link, or for -c/emit flows.
        pli::EmbeddedSysroot emb;
        bool embTried = false;
        auto ensureEmbedded = [&]() {
          if (!embTried) {
            embTried = true;
            emb = pli::materializeEmbeddedSysroot();
          }
        };
        if (!vsys.haveMusl && !vsys.haveMingw) {
          ensureEmbedded();
          if (emb.name == "musl" && !emb.dir.empty()) {
            vsys.haveMusl = true;
            vsys.muslDir = emb.dir;
          } else if (emb.name == "mingw" && !emb.dir.empty()) {
            vsys.haveMingw = true;
            vsys.mingwDir = emb.dir;
          }
        }
        bool wantStaticPIE = false;
        for (const std::string& la : linkArgs) {
          const std::string f = la.rfind("-Wl,", 0) == 0 ? la.substr(4) : la;
          if (f == "-static-pie") {
            wantStaticPIE = true;
            break;
          }
        }
        const bool useMusl = llt.getObjectFormat() == llvm::Triple::ELF && vsys.haveMusl &&
                             !wantShared && !wantStaticPIE;
        const bool useMingw = llt.getObjectFormat() == llvm::Triple::COFF && vsys.haveMingw &&
                              !wantShared;
        if (useMusl && verbose)
          std::cerr << "pli: using vendored musl sysroot ("
                    << (muslOverride ? "exe-relative override" : "embedded") << ": " << vsys.muslDir
                    << ")\n";
        if (useMingw && verbose)
          std::cerr << "pli: using vendored mingw sysroot ("
                    << (mingwOverride ? "exe-relative override" : "embedded") << ": "
                    << vsys.mingwDir << ")\n";
        // Host C-runtime/sysroot recipe (hostcrt.cpp): stable filesystem
        // locations, first-hit-wins, $PLI_SYSROOT redirect. No subprocess.
        // Skipped entirely on vendored paths (host independence proof: even
        // a bogus $PLI_SYSROOT cannot break the link).
        std::string sysroot;
        if (const char* env = std::getenv("PLI_SYSROOT"))
          sysroot = env;
        pli::HostCrt crt;
        if (!useMusl && !useMingw) {
          crt = pli::resolveHostCrt(llt, sysroot);
          if (!crt.ok) {
            std::cerr << "pli: " << crt.err << "\n";
            // The sysroot was already materialized above: release it before
            // the early return, or the temp dir is left behind.
            pli::cleanupMaterializedSysroot(emb.dir);
            return 1;
          }
        }
        std::vector<std::string> argStore = {"ld.lld"};
        if (useMingw) {
          // MinGW driver (GNU-ld-compatible argv): crt2.o + crtbegin.o, the
          // objects, fltused.o (MSVC _fltused marker clang emits), then the
          // import libs + GCC helpers, then crtend.o. libgcc.a covers
          // __udivti3/___chkstk_ms, so no compiler-rt builtins ride.
          // -m i386pep is the x86_64 emulation (aarch64 ships its own
          // sysroot + spelling when that target lands).
          const std::string& md = vsys.mingwDir;
          argStore.push_back("-m");
          argStore.push_back("i386pep");
          argStore.push_back(md + "/crt2.o");
          argStore.push_back(md + "/crtbegin.o");
        }
        if (useMusl) {
          // Static musl (matches musl-gcc -static): no interpreter, no host
          // libdirs, no -lgcc — the binary needs only the kernel at runtime.
          argStore.push_back("-static");
          argStore.push_back(vsys.muslDir + "/crt1.o");
          argStore.push_back(vsys.muslDir + "/crti.o");
        }
        if (llt.getObjectFormat() == llvm::Triple::MachO) {
          // Vendored SDK stubs (ADR-226/227): exe-relative <exeDir>/sdk is
          // a dev override; the embedded stubs are next (single-file
          // release); otherwise the Xcode CLT provides the syslibroot via
          // xcrun (Apple tooling, not clang).
          std::string sdkPath;
          bool sdkOverride = false;
          {
            std::error_code ec;
            fs::path stub = exeDir / "sdk";
            if (fs::is_directory(stub, ec)) {
              sdkPath = stub.string();
              sdkOverride = true;
            }
          }
          if (sdkPath.empty()) {
            ensureEmbedded();
            if (emb.name == "sdk" && !emb.dir.empty())
              sdkPath = emb.dir;
          }
          if (!sdkPath.empty()) {
            if (verbose)
              std::cerr << "pli: using vendored SDK stubs ("
                        << (sdkOverride ? "exe-relative override" : "embedded") << ": " << sdkPath
                        << ")\n";
          } else {
            if (FILE* f = popen("xcrun --show-sdk-path 2>/dev/null", "r")) {
              char buf[1024];
              while (fgets(buf, sizeof buf, f))
                sdkPath += buf;
              pclose(f);
            }
            while (!sdkPath.empty() && (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
              sdkPath.pop_back();
          }
          argStore.push_back("-arch");
          argStore.push_back(std::string(llt.getArchName()));
          if (!sdkPath.empty()) {
            argStore.push_back("-syslibroot");
            argStore.push_back(sdkPath);
          }
        } else if (llt.getObjectFormat() == llvm::Triple::ELF && !useMusl) {
          // PIE startup (matches clang): Scrt1.o + crtbeginS.o/crtendS.o with
          // -pie and an explicit interpreter, else a non-PIE EXEC segfaults.
          if (wantPIE) {
            argStore.push_back("-pie");
            argStore.push_back("--eh-frame-hdr");
            argStore.push_back("--hash-style=gnu");
            argStore.push_back("--build-id");
            if (!crt.interpreter.empty()) {
              argStore.push_back("-dynamic-linker");
              argStore.push_back(crt.interpreter);
            } else if (verbose) {
              std::cerr << "pli: no host interpreter found\n";
            }
          }
          // Non-PIE links need the non-S CRT set; the recipe probed the PIE
          // set, so fall back to bare names lld resolves via -L (same as the
          // old clang -print-file-name fallback to the bare filename).
          // Shared libraries take neither.
          if (!wantShared) {
            if (wantPIE) {
              for (const std::string& s : crt.startupObjects)
                argStore.push_back(s);
            } else {
              argStore.push_back("crt1.o");
              argStore.push_back("crti.o");
              argStore.push_back("crtbegin.o");
            }
          }
          // Recipe lib dirs so lld finds -lm, -lc, -lgcc_s without clang.
          for (const std::string& d : crt.libDirs)
            argStore.push_back("-L" + d);
          if (!sysroot.empty() && sysroot != "/") {
            argStore.push_back("--sysroot");
            argStore.push_back(sysroot);
          }
        }
        for (const std::string& o : objs)
          argStore.push_back(o);
        if (haveRtObj)
          argStore.push_back(rtObj);
        // Explicit --runtime archive (external option): on the link line where
        // its members are never pulled once MAIN defines pli_* (bitcode path),
        // or as the symbol source on the archive path. The old clang fallback
        // owned this; the in-process link is now the only link.
        {
          const bool runtimeLibUsable = !runtimeLib.empty() && fs::exists(runtimeLib);
          if (runtimeLibUsable)
            argStore.push_back(runtimeLib);
          else if (runtimeExplicit) {
            std::cerr << "pli: runtime archive not found: " << runtimeLib << "\n";
            pli::cleanupMaterializedSysroot(emb.dir);
            return 1;
          } else if (verbose && linkBitcode)
            std::cerr << "pli: runtime archive not found; MAIN already defines pli_* (embedded)\n";
        }
        if (llt.getObjectFormat() == llvm::Triple::ELF && !useMusl) {
          if (!wantShared) {
            if (wantPIE) {
              for (const std::string& s : crt.endObjects)
                argStore.push_back(s);
            } else {
              argStore.push_back("crtend.o");
              argStore.push_back("crtn.o");
            }
          }
          // GCC unwinding helpers (matches clang: -lgcc --as-needed -lgcc_s).
          argStore.push_back("-lgcc");
          argStore.push_back("--as-needed");
          argStore.push_back("-lgcc_s");
          argStore.push_back("--no-as-needed");
        }
        if (useMusl) {
          // musl close: our builtins (128-bit helpers musl omits) + the libc
          // archive (provides -lc/-lm/pthread) + crtn.o. -L points at the
          // sysroot so user -l flags (e.g. -lm) resolve to its empty stubs,
          // never to host libraries. Host -lgcc/system libs stay off: the
          // binary is fully static.
          argStore.push_back(vsys.muslDir + "/libpli_builtins-" +
                             std::string(llt.getArchName()) + ".a");
          argStore.push_back(vsys.muslDir + "/libc.a");
          argStore.push_back(vsys.muslDir + "/crtn.o");
          argStore.push_back("-L" + vsys.muslDir);
        }
        if (!useMusl && !useMingw) {
          for (const std::string& s : desc.systemLibs)
            argStore.push_back(s);
        }
        if (llt.getObjectFormat() == llvm::Triple::ELF && !useMusl) {
          // Trailing helpers (matches clang: -lc -lgcc --as-needed -lgcc_s).
          argStore.push_back("-lgcc");
          argStore.push_back("--as-needed");
          argStore.push_back("-lgcc_s");
          argStore.push_back("--no-as-needed");
        }
        if (!useMingw) {
          for (const std::string& f : desc.linkerFlags)
            argStore.push_back(f);
        } else {
          // MinGW driver: GNU-style close (crtend.o); the COFF
          // /entry:/subsystem: flags are not valid here (crt2.o defaults).
          argStore.push_back(vsys.mingwDir + "/crtend.o");
        }
        for (const std::string& la : linkArgs) {
          // Vendored musl dictates a static EXEC: -pie conflicts with the
          // -static above, and the GNU-named -nopie/-no-pic are not lld
          // flags, so drop them (-static-pie routed to host earlier). The
          // non-PIE spelling -no-pie/-no_pie agrees with static and reaches
          // lld like any other -Wl flag.
          const std::string fwd = la.rfind("-Wl,", 0) == 0 ? la.substr(4) : la;
          if (useMusl && (fwd == "-pie" || fwd == "-nopie" || fwd == "-no-pic"))
            continue;
          if (la.rfind("-Wl,", 0) == 0) {
            // lld takes the flag directly, but ELF lld uses -no-pie (hyphen),
            // not Mach-O's -no_pie (underscore).
            std::string f = la.substr(4);
            if (llt.getObjectFormat() == llvm::Triple::ELF && f == "-no_pie")
              f = "-no-pie";
            argStore.push_back(f);
          } else if (la == "-shared" && llt.getObjectFormat() == llvm::Triple::MachO) {
            // The clang driver maps -shared to -dylib; in-process lld needs
            // the spelling itself.
            argStore.push_back("-dylib");
          } else if (la == "-shared" && llt.getObjectFormat() == llvm::Triple::COFF) {
            argStore.push_back("/dll");
          } else
            argStore.push_back(la);
        }
        // PGO instrumented link: the counters need the compiler-rt profile
        // runtime (shipped next to pli / in the toolchain resource dir).
        if (pgoMode == pli::PgoMode::Generate) {
          std::string profRt = pli::profileRuntimeLib(llt, exeDir.string());
          if (profRt.empty()) {
            std::cerr << "pli: --fprofile-generate needs the profile runtime (clang_rt.profile-"
                      << llt.getArchName().str() << ".a next to pli)\n";
            pli::cleanupMaterializedSysroot(emb.dir);
            return 1;
          }
          argStore.push_back(profRt);
        }
        if (useMingw) {
          // fltused marker + import libs + GCC helpers (MinGW driver takes
          // GNU -l names; -L points at the vendored dir). Order mirrors the
          // mingw-gcc recipe: startup, objects, libs, crtend.o (added above).
          const std::string& md = vsys.mingwDir;
          argStore.push_back(md + "/fltused.o");
          argStore.push_back(md + "/chkstk.o");
          argStore.push_back("-L" + md);
          // libkernel32.a repeats: one-pass GNU linking needs it again for
          // symbols referenced by the later import libs.
          for (const char* lib :
               {"libmingw32.a", "libgcc.a", "libmoldname.a", "libmingwex.a", "libucrt.a",
                "libmsvcrt.a", "libkernel32.a", "libadvapi32.a", "libshell32.a",
                "libuser32.a", "libkernel32.a"})
            argStore.push_back(std::string("-l:") + lib);
          argStore.push_back("-o");
          argStore.push_back(output);
        } else if (llt.getObjectFormat() == llvm::Triple::COFF) {
          // compiler-rt builtins for 128-bit helpers (__udivti3, __divti3)
          // used by the runtime. Shipped next to pli.exe in the release
          // archive (copied from our own toolchain at pack time); the old
          // backend-clang-relative probe is gone with the clang subprocess.
          std::string rtArch;
          if (llt.getArch() == llvm::Triple::x86_64)
            rtArch = "x86_64";
          else if (llt.getArch() == llvm::Triple::aarch64)
            rtArch = "aarch64";
          else if (llt.getArch() == llvm::Triple::arm)
            rtArch = "arm";
          if (!rtArch.empty()) {
            fs::path builtins = exeDir / ("clang_rt.builtins-" + rtArch + ".lib");
            std::error_code bsec;
            if (fs::exists(builtins, bsec))
              argStore.push_back(builtins.string());
            else if (verbose)
              std::cerr << "pli: compiler-rt builtins not found (" << builtins.string()
                        << "), 128-bit division may fail to link\n";
          }
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
        // ADR-227: the vendored-mingw link runs the MinGW driver (GNU argv);
        // every other link runs the object-format driver as before.
        const bool linkOk = useMingw ? pli::linkMinGWExecutable(argv, linkErr)
                                     : pli::linkExecutable(argv, (int)llt.getObjectFormat(), linkErr);
        if (linkOk) {
          for (size_t i = foreignObjs.size(); i < objs.size(); ++i) {
            std::error_code ec;
            fs::remove(objs[i], ec);
          }
          if (haveRtObj) {
            std::error_code ec;
            fs::remove(rtObj, ec);
          }
          pli::cleanupMaterializedSysroot(emb.dir);
          return 0;
        }
        std::cerr << "pli: in-process link failed (" << linkErr << ")\n";
        // Release every temp object, not just the runtime: the success path
        // above removes them all, and a failed link left the per-input
        // objects behind in the temp dir.
        for (size_t i = foreignObjs.size(); i < objs.size(); ++i) {
          std::error_code ec;
          fs::remove(objs[i], ec);
        }
        if (haveRtObj) {
          std::error_code ec;
          fs::remove(rtObj, ec);
        }
        pli::cleanupMaterializedSysroot(emb.dir);
        return 1;
      }
    } else {
      std::cerr << "pli: no target (" << tmErr << ")\n";
      return 1;
    }
  }
  // Unreachable: createTargetMachine either links above (return 0) or returns
  // nonzero. No clang fallback remains (ADR-225).
  return 1;
}
