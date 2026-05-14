# Security policy

vmhook is a low-level reverse-engineering / runtime-instrumentation library.
Misuse can crash, deadlock, or destabilise the target JVM.  That said, the
library code itself can still contain memory-safety bugs that are worth
reporting privately.

## Reporting a vulnerability

Please **do not** open a public issue for a security bug.  Instead:

1. Open a private security advisory at
   <https://github.com/xxxnpno/vmhook/security/advisories/new>.
2. Include a minimal reproducer, a description of the impact, and any
   suggested fix.

You should expect a first response within a few days.  Once a fix is in
place we will coordinate disclosure (typically by merging the fix to
`master`, tagging a release, and credit-attributing the reporter in
`CHANGELOG.md`).

## Supported versions

Only the most recent minor version on `master` receives security fixes.
There are no LTS branches.  If you need a fix back-ported, please mention
that in the advisory; we'll consider it case-by-case.

## What counts as a security bug

- Memory-safety bugs (use-after-free, OOB read/write) in the library code.
- Logic bugs that allow a hooked Java method to escape the trampoline and
  corrupt the JVM.
- Misuses of OS APIs (`VirtualProtect`, `mprotect`, `mmap`) that leave
  pages in an unintended state.

## What does *not* count

- "vmhook can be used to do X" — vmhook is intentionally a debugging /
  injection library; the surface area is the threat model.
- Compatibility breaks across JDK builds.
- Crashes in code that explicitly accepts a raw `void*` from outside
  vmhook (e.g. an OOP returned by user code).
