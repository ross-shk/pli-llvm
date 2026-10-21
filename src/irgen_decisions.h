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
  std::string reason; // why the path was taken (ADR-254)
};

// Which path a built-in call site takes (ADR-254).
enum class InlinePath {
  Library,       // the pli_* runtime call
  LLVM,          // the compiler's own LLVM expansion
  ConstantTable, // a compile-time table built from a constant argument
};

// The decision for one built-in call site (ADR-254): the path, and the
// reason — the same string --print-facts prints and the remark quotes.
struct InlineDecision {
  InlinePath path = InlinePath::Library;
  std::string reason;

  // True when the call goes to the library routine.
  bool library() const { return path == InlinePath::Library; }
};

// What the call site looks like to the decision (ADR-254): the shape facts
// the predicates need. The default is the plain shape.
struct CallSite {
  bool constantArg = false;    // an argument is a compile-time constant
  const char* what = nullptr;  // what the constant is ("alphabet", "picture")
};

// The facts the compiler computed for one procedure.
struct ProcFacts {
  std::string name;
  std::string effect;      // the memory-effect verdict (ADR-264), "" when none
  std::string effectNote;  // why the verdict is not stronger
  bool nounwind = false;   // the procedure cannot unwind
  bool willReturn = false; // the procedure always returns
  std::vector<BuiltinFact> builtins;
  // The automatics an ON unit of this procedure can read, so they stay in
  // storage at the trap point (ADR-253). Pinned by the capture context.
  std::vector<std::string> pinned;
};

// --- WS-K2: the inline-vs-library decision (ADR-254) ------------------------

// The per-operation policy: which operations have an LLVM expansion, and
// which prefer it. The tables are seeded with IBM's (Tables 41/42/43,
// mvs:25226-26045).
bool hasLLVMLowering(const std::string& op);
bool prefersLLVMLowering(const std::string& op);

// The call-site decision (ADR-254). Pure: it inspects the call site and
// returns a verdict; it emits no IR.
InlineDecision decideInline(const std::string& op, const CallSite& site);

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
  void recordBuiltin(const std::string& op, const std::string& path,
                     const std::string& reason = "");
  // An automatic an ON unit can read: it is pinned in storage (ADR-253).
  void recordPinned(const std::string& name);
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

// The text of a REPEAT of a constant string a constant number of times
// (ADR-241): the repeated text is a compile-time constant, so the result
// needs neither a runtime buffer nor a copy. Empty when the repetition is
// not a foldable constant.
std::string foldRepeatText(const std::string& src, long long count);

// The bytes of the membership table of a constant character alphabet (ADR-241):
// entry c is 1 when the byte value c occurs in the alphabet, 0 otherwise. The
// table replaces a scan of the alphabet, so building it is a decision, not
// codegen — it lives here (the plan's C2).
std::string searchTableBytes(const std::string& alphabet);

// The bytes of the translation table of constant `out`/`in` strings (ADR-273):
// entry c is the character that a byte with value c maps to. `out` is padded
// with blanks to the length of `in`, the PL/I rule the inline scan already
// uses, so a match past the end of `out` maps to a blank; a character absent
// from `in` maps to itself. The first occurrence in `in` decides.
std::string translateTableBytes(const std::string& out, const std::string& in);

// WS-K3: the classification of one PICTURE specification (rules (146)-(148)).
// The classifier expands repetition factors (rule (147)) and separates the
// trailing F(n) scaling factor (rule (146)); `foldable` is true only for the
// numeric subset the constant folder below reproduces, so every other picture
// keeps the runtime interpreter.
struct PictureSpec {
  bool valid = false;     // parses as a picture specification
  bool numeric = false;   // numeric rather than character picture
  bool foldable = false;  // the simple numeric subset B4 folds
  bool hasRepeat = false; // a repetition factor was expanded
  bool hasScale = false;  // a trailing F(n) scaling factor was present
  int width = 0;          // output positions (V and F(n) emit none)
  int intDigits = 0;      // 9/Z positions before the decimal point
  int fracDigits = 0;     // 9/Z positions after the decimal point
  int point = -1;         // expanded index of V/., -1 when none
  int scale = 0;          // F(n) scaling factor
  std::string expanded;   // repetition factors expanded, F(n) removed
};

// Classify a picture string: expand repetitions, record the digit layout and
// the scaling factor, and decide whether the constant folder can reproduce it.
PictureSpec classifyPicture(const std::string& pic);

// B4: format a constant numeric value through a classified picture at compile
// time. `value` is scaled by 10^`scale`, exactly the pair the runtime
// `pli_put_edit_pic` receives. Returns false when the picture is not foldable
// or the value cannot be represented, leaving the caller on the runtime path.
bool foldPictureConversion(long long value, int scale, const PictureSpec& spec, std::string& out);

// WS-C1: the constant-offset store plan for one concatenation (ADR-242). A
// concatenation assigned to a fixed-length CHARACTER target of `cap` bytes
// holds the leftmost `cap` characters of the concatenation, blank-padded to
// `cap` when it is shorter. The plan says how many bytes each leaf
// contributes and how many blanks follow them, so the decision needs no
// backend. `leafLen` is each leaf's compile-time-constant length.
struct ConcatStorePlan {
  bool ok = false;               // the plan applies
  std::vector<long long> sizes;  // the bytes each leaf contributes
  long long used = 0;            // the bytes the leaves occupy
  long long pad = 0;             // the blank padding past the leaves
};
ConcatStorePlan planConcatStore(const std::vector<long long>& leafLen, long long cap);

// WS-C3: the merged masked test for two bit tests on one value (ADR-243).
// `(X & M1) = E1` and `(X & M2) = E2` are one test `(X & (M1|M2)) = (E1|E2)`
// when the masks are disjoint and each expectation lies inside its own mask:
// then every bit of the conjunction is decided by the merged mask alone. The
// masks and expectations are PL/I bit strings, so the decision needs no
// backend. `ok` is false when the two tests may not be merged.
struct BitTestMerge {
  bool ok = false;   // the two tests may be merged
  std::string mask;  // the merged mask
  std::string want;  // the merged expectation
};
BitTestMerge mergeBitTests(const std::string& mask1, const std::string& want1,
                            const std::string& mask2, const std::string& want2);

} // namespace pli
