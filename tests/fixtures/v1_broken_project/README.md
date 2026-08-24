# V1 acceptance fixture

This tiny C++ project contains one intentional implementation bug.

Acceptance workflow:

1. Configure and build with CMake.
2. Run CTest before editing and observe the failing test.
3. Diagnose and repair the implementation without weakening the tests.
4. Create `FIX_REPORT.md` summarizing the root cause and verification commands.
5. Rebuild and run CTest after the latest edit; all tests must pass.

Do not change the public declaration or the test expectations.
