# cpm — package manager

`cpm` resolves the `dependencies` of `packages.json`, writes a deterministic lock file
(`packages.lock.json`) and installs the packages into `.coyote/packages/<name>/`.

**Status:** local (`path`) and `git` dependencies work. `url` and registry dependencies are parsed
and validated, but their providers are explicit interfaces that fail with a clear message — there is
no HTTPS client and no registry yet. Installed packages are used from Coyote code with
`import "<name>"`: the compiler looks in `.coyote/packages/<name>/` and compiles the package's `entry`
(see `language.md`, *Modules*). Only methods declared with `export method` are visible to importers.

## Commands

```
cpm [-C dir] install [--locked]   install what the lock file says (resolving first if it is missing or stale)
cpm [-C dir] update               re-resolve everything, rewrite the lock file, install
cpm [-C dir] lock                 re-resolve and write the lock file; install nothing
cpm [-C dir] list                 list locked packages
cpm [-C dir] verify               compare installed files with the lock (exit 1 on any difference)
cpm [-C dir] init [name] [--structured]
cpm [-C dir] layout --structured [--apply]
```

Exit codes: 0 success; 1 the operation failed (resolution, provider, lock, install); 64 usage
error or invalid `packages.json`.

There is no `cpm add`: edit `packages.json` by hand (cpm never rewrites your manifest).

## Dependencies in packages.json

```json
"dependencies": {
  "util":  { "path": "../libs/util" },
  "tools": { "git": "https://example.com/tools.git", "rev": "v1.4.0" },
  "core":  { "path": "../core", "version": "1.0.0" }
}
```

| Form | Meaning |
|------|---------|
| `{"path": dir}` | a local directory with its own `packages.json`; `dir` is relative, `/`-separated |
| `{"git": url, "rev": ref}` | a git repository; `rev` is a branch, tag or commit (required) |
| `{"url": https-url, "sha256": hex}` | *not implemented* |
| `"1.2.3"` | a registry package — *not implemented* |
| `"version": "x.y.z"` (optional, on path/git) | the package must declare exactly this version |

The key is the package name and must equal the `name` in the package's own `packages.json`.
Version requirements are **exact**, not ranges.

## Resolution

* Packages are found transitively from each package's own `dependencies`. Relative `path`s are
  relative to the package that declares them.
* There is one copy of each name. If two packages need the same name from **different sources**
  (or different versions), resolution fails and names both requesters; nothing is guessed.
  Dependency cycles between packages are allowed.
* Only packages that are themselves local paths may use `path` dependencies.
* The result does not depend on ordering or on the machine: packages are sorted by name, and
  nothing in the lock file is a timestamp or an absolute path.

## Lock file

`packages.lock.json` records, per package: version, source (path; or git url, requested `rev` and the
**exact commit**), an **integrity** hash and its dependency names, plus the root `requested`
specs. Integrity is SHA-256 over the package's files (sorted relative paths, sizes and contents;
`.git` and `.coyote` are skipped; symbolic links, more than 20,000 files or more than 256 MiB are
rejected).

* `install` uses the lock when its `requested` section matches `packages.json`; it fetches the
  locked sources (git: the locked commit, not the moving tag) and **fails if any package's
  integrity differs**. Changing a source package on purpose needs `cpm update`.
* `install --locked` never changes the lock; it fails when the lock does not match the manifest.
  Use it in CI.
* Installed packages not in the lock are removed from `.coyote/packages` (that directory belongs
  to cpm).

## Git safety

cpm runs the system `git` (it must be installed). It disables credential prompts, allows only the
`file`, `https`, `ssh` and `git` transports (so a URL such as `ext::sh -c ...` cannot run
commands), and rejects `git`/`rev` values that start with `-`. Only files are kept; the `.git`
directory is deleted.

## Project layout commands

* `cpm init [name]` creates `packages.json` and a Hello-world `app.coy` (a *vanilla* project).
  `--structured` also creates `src/`, `modules/`, `bin/`, `appsettings.json` and workflows that
  build into `bin/`. It never overwrites an existing file.
* `cpm layout --structured` lists the root `.coy` files (other than the entry) that would move into
  `src/`; `--apply` performs the move. Moving files never happens as a side effect of any other
  command. Imports are not rewritten (there are none yet).
