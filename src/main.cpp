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

namespace fs = std::filesystem;

static void usage() {
  std::cout
      << "plic — PL/I compiler (LLVM backend)\n"
         "\n"
         "usage: plic [options] file.pli\n"
         "\n"
         "options:\n"
         "  -o <file>        output file (default: a.out, or <base>.ll with -emit-llvm)\n"
         "  -c               compile to a relocatable object (no linking)\n"
         "  -emit-llvm       write LLVM IR and stop\n"
         "  --print-hir      lower to HIR and print it, then stop\n"
         "  -fsyntax-only    parse and analyse only\n"
         "  -O0 -O1 -O2 -O3  optimization level passed to the LLVM pipeline (default -O2)\n"
         "  --no-size-checks elide FIXED overflow traps program-wide (cf. (NOSIZE), ADR-111)\n"
         "  --release        maximum optimization + stripped binary (minimal size)\n"
         "  --debug          no optimization + debug info (-O0 -g)\n"
         "  --keep-ll        keep the intermediate .ll next to the output\n"
         "  --runtime <lib>  path to libpli.a (default: baked in at build time)\n"
         "  --clang <path>   clang to assemble/link the IR (default: LLVM's clang)\n"
                    "  --triple <t>     target triple (default: `clang -dumpmachine`)\n"
          "  --sysparm <s>    value returned by the SYSPARM builtin (rule (123))\n"
         "  -L <dir>         add a library search path to the link step\n"
         "  -I <dir>         add a %INCLUDE search directory (repeatable; -I<dir> too)\n"
         "  -l<lib>          link a library (e.g. -lm) on the link step\n"
         "  -Wl,<flag>       pass a raw flag to the linker (repeatable)\n"
         "  --linker <ld>    select the linker via -fuse-ld=<ld>\n"
         "  -shared -static  produce a shared / static binary\n"
         "  --extra <a,b,c>  comma-separated extra backend args appended to the link\n"
         "  --explain <n>    print TR 25.084 rule (n)'s production and exit\n"
         "  -v               show the sub-commands being run\n"
         "  -h, --help       this message\n";
}

static std::string runCapture(const char* cmd) {
  std::string out;
  FILE* f = popen(cmd, "r");
  if (!f)
    return out;
  char buf[256];
  while (fgets(buf, sizeof buf, f))
    out += buf;
  pclose(f);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
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

int main(int argc, char** argv) {
  std::string input, output, runtimeLib = PLIC_RUNTIME_LIB, triple;
  std::string clangPath = PLIC_CLANG;
  std::string sysparm;
  bool sysparmExplicit = false;
  std::string optLevel = "-O2";
  std::vector<std::string> linkArgs;    // extra args appended to the link step
  std::vector<std::string> includeDirs; // %INCLUDE search dirs (-I, repeatable)
  bool emitLLVM = false, syntaxOnly = false, keepLL = false, verbose = false, compileOnly = false;
  bool runtimeExplicit = false, print_hir = false, release = false, debug = false;
  bool noSizeChecks = false;
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
    else if (!a.empty() && a[0] == '-') {
      std::cerr << "plic: unknown option " << a << "\n";
      return 2;
    } else if (input.empty())
      input = a;
    else {
      std::cerr << "plic: more than one input file given\n";
      return 2;
    }
  }

  // `--explain` needs no input file: print the production and exit.
  if (explain) {
    if (!explainRule(explain)) {
      std::cerr << "plic: no TR 25.084 rule (" << explain << ")\n";
      return 1;
    }
    return 0;
  }

  if (input.empty()) {
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

  std::string src;
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
  if (!preprocessor.run(input, src))
    return 1;

  Diags diags(input);
  diags.setSource(&src);

  // --- front end ---------------------------------------------------------
  Lexer lexer(src, diags);
  std::vector<Token> toks = lexer.run();
  if (!diags.ok())
    return 1;

  Parser parser(std::move(toks), diags);
  std::unique_ptr<Program> prog = parser.parse();
  if (!diags.ok())
    return 1;

  Sema sema(diags);
  if (!sysparmExplicit) {
    if (const char* env = std::getenv("PLIC_SYSPARM"))
      sysparm = env;
  }
  sema.setSysparm(sysparm);
  sema.run(*prog, compileOnly);
  if (!diags.ok())
    return 1;

  if (syntaxOnly)
    return 0;

  // Lower the typed AST to HIR (ADR-005). `--print-hir` shows it and stops.
  HProgram hir = lower(*prog);
  if (print_hir) {
    printHIR(hir, std::cout);
    return 0;
  }

  // --- code generation ---------------------------------------------------
  if (triple.empty())
    triple = runCapture((shellQuote(clangPath) + " -dumpmachine 2>/dev/null").c_str());
  IRGen irgen(diags, sema, triple, noSizeChecks);
  std::string ir = irgen.run(hir);
  if (!diags.ok())
    return 1;

  fs::path inPath(input);
  std::string base = inPath.stem().string();

  if (emitLLVM) {
    std::string dest = output.empty() ? base + ".ll" : output;
    std::ofstream os(dest, std::ios::binary);
    if (!os) {
      std::cerr << "plic: cannot write " << dest << "\n";
      return 1;
    }
    os << ir;
    return 0;
  }

  if (output.empty())
    output = compileOnly ? base + ".o" : "a.out";

  fs::path llPath =
      keepLL ? fs::path(output).parent_path() / (base + ".ll")
             : fs::temp_directory_path() / (base + "-" + std::to_string(getpid()) + ".ll");
  {
    std::ofstream os(llPath, std::ios::binary);
    if (!os) {
      std::cerr << "plic: cannot write " << llPath << "\n";
      return 1;
    }
    os << ir;
  }

  // --- assemble, optimize, link -----------------------------------------
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
  std::string cmd = shellQuote(clangPath) + " -Wno-override-module " + optLevel + backendFlags +
                    " " + shellQuote(llPath.string());
  if (compileOnly) {
    cmd += " -c"; // relocatable object: the caller performs the link step
  } else {
    // Drop unreferenced runtime sections (the archive is sectioned, ADR-079).
#ifdef __APPLE__
    cmd += " -Wl,-dead_strip";
#else
    cmd += " -Wl,--gc-sections";
#endif
    // Multitasking (QR2.8) runs on pthreads; the flag is a no-op where the
    // threading library lives in the system library (e.g. macOS).
    cmd += " -pthread";
    if (!runtimeLib.empty())
      cmd += " " + shellQuote(runtimeLib);
    for (const std::string& la : linkArgs)
      cmd += " " + la; // link flags
  }
  cmd += " -o " + shellQuote(output);
  if (verbose)
    std::cerr << "+ " << cmd << "\n";
  int rc = system(cmd.c_str());
  if (!keepLL) {
    std::error_code ec;
    fs::remove(llPath, ec);
  }
  if (rc != 0) {
    std::cerr << "plic: backend failed\n";
    return 1;
  }
  return 0;
}
