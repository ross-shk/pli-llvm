// main.cpp — plic driver.
//
// Pipeline: source -> lexer -> parser -> sema -> LLVM IR -> clang (assemble,
// optimize, link with libpli). See docs/ARCHITECTURE.md.
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
#include "irgen.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

#ifndef PLIC_RUNTIME_LIB
#define PLIC_RUNTIME_LIB ""
#endif

namespace fs = std::filesystem;

static void usage() {
  std::cout <<
      "plic — PL/I compiler (LLVM backend), M0 wireframe\n"
      "\n"
      "usage: plic [options] file.pli\n"
      "\n"
      "options:\n"
      "  -o <file>        output file (default: a.out, or <base>.ll with -emit-llvm)\n"
      "  -c               compile to a relocatable object (no linking)\n"
      "  -emit-llvm       write LLVM IR and stop\n"
      "  -fsyntax-only    parse and analyse only\n"
      "  -O0 -O1 -O2 -O3  optimization level passed to the LLVM pipeline (default -O2)\n"
      "  --keep-ll        keep the intermediate .ll next to the output\n"
      "  --runtime <lib>  path to libpli.a (default: baked in at build time)\n"
      "  --triple <t>     target triple (default: `clang -dumpmachine`)\n"
      "  -v               show the sub-commands being run\n"
      "  -h, --help       this message\n";
}

static std::string runCapture(const char *cmd) {
  std::string out;
  FILE *f = popen(cmd, "r");
  if (!f) return out;
  char buf[256];
  while (fgets(buf, sizeof buf, f)) out += buf;
  pclose(f);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

int main(int argc, char **argv) {
  std::string input, output, runtimeLib = PLIC_RUNTIME_LIB, triple;
  std::string optLevel = "-O2";
  bool emitLLVM = false, syntaxOnly = false, keepLL = false, verbose = false, compileOnly = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char *what) -> std::string {
      if (i + 1 >= argc) { std::cerr << "plic: missing argument for " << what << "\n"; exit(2); }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (a == "-o") output = next("-o");
    else if (a == "-c") compileOnly = true;
    else if (a == "-emit-llvm" || a == "--emit-llvm") emitLLVM = true;
    else if (a == "-fsyntax-only") syntaxOnly = true;
    else if (a == "--keep-ll") keepLL = true;
    else if (a == "--runtime") runtimeLib = next("--runtime");
    else if (a == "--triple") triple = next("--triple");
    else if (a == "-v") verbose = true;
    else if (a == "-O0" || a == "-O1" || a == "-O2" || a == "-O3" || a == "-Os") optLevel = a;
    else if (!a.empty() && a[0] == '-') { std::cerr << "plic: unknown option " << a << "\n"; return 2; }
    else if (input.empty()) input = a;
    else { std::cerr << "plic: more than one input file given\n"; return 2; }
  }

  if (input.empty()) { usage(); return 2; }

  std::ifstream in(input, std::ios::binary);
  if (!in) { std::cerr << "plic: cannot open " << input << "\n"; return 1; }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  Diags diags(input);
  diags.setSource(&src);

  // --- front end ---------------------------------------------------------
  Lexer lexer(src, diags);
  std::vector<Token> toks = lexer.run();
  if (!diags.ok()) return 1;

  Parser parser(std::move(toks), diags);
  std::unique_ptr<Program> prog = parser.parse();
  if (!diags.ok()) return 1;

  Sema sema(diags);
  sema.run(*prog, compileOnly);
  if (!diags.ok()) return 1;

  if (syntaxOnly) return 0;

  // --- code generation ---------------------------------------------------
  if (triple.empty()) triple = runCapture("clang -dumpmachine 2>/dev/null");
  IRGen irgen(diags, sema, triple);
  std::string ir = irgen.run(*prog);
  if (!diags.ok()) return 1;

  fs::path inPath(input);
  std::string base = inPath.stem().string();

  if (emitLLVM) {
    std::string dest = output.empty() ? base + ".ll" : output;
    std::ofstream os(dest, std::ios::binary);
    if (!os) { std::cerr << "plic: cannot write " << dest << "\n"; return 1; }
    os << ir;
    return 0;
  }

  if (output.empty()) output = compileOnly ? base + ".o" : "a.out";

  fs::path llPath = keepLL ? fs::path(output).parent_path() / (base + ".ll")
                           : fs::temp_directory_path() / (base + "-" + std::to_string(getpid()) + ".ll");
  {
    std::ofstream os(llPath, std::ios::binary);
    if (!os) { std::cerr << "plic: cannot write " << llPath << "\n"; return 1; }
    os << ir;
  }

  // --- assemble, optimize, link -----------------------------------------
  std::string cmd = "clang -Wno-override-module " + optLevel + " \"" + llPath.string() + "\"";
  if (compileOnly) {
    cmd += " -c";  // relocatable object: the caller performs the link step
  } else {
    if (!runtimeLib.empty()) cmd += " \"" + runtimeLib + "\"";
  }
  cmd += " -o \"" + output + "\"";
  if (verbose) std::cerr << "+ " << cmd << "\n";
  int rc = system(cmd.c_str());
  if (!keepLL) {
    std::error_code ec;
    fs::remove(llPath, ec);
  }
  if (rc != 0) { std::cerr << "plic: backend failed\n"; return 1; }
  return 0;
}
