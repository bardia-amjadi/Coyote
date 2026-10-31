# cpx — project executer

`cpx` runs the **workflows** declared in a project's `packages.json`. A workflow is an ordered
list of steps; each step runs a program (never through a shell) or another workflow.

## Commands

```
cpx [-C dir] [-q] <workflow>     run a workflow (for example: start, build)
cpx [-C dir] [-q] run <workflow> the same, for names that clash with commands
cpx [-C dir] list                list workflows ("(default)" marks synthesized ones)
cpx [-C dir] check               validate packages.json
```

`-C dir` uses the project in `dir`; `-q` suppresses the `cpx: <workflow>> <command>` lines that
are otherwise printed to **stderr** before each step. cpx looks for `packages.json` only in the
project directory (it does not search parent directories).

## Semantics

* Steps run in order, in the project directory, with cpx's own stdin, stdout and stderr, so the
  program's output appears unmodified. cpx's own messages go to stderr.
* The first step that fails stops the workflow. **cpx exits with that step's exit code.** If the
  step was killed by a signal the code is 128 + the signal number.
* Exit codes cpx uses itself: **64** for usage errors and invalid `packages.json`; **127** when a
  step's program cannot be started. (A step may of course also exit with these numbers.)
* Programs are found first next to the `cpx` executable (so an installed toolchain's `coyote` and
  `pvm` are used even if they are not on `PATH`), then on `PATH`.
* There is no shell: no globbing, no variable expansion, no pipes. Use the array form to pass
  arguments that contain spaces.

## packages.json (schema version 1)

```json
{
  "schemaVersion": 1,
  "name": "demo",
  "version": "0.1.0",
  "entry": "app.coy",
  "workflows": {
    "build": ["coyote build app.coy -o bin/app.ml"],
    "start": ["@build", ["pvm", "bin/app.ml"]]
  },
  "dependencies": {}
}
```

| Key | Required | Meaning |
|-----|----------|---------|
| `schemaVersion` | yes | must be `1` |
| `name` | yes | 1–64 characters: letters, digits, `_`, `-`, `.` |
| `version` | no | `MAJOR.MINOR.PATCH` |
| `entry` | no | relative `.coy` path using `/`, no `..`; default `app.coy` |
| `workflows` | no | object: name → step or array of steps (at most 256) |
| `dependencies` | no | validated by cpx, resolved and installed by [`cpm`](cpm.md); cpx itself does not install them |

Unknown keys are errors. The file must be strict JSON (no comments or trailing commas, no duplicate
keys, at most 64 levels of nesting, at most 1 MiB). Errors are reported as
`packages.json:<line>:<column>: error: ...`.

A **step** is a string or an array of strings:

* `"@name"` runs workflow `name`. References to unknown workflows and cycles are rejected by
  `cpx check`, and by every run.
* `"program arg arg"` is split on spaces and tabs (no quoting; use the array form for that).
* `["program", "arg one", "arg two"]` is an exact argument list.

A workflow may be a single step instead of an array. `list`, `check` and `run` cannot be workflow
names.

## Default workflows (vanilla projects)

A project with just `app.coy` and a minimal `packages.json` works with no `workflows`:

* `build` creates `.coyote/out/` and runs `coyote build <entry> -o .coyote/out/<name>.ml`
* `start` runs `build`, then `pvm .coyote/out/<name>.ml`

Declaring `build` or `start` yourself replaces the default. **Source files are never moved,
renamed or rewritten**; the only thing cpx creates on its own is the `.coyote/out` build directory.
There is no structured-layout generator or migration command yet.

## Not implemented

Passing extra arguments through to a step, environment variables per step, parallel steps,
conditional steps and watching files. The process-spawning code has a Windows implementation
that has **not been compiled or run** (no Windows machine here); CI is where it will first be built.
