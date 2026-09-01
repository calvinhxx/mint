# Broken calculator fixture

This tiny C++ project contains one intentional implementation bug.

The regression runner proves the failing baseline before starting the agent. The agent workflow is:

1. Read this file and `src/calculator.cpp`.
2. Repair the implementation without weakening the tests.
3. Create `FIX_REPORT.md` summarizing the root cause and verification commands.
4. Run the configure, build, and test recipes after both edits; all tests must pass.
5. After the verification recipe passes, stop using tools and return the final summary.

Do not repeat the baseline, run commands before both edits, or change the public declaration or test expectations.
