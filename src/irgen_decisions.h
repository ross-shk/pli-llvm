// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// irgen_decisions.h — the compiler's PL/I-specific decisions, and the report
// `--print-facts` prints.
//
// ADR-260: the decision layer lives outside irgen.cpp (the plan's C2). What
// lives here answers "what did the compiler decide, and why" without touching
// LLVM, so a decision can be read in a test without a backend.
#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct HProc;
struct HProgram;
struct Symbol;

namespace pli {

// One built-in lowering decision: "llvm" means the inline LLVM expansion won
// over the pli_* runtime call, "runtime" means the library call.
struct BuiltinFact {
  std::string op;   // "assign_char"
  std::string path; // "llvm" | "runtime"
  unsigned count = 0;
};

// The facts the compiler computed for one procedure.
struct ProcFacts {
  std::string name;
  std::string effect;      // the memory-effect verdict (ADR-264), "" when none
  std::string effectNote;  // why the verdict is not stronger
  bool nounwind = false;   // the procedure cannot unwind
  bool willReturn = false; // the procedure always returns
  std::vector<BuiltinFact> builtins;
};

// The compiler's fact store: filled while lowering, printed by --print-facts.
// A fact is a decision the compiler already made, not a new decision — the
// decisions themselves stay where the plan put them.
class FactStore {
public:
  // Module-level facts: the lowering policy and which checks are enabled.
  void setLoweringMode(const std::string& mode) { loweringMode_ = mode; }
  void setCheck(const std::string& kind, bool on);
  // Per-procedure facts: beginProc opens a procedure, recordBuiltin adds to it.
  void beginProc(const std::string& name);
  void recordBuiltin(const std::string& op, const std::string& path);
  // The verdict for a procedure: memory effect, unwinding, return (ADR-264).
  void setEffects(const std::string& name, const std::string& effect,
                  const std::string& note, bool nounwind, bool willReturn);
  // The report: one fact per line, module facts first.
  std::string report() const;

private:
  std::string loweringMode_ = "auto";
  std::vector<std::pair<std::string, bool>> checks_;
  std::vector<ProcFacts> procs_;
  // Index into procs_ of the procedure being lowered; -1 when none is open.
  int cur_ = -1;
};

// --- WS-A1: what a PL/I procedure can do to memory (ADR-264) ---------------
//
// The verdict is computed here, outside irgen.cpp, and reaches LLVM only as
// function attributes — no custom pass (the plan's C1). Each effect value is a
// promise the optimizer may rely on; Unknown is the absence of a promise and
// leaves the function exactly as it was before this work.
enum class MemEffect {
  Unknown,      // may read and write anything — no attribute
  Read,         // may read any memory, writes none
  ArgRead,      // reads only memory reachable from its pointer arguments
  ArgReadWrite, // reads and writes only memory reachable from its pointer arguments
  None,         // touches no memory at all
};

// The verdict for one procedure.
struct ProcEffects {
  MemEffect effect = MemEffect::Unknown;
  bool nounwind = false;   // cannot unwind: every call it makes is known
  bool willReturn = false; // always returns when it is called
  std::string note;        // why the effect is not stronger, for the report
};

// Which checks the driver enabled. A check that is off cannot raise a
// condition, so the procedure cannot dispatch to an ON handler either.
struct EffectOptions {
  bool sizeChecks = true;
  bool subscriptChecks = true;
  bool zerodivideChecks = true;
  bool conversionChecks = true;
};

// Analyze every procedure in the program. `byValue` names the parameters that
// ride by value (ADR-215): their storage is the callee's own frame, so they
// are not pointer arguments.
std::map<const HProc*, ProcEffects> analyzeEffects(const HProgram& prog,
                                                    const std::unordered_set<const Symbol*>& byValue,
                                                    const EffectOptions& opts);

// --- WS-A2: publishing a procedure's effects across a compilation unit ------
//
// The inference is worth something only where the optimizer cannot see the
// callee's body: there, the callee's attributes are the only description of it
// the caller has. These two functions carry that description from the module
// that compiled the callee to the module that calls it (ADR-267).
//
// The format is one line per procedure: the name, the signature digest the
// promise was published for, then the flags.
//   PURE_HELPER 3f2a1c9d argmem-read nounwind willreturn
//
// The digest is what makes the promise safe to consume: a promise published
// for a different signature is not a promise for this call, so the consumer
// drops it rather than trusting it.
struct PublishedEffect {
  ProcEffects effects;
  std::string digest; // the PL/I signature the promise was published for
};

std::string formatEffectLine(const std::string& name, const ProcEffects& e,
                              const std::string& digest);
// Parse one line; false when the line is malformed or names an unknown flag.
bool parseEffectLine(const std::string& line, std::string& name, ProcEffects& e,
                     std::string& digest);
// The file's header line; a reader that does not know the version must refuse
// the file rather than guess at it.
extern const char* const kPublishedEffectsHeader;

// --- WS-A4: the PL/I alias taxonomy (ADR-265) ------------------------------
//
// IBM's list is the normative one (mvs:24678-24684): an aliased variable is
// any variable whose value can be modified through a name other than its own
// — DEFINED overlays, the bases of DEFINED variables, parameters, arguments,
// based variables, and variables whose address an external procedure knows.
// MULTICS's `aliasable` bit (optimizer.pl1:212-232) says how the compiler
// detects them. This is that detection, in one place, so every alias claim the
// compiler makes is checked against the same list.
enum class AliasClass {
  Disjoint, // owns its storage; disjoint from every other declaration
  Shares,   // may share storage with another declaration
  Unknown,  // may point anywhere: a locator, an overlay, an escaped address
};

// The alias class of one declaration's storage.
AliasClass aliasClass(const Symbol* s);

} // namespace pli
