<!--
Thanks for the contribution.  A few things help reviewers move quickly:

  1. Describe *why* the change is needed, not just what it does — the diff
     already shows the "what".
  2. Include the matrix you tested.  CI covers Windows × {MSVC, Clang, MinGW},
     Linux × {GCC, Clang}, macOS × Clang, plus iOS/Android cross-compile and
     JVM-integration runs against Java 8/11/17/21/24/25.  If your local tests
     covered a subset, list which.
  3. If the change touches the public header, add a CHANGELOG.md entry under
     the [Unreleased] section.

Delete this comment block once you're done.
-->

## Summary

<!-- 1–3 sentences on what changes and why.  Link to issues with `Closes #N`. -->

## Test plan

- [ ] CI passes locally (`cmake --build && ctest`)
- [ ] JVM integration: `injector.exe` + Main on the JDK versions I care about (list which)
- [ ] If header-public: CHANGELOG.md updated
- [ ] If header-public: README API section updated when needed

## Risk

<!-- What's the worst-case for downstream consumers?  E.g.:
     - Breaks no public API
     - Changes one field-watch callback signature; old callers still compile
     - Drops support for X
-->
