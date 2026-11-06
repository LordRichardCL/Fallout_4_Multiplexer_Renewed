# FormID reference

`resources/Fallout4_reference_editorids.bin` (1.4 MB, in Git) lists the Editor IDs of the records in
`Fallout4.esm` and the six DLC masters. It is built from `resources/Fallout4_main_FormIDs.json` (Git LFS,
331 MB, an export made by another tool) by `tools/build_formid_reference.py`:

```
python3 -I tools/build_formid_reference.py resources/Fallout4_main_FormIDs.json resources/Fallout4_reference_editorids.bin
```

## What the source holds

2,192,221 rows of `plugin`, `formid` (the lower 24 bits only, no master index), `entry` and `id` (a row
number). Only 141,537 rows have a real Editor ID; the rest are placeholders such as
`[PlacedObjectBinaryOverlay_0E9610]` (the record has no Editor ID). A DLC lists both its own records and
its overrides of `Fallout4.esm` records under the same 24-bit id. There are no record types, no data and
no game version. The two `Fallout4_main_sqlite_*.json` files are SQLite bookkeeping (the row counter and
index statistics) and are not used.

## What the converter keeps

The rows with a real Editor ID, sorted by (plugin, object id), with a flag on DLC rows that are the DLC's
override of the `Fallout4.esm` record with that id (same id and Editor ID in `Fallout4.esm`), the row
count and highest id per plugin, and the SHA-256 of the source JSON. The format is described at the top
of the script and read by `src/core/reference/FormIdReference.cpp`.

## What uses it

It is read-only reference data and never decides anything about a build: Multiplexer always reads the
game's own masters from the user's install.

- **Check Definitions** compares the records this app read from each base-game master present in the load
  order with the reference (Validation, category **Reference Check**): an Info line per plugin that
  agrees, a Warning listing the first differences otherwise. Many differences usually mean another
  version of the game files; a few can point at a reading problem.
- **`fo4mx formid <plugin> <formid>`** looks up a base-game Editor ID without scanning.

The file ships next to `Fallout4MultiplexerRenewed.exe` and `fo4mx.exe`; without it the check is skipped.
