# Testing (0.8.0)

The repository carries a small, fast regression suite in `tests/`. It runs the real `coyote` and `pvm` binaries, so build first
(`cmake -S . -B build && cmake --build build`), then:

```
tests/run.sh build                    # every case on the PVM + every error case
tests/run.sh build --gc-stress        # + each case again with the garbage collector forced before every allocation
tests/run.sh build --native           # + PVM-versus-native comparison (tools/diff-native.sh; needs clang)
tests/run.sh build ref                # only tests whose name contains "ref"
tests/run.sh build --bless            # rewrite the expected .out files from the current output (review the diff!)
```

The exit code is the number of failed tests. CI: `.github/workflows/tests.yml` (new in 0.7.0, separate from `ci.yml`) builds and installs Coyote on `windows-2022` and `ubuntu-24.04` and runs the suite plain and with `--gc-stress`;
it has **not run yet** (nothing was run on GitHub Actions or real Windows by the author). Line endings are ignored when comparing output, `.gitattributes` keeps `tests/` on LF.

| Folder | Files | A test passes when |
|---|---|---|
| `tests/cases/` | `name.coy`, `name.out`, optional `name.in` (stdin), optional `name.rc` (expected exit code, default 0) | `coyote run` exits 0 and prints exactly `name.out` (and, with `--gc-stress`, the same output under forced collection) |
| `tests/projects/<name>/` | `main.coy` (+ other files), `expected.out` | `coyote run main.coy` prints exactly `expected.out` (multi-file programs: namespaces, qualified names) |
| `tests/errors/` | `name.coy`, `name.err` | `coyote check` fails and every non-empty line of `name.err` appears in its diagnostics |

Current cases: `basics`, `native_basic` (inside the native subset; `--native` compares PVM and native output), `classes` (inheritance, interfaces, shared fields, properties),
`generics_lambdas`, `errors_try`, `collections_gc` (linked list, maps; heavy allocation), `constructors` (overloading), `overloads`, `ref_copy` (`(copy)`/`(ref)`),
`mouths` (host variables; exits with code 3), `encoding` (Standard.Encoding), `regex` (Community.Text), and since 0.7.0 `core_types` (long, byte, bitwise, literals, strings, `array<byte>`), `core_types_rt` (checked conversions and overflow),
`dyn_long`, `bytes_files` (`File.ReadAllBytes/WriteAllBytes`, `Encoding.ToBytes/FromBytes`, long time/size values) and `enum_switch`. since 0.9 `native_scalars`, `native_arrays` and `native_classes`, `native_maps`, `native_closures`, `native_exceptions`, `native_strings`, `native_obj`, `native_dyn`, `native_files`, `native_random`, `native_runtime`, `lib_sort_time` (inside the native subset, so `--native` compares PVM and native), since 0.8.0 `exceptions` (Exception classes, typed catch, finally, traces), `closures_shared`, `globals`, `object_protocol` and the project `qualified`. Error cases: `exceptions`..`exceptions4`, `constructors`, `ref_copy`, `mouths`, `mouths_syntax`, `core_types`, `enum_switch`, `enum_dup`.

Rules for new tests: keep them deterministic (no clock, no random numbers, no machine-specific paths), small and fast; a new language feature gets one case plus one
error case for its diagnostics; bless the `.out` only after reading the output yourself. The suite does not cover Windows-only libraries, Studio or the VS Code extension.

## Verification apps (`selftest/`, since 0.10.1)

`selftest/run-selftest.bat` (or `.sh`) runs everything that does not need a person and writes `selftest-report.txt`: the conformance suite on the PVM, with
GC stress and as native executables, the self-test apps (`SelfTest` = native subset, run as PVM and native and compared; `SelfTestSystem` = processes, Windows
features, DLL calls, HTTP; `SelfTestUi` = dialogs, you answer questions) and the `cpx` project flow. See [windows-verification.md](windows-verification.md).


## Vision UI tests (0.11.0)
`tests/cases/vision_parsers.coy` (JSON/XML readers with their error messages, settings, `$Model` bindings, designs) and `tests/cases/vision_window.coy` (a window from XML on the headless backend: layout dump,
two-way text box and check box, enabled/visible bindings, `attach`, handler fallback `OnEvent`, a throwing handler, cancelled/accepted closing, window-file errors, unsupported platform).
`selftest/SelfTestVision` is the same idea on the real backend (Windows) and is part of `selftest/run-selftest.bat`.
