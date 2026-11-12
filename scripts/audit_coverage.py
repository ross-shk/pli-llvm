#!/usr/bin/env python3
"""
Audit script: cross-reference GRAMMAR-COVERAGE.md with actual test results.
Outputs a matrix: rule, alternative, status, test evidence, IR inspection needed.
"""
import re
import subprocess
import sys
from pathlib import Path

GRAMMAR_COVERAGE = Path("docs/GRAMMAR-COVERAGE.md")
TESTS_DIR = Path("tests")

def parse_coverage():
    """Parse GRAMMAR-COVERAGE.md rule table."""
    content = GRAMMAR_COVERAGE.read_text()
    rules = []
    in_table = False
    for line in content.splitlines():
        if line.startswith("| Rules | Feature | Status |"):
            in_table = True
            continue
        if in_table and line.startswith("|---"):
            continue
        if in_table and line.startswith("| ("):
            parts = [p.strip() for p in line.split("|")[1:-1]]
            if len(parts) >= 4:
                rules.append({
                    "rule": parts[0],
                    "feature": parts[1],
                    "status": parts[2],
                    "evidence": parts[3]
                })
        elif in_table and line.startswith("| §"):
            # Also capture §2.3.x rows
            parts = [p.strip() for p in line.split("|")[1:-1]]
            if len(parts) >= 4:
                rules.append({
                    "rule": parts[0],
                    "feature": parts[1],
                    "status": parts[2],
                    "evidence": parts[3]
                })
        elif in_table and not line.startswith("|"):
            break
    return rules

def run_test(test_path):
    """Run a single test and return pass/fail."""
    try:
        result = subprocess.run(
            ["python3", "tests/run_tests.py", test_path],
            capture_output=True, text=True, timeout=60, cwd="."
        )
        return result.returncode == 0, result.stdout
    except Exception as e:
        return False, str(e)

def main():
    rules = parse_coverage()
    print(f"# Coverage Audit - {len(rules)} rules")
    print()
    print("| Rule | Feature | Status | Test Evidence | Test Result | IR Check Needed |")
    print("|------|---------|--------|---------------|-------------|-----------------|")
    
    passed = 0
    failed = 0
    not_run = 0
    
    for r in rules:
        test_paths = re.findall(r'(?:tests?/|driver/)[\w/_.-]+\.pli', r["evidence"])
        test_result = "NOT RUN"
        for tp in test_paths[:1]:
            ok, _ = run_test(tp)
            test_result = "PASS" if ok else "FAIL"
            if ok:
                passed += 1
            else:
                failed += 1
            break
        else:
            not_run += 1
        
        ir_check = "YES" if any(k in r["feature"].lower() for k in 
            ["irgen", "codegen", "lowering", "abi", "descriptor", "static", "dynamic", 
             "emit", "global", "alloca", "format", "picture", "record", "task", "condition"]) else "NO"
        
        feat = r['feature'][:60].replace("|", "\\|")
        evid = r['evidence'][:70].replace("|", "\\|")
        print(f"| {r['rule']} | {feat} | {r['status']} | {evid} | {test_result} | {ir_check} |")
    
    print()
    print(f"## Summary")
    print(f"- Rules audited: {len(rules)}")
    print(f"- Tests run: {passed + failed}")
    print(f"- Passed: {passed}")
    print(f"- Failed: {failed}")
    print(f"- Not run (no test path found): {not_run}")

if __name__ == "__main__":
    main()