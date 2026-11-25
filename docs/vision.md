# Vision UI (0.11.0, Gen One preview)

Vision is Coyote's GUI framework: windows are described in XML, the code behind them is a Coyote class, and the controls
are **native Windows controls** (Win32 STATIC / BUTTON / EDIT). It is the library `VisionUI` (`use "VisionUI"`), an
**optional component**: the installer offers it separately, `libs/visionui.lib` is all it needs.

Status: **Windows only** (Android comes later). Everything except the Win32 drawing was run on Linux through a headless backend
(the same code path, no screen). The Win32 backend compiles warning-free with MinGW and links into `pvm.exe`, but it has
**never been run on real Windows**: `selftest\run-selftest.bat` has a Vision step for that (see `docs/windows-verification.md`).

## A project

`cpm init MyApp --vision` creates it (needs no network; run `cpx`/`coyote` from the project folder):

```
MyApp.cyproj            requires ["VisionUI"]
appsettings.json
src/main.coy
windows/MainWindow.xml
windows/MainWindow.xml.coy
```

(The project file is `.cyproj` like every Coyote project.)

**src/main.coy**
```
use "Standard"
use "VisionUI"
use "MyApp.Windows"           # the namespace of MainWindow.xml.coy (= ns="..." in the XML)

var builder = ApplicationBuilder.Create()
builder.Configuration.Setup("appsettings.json")
var app = builder.Build()

app.Map("~/windows/MainWindow.xml", new MainWindow())   # the first mapped window is the main window

app.Run()
```
`~` is the folder of `appsettings.json` (the working directory when no settings file was loaded). Run from the project folder:
`coyote run src/main.coy`.

**appsettings.json** - JSON, read by the small forgiving reader in VisionUI (comments, a missing comma and trailing commas are accepted):
```
{ "Targeting": { "Platform": "Windows" }, "Logging": { "Level": "Information" } }
```
`Targeting.Platform` must be `Windows` (anything else is an error for now). `Logging.Level`: Trace, Debug, Information, Warning, Error, None
(`app.Log.Info(...)` etc. print `[info] ...`). Read your own values with `app.Configuration.Get("A.B", "default")`, `GetInt`, `GetBool`.

**windows/MainWindow.xml**
```
<Window ns="MyApp.Windows" name="MainWindow">
    <Meta>
        <Basic title="Main Window" width="480" height="320" />
        <Import src="VisionUI.Packages.Designs.Material" />
    </Meta>
    <Label text="Hello World" />
    <Label id="counterLabel" text="You clicked $Model.Counter times" visible="false" />
    <Button text="Click Me" onClick="ButtonClicked" attach="counterLabel" />
</Window>
```

**windows/MainWindow.xml.coy**
```
namespace MyApp.Windows
use "Standard"
use "VisionUI"

public class MainWindow : Window {
    constructor() {
        here.Model.Set("Counter", 0)
        here.Handle("ButtonClicked", (EventArgs e) : EventResults => here.ButtonClicked(e))
    }
    method ButtonClicked(EventArgs e): EventResults {
        int n = here.Model.GetInt("Counter") + 1
        here.Model.Set("Counter", n)
        e.Attached[0].Visible = true
        return EventResults.Handled
    }
}
```

### Why it differs from the first sketch
The sketch used `here.Counter++` on a property and `onClick` as a method name. Coyote has no reflection (the language is frozen), so the XML
cannot look up a property or a method by name. Vision therefore uses two small, explicit tables instead:
* **`Model`** (`ViewModel`): named values. `$Model.Counter` in the XML reads `Model.Get("Counter")`; `Model.Set` refreshes the window.
* **`Handle(name, fn)`**: connects the name in `onClick="ButtonClicked"` to code. Alternative: override `OnEvent(string handlerName, EventArgs e)` and `switch` on the name.

Reflection-based binding (`here.Counter` directly) is a possible Gen One language addition; it needs your decision because the language is frozen.

## The XML
Root `<Window ns="..." name="...">`. Children: `<Meta>` then controls.

| Element | What | Attributes |
|---|---|---|
| `<Meta><Basic title width height />` | window title and client size (default 480 x 360) | |
| `<Meta><Import src="..." />` | design: `VisionUI.Packages.Designs.Default` (system look), `.Material`, `.Dark` | |
| `<Label>` | text | `text` |
| `<Button>` | push button | `text`, `onClick` |
| `<TextBox>` | one-line edit box | `text`, `onChange` |
| `<CheckBox>` | check box | `text`, `checked`, `onChange`, `onClick` |
| `<StackPanel orientation="vertical\|horizontal" spacing="8">` | layout container, may nest | |

Common attributes: `id` (find it with `window.Find("id")`), `visible`, `enabled`, `width` (pixels or `stretch`), `height`, `attach="id1,id2"`
(the controls arrive in `e.Attached[0]`, `e.Attached[1]` ...). The text of a `<Button>x</Button>` element can also be its inner text.
Unknown elements, unclosed tags, unknown `attach` ids and non-numeric sizes are errors with `file: line N: message`.

### Bindings
In `text`, `visible`, `enabled` and `checked`, `$Model.Name` is replaced by `Model.GetString("Name")` (several per text are allowed). A **text box whose
text is exactly `$Model.Name`** is two-way: typing writes the model; a **check box with `checked="$Model.Flag"`** likewise. `visible="$Model.Show"` accepts a boolean or `"true"`.

### Layout
No coordinates: controls are placed top to bottom with the design's padding and spacing. Labels, text boxes and check boxes stretch across the
window; buttons are 140 px wide (or as wide as their text) unless `width` says otherwise; hidden controls take no room. A horizontal `StackPanel`
uses automatic widths (label: 8 px per character, button 120+, text box 200). Resizing the window lays it out again. All sizes are device-independent pixels (96 dpi); the Win32 backend scales them.

### Designs
`Default`: the system look. `Material` (light gray-white, purple accent, Segoe UI 10) and `Dark`: window background, text colour and font are set,
and **buttons are drawn flat in the accent colour** (owner-drawn, white text, darker while pressed). Labels and check boxes follow the colours; the edit box stays white.
A design is chosen when the window is created; changing it later is not supported.

## API
* `ApplicationBuilder.Create()`, `.Configuration` (`Setup(path)`, `SetupText(json)`, `Get/GetInt/GetBool`, `BaseDirectory`), `.Build()`.
* `Application`: `Map(path, window)`, `MapText(xml, sourceName, window)`, `Prepare()` (create + show the main window), `Run()` (event loop; returns 0), `Open(name)`, `WindowNamed(name)`, `Dispatch(event)`, `Log`, `Configuration`.
* `Window`: `Model`, `Find(id)`, `Handle(name, fn)`, `OnEvent`, `OnClosing(e)` (return `EventResults.Cancel` to keep the window), `Close()`, `Refresh()`, `Title`, `Width`, `Height`, `Dump()`.
  `onClosing="Name"` on the `<Window>` element names a handler for closing.
* `Control`: `Id`, `Kind`, `Text`, `Visible`, `Enabled`, `Checked` (properties), `Click()`, `Type(text)` (simulate the user).
* `EventArgs`: `Source`, `Sender`, `Name` (`click`, `change`, `closing`), `Value` (new text / "true"/"false"), `Attached`, `Find(id)`. `EventResults`: `Handled`, `Unhandled`, `Cancel`.
* A handler that throws is logged (`[error] window X: handler 'Y' failed: ...`) and the program continues; an unknown handler name logs a warning.

## The `ui` host module
Permission `ui` (granted by `coyote run`, Studio and `pvm --allow-all`). Use it through `UiHost` (VisionUI) only; it is the thin layer under the library:
`windowCreate/Show/Close/SetTitle/SetTheme/ClientSize`, `controlCreate/SetText/GetText/SetVisible/GetVisible/SetEnabled/GetEnabled/SetChecked/GetChecked/SetBounds/Bounds`,
`nextEvent(waitMs)`, `simClick/simSetText/simClose`, `backend()`.
Events are TAB-separated strings: `click<TAB>ctl`, `change<TAB>ctl`, `close<TAB>win`, `resize<TAB>win<TAB>w<TAB>h`, `quit`.

* **Win32 backend** (Windows): real top-level windows; `nextEvent` pumps messages (tab navigation works; DPI aware, sizes scaled; the exe manifest gives modern control styling).
  `WM_CLOSE` becomes a `close` event and the program decides (`Window.Close`). `nextEvent` returns `quit` when the last window is gone.
* **Headless backend** (every other system, or Windows with `COYOTE_UI=headless`): the same model without a screen; `nextEvent` returns `quit` as soon as no event is pending,
  so `app.Run()` ends by itself. This is what the regression tests use (`tests/cases/vision_*.coy`).
* The `ui` hosts are **not native yet**: `coyote-llvm` builds Vision programs as a bundled-PVM executable (with the usual note).

## Not there yet (Gen One, in this order of likelihood)
Android; timers/`async` work from UI code (one thread, so a long handler freezes the window); more controls (list, combo box, image, menu, tabs, grid layout); dialogs from XML;
reflection-style binding; styles in files; compile-time check of handler names; Studio designer and templates in the Studio "New project" dialog; the new Visual-Studio-style installer
(this version only adds a plain "Vision UI" component to the existing installer).
