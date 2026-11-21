# Logl 2.0 – What's new

## Files (`Host:`)
- New operations: `Copy`, `Move` (`To;`), `Lines` (→ list), `WriteLines` (`Value;list`), `Stat` (→ dictionary with exists/size/isdir/mtime), `IsDir`, `Rmdir`, `Cwd`.
- `SaveJson` / `LoadJson` store and load lists, dictionaries, texts and numbers.
- `List` supports `Mark;1` (folders end with "/"), `Filter;.ext`, `Hidden;1` and `File;subfolder`.
- `Write` / `Append` accept `Raw;1` (keeps `\n` as typed; required to save source code).
- `Exec` now resolves relative paths from the program's folder, like `Host:`.

## 3D (`G3D:`)
- Shapes: cube, sphere, torus, pyramid, cylinder, cone, plane, diamond. Real z-buffer (intersecting objects render correctly).
- `Object` (`Mesh;name`), `Load` (`Name;x File;a.obj Fit;1`), `Save` (`File;scene.obj`), `Remove`, `Info`, `List`, `Meshes`.
- `Pick` (`X;Y;` → id of the object under that pixel), `Project` (→ `[x, y, depth]`), `Orbit` (`Yaw/Pitch/Dist` or `DYaw/DPitch/Zoom`).
- `Scene`: `Grid;1`, `Ambient;0..1`. Create/`Set`: `SX/SY/SZ`, `Visible;0|1`, `Mesh`. `Id` accepts `{variables}`.

## Interface (`UI:`)
- `TextArea` is a code editor: line numbers, Tab / Shift+Tab, auto-indent, `Syntax;log` highlighting, `Rows`, `Cols`, `Goto;n`.
  Ctrl+S/O/N/F/R and F5 reach `On:Key` as "Ctrl+S", "F5", etc.
- Also: `Dropdown` / `List` (`Options;a|b|c`), `Progress`, `Title`, `Separator`, `Break`.
- `UI:Get Id;x [What;value|cursor|options] Into;v` — cursor = `[line, column, selStart, selEnd]`.
- Large texts (up to 8 MB) travel through `POST /put`.

## Low level
- `Mem:` — `Find`, `Cmp`, `Hash` (CRC-32), `Swap`, `ReadText`, `SetBit` / `ClearBit` / `ToggleBit` / `TestBit`, `Xor` / `And` / `Or` / `Not`.
- `Math:` — `popcount`, `clz`, `ctz`, `wrap8/16/32`, `sext8/16/32`, `bswap16`, `clamp`, `lerp`.

## Examples
- `logedit.log+` — a code editor for Log written in Log: open, save, run, folders.
- `demo3d.log+` — interactive 3D demo.
- Build: `gcc -O2 -static -o logl logl.c -lm -lpthread`

---

# HTML simulator (`index.html`) – compatible with Logl 2.0

The web simulator now has **the same new commands** as `logl.c` 2.0 and produces the same results
(verified by running the same programs in both: same CRC-32, same search positions, same 3D projection,
same exported OBJ: 517 vertices / 889 faces).

| Area | What the simulator has |
|---|---|
| `Host:` | Copy, Move, Lines, WriteLines, Stat, IsDir, Rmdir, Cwd, Size, Append, SaveJson, LoadJson, `List` with Mark/Filter/Hidden/File, `Raw;1` |
| `G3D:` | cylinder, cone, plane, diamond, Object, OBJ Load/Save, z-buffer, Pick, Project, Orbit, Info, List, Meshes, Remove, SX/SY/SZ, Visible, Ambient, Grid, `Id` with `{variables}`, named colors |
| `UI:` | TextArea (editor with line numbers, Tab, auto-indent, highlighting, Ctrl+S/O/N/F/R and F5 → `On:Key`), Dropdown, List, Progress, Title, Separator, Break, `UI:Get` (value, cursor, options) |
| `Mem:` / Math | Find, Cmp, Hash, Swap, ReadText, SetBit/ClearBit/ToggleBit/TestBit, Xor/And/Or/Not; popcount, clz, ctz, wrap8/16/32, sext8/16/32, bswap16, clamp, lerp |
| `Screen:` | `Screen:Show()` (no-op in the simulator; the canvas is already live) |

## Moving files between the simulator and the interpreter
- PC → simulator: `upload` command (.log+, .obj, .json…).
- Simulator → PC: new `download <file>` command.
- Programs are the same plain text in both; saved OBJ and JSON files are standard.

## Differences worth knowing (by design of the simulator)
- **Writes** (`Host:Write/Append/Copy/Move/Delete/Mkdir…`, `G3D:Save`) require kernel mode: `run # program.log+`.
- In kernel mode `Exec` reads from the virtual disk by default: use `"From;host"` to run files from the simulator's
  file system (this is how `logedit.log+` works the same in both).
- `Host:List` without options keeps its old behavior (all files with their paths). With `File`, `Mark`,
  `Filter` or `Hidden` it behaves like `logl.c` (single-folder listing).
- To use UI and 3D you must install the extensions: `pkg install UIVisuals` and `pkg install Graphics3D`.

---

# Split (new, in both `logl.c` and `index.html`)
`Split:parts("Value;text","By;,")` or `Split("Text;a b c","Into;parts")` cuts a text into a list
(the destination variable must be declared: `Var:parts("List;")`, or `Var:STR:parts("List;")` in Log-).
Options: `By` / `Sep` / `On` = separator (`\n` and `\t` allowed; with none, it splits on runs of spaces),
`Limit;n` = at most n pieces, `Trim;1` = trim every piece, `Skip;1` = drop empty pieces.
