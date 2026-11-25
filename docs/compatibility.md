# Compatibility policy (from 0.8 on)

## Language

* **Until the freeze (0.8 → 0.10)** breaking changes were allowed and every one came with a migration script (`tools/migrate-*.py`).
  0.8 is the last release that changes what existing programs mean. 0.9 (native on the frozen language) and 0.10 (library) add features
  and libraries without changing the grammar of [grammar.md](grammar.md).
* **After the freeze**, within generation 1 (1.x): a program that compiles and runs on 1.0 compiles and runs the same on every 1.x. New
  syntax may only be added where the old grammar had no meaning for it (a new keyword is never added to the reserved list; new keywords
  are contextual). Bug fixes that make an incorrect program fail are allowed and are listed in the release notes.
* **Standard library**: functions and classes of `Standard*` libraries are never removed or changed incompatibly within 1.x; they can be
  added, or deprecated (a deprecated name keeps working and is reported by `coyote check` as a warning).
* **Diagnostics** (the text of error messages) are not part of the contract; their `file:line:column` prefix is.
* **Runtime errors**: the first line of an uncaught error (`<file>:<line>: runtime error: <message>`) and the exit code 1 are stable; the
  `  at ...` trace lines follow the format `  at <Method> (<file>:<line>)` but the elision of very long stacks may change.

## ML (the middle language the compiler emits and the PVM runs)

* ML files carry a version number in their header. The PVM **keeps accepting every ML version it has ever shipped** (currently 1 to 12): an
  `.ml` file built with an older compiler keeps running on a newer PVM.
* A new PVM instruction raises the version only for files that use it, so a program that does not need it still runs on an older PVM.
* A *newer* ML on an older PVM is refused with a clear message (`needs a newer PVM`), never run partially.
* The ML text format of released versions is frozen; only new versions add instructions (see [ml-spec](ml-spec.md)).

## Native backend

`coyote-llvm` output must behave exactly like the PVM (same stdout, stderr and exit code); a difference is a bug fixed in a hotfix release
(`X.Y.Z.9`). Programs outside the native subset are bundled with the PVM instead of being refused.
