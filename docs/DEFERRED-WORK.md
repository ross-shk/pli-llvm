# Deferred work registry

Tracks PL/I forms that are intentionally not yet served, with the condition
under which each may be promoted to implemented. A row here is not a
rejection of the feature; it is a gate that must be satisfied before the
diagnostic is lifted (invariant 2: every unsupported form stays diagnosed
with its rule number until then). Rows are immutable once a gate is recorded;
promote by satisfying the gate and moving the row out of this file.

| ID | Feature | Reference | Promotion gate |
|----|---------|-----------|----------------|
| B9 | External `ENTRY RETURNS(NAME)` returning structure through caller's hidden sret buffer | `c_struct_return.pli` | sret buffer lowering validated + IR check passes |
| C6 | Recycled task fiber with two+ CONTROLLED variables starts clean | `task_ctl_reuse_multi.pli` | task fiber reset clears CONTROLLED lookup cache |
| A11 | FINISH not copied to subtasks (deliberate deviation, ADR-322) | `task_alloc_combo.pli` | documented deviation; no code change needed |
