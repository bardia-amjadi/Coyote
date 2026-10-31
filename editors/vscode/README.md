# Coyote for VS Code

Language support for Coyote (`.coy`): syntax highlighting, snippets, bracket/comment configuration,
**diagnostics** (from `cdt check --json`, refreshed when a file is opened or saved) and
**formatting** (`cdt fmt`, via *Format Document*).

All analysis is done by `cdt`; this extension only connects it to the editor. Install the Coyote
toolchain so that `cdt` is on your `PATH`, or set `coyote.cdtPath`.

## Settings

| Setting | Default | Meaning |
|---------|---------|---------|
| `coyote.cdtPath` | `cdt` | path to the `cdt` executable |
| `coyote.diagnostics.enable` | `true` | show compiler errors in the editor |

## Limits

* Diagnostics reflect the **saved** file, not unsaved edits (cdt reads files).
* There is no language server: no completion, hover or go-to-definition.
* The extension has been unit-tested (diagnostic mapping, grammar scopes with the real TextMate
  engine) but **never loaded in a running VS Code** from this build environment.

## Developing

```
npm install --no-save vscode-textmate@9.0.0 vscode-oniguruma@2.0.1   # test-only packages
npm test
npx @vscode/vsce package          # builds coyote-0.1.0.vsix
```
