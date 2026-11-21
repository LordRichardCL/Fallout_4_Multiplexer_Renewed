# Compiling New Cells with Precombine and Previs Data (design)

Status: PC1 (the read-only check) is implemented; PC2 and PC3 are proposals. It builds on facts already established on real load orders
(`RECORD_HANDLING_AUDIT.md`, stages C2, P1, P2a and P2b, and the Version Control Info findings).

## The problem

A plugin that adds a **new cell** with precombined meshes or previs data stays in the load order
(**Precombined Cell**). It is the largest group of automatic keep-outs left (34 plugins in build 73, most
of the 64 kept out automatically in build 76). Overrides of existing cells already compile: their
references are translated in `XCRI`, and their previs files are translated into the output's numbering
(P2a, P2b).

A new cell is different because compiling gives it a **new FormID in an output plugin**. The game finds a
cell's precombined meshes and previs file by the **cell's defining plugin and FormID**, so after compiling
it looks under the output's name and the new FormID, where nothing exists.

## What the files are (established)

| File | Path | Holds FormIDs? | Source |
|---|---|---|---|
| Precombined mesh | `meshes\precombined\<cell's plugin>\<cell FormID>_<mesh hash>_OC.nif` | No. Geometry, materials, texture paths; its root node is named after the file | C2, P1 |
| Precombined physics | `...\<cell FormID>_<mesh hash>_Physics.nif` | Assumed as above; to be confirmed with one sample | — |
| Previs | `vis\<cell's plugin>\<cell FormID>.uvd` | Yes: an ID table of reference FormIDs in the numbering of the plugin whose cell record wins; translation exists (P2b) | P1, P2b |
| "Clean" precombine geometry | `<plugin> - Geometry.csg`, `<plugin>.cdx` in Data | Unknown; loaded only while the plugin is active, and stores a hash of its name | Filename references |
| Cell record | `XCRI` (meshes and their references), `XPRI` (previs references), `PCMB`, `VISI`, `RVIS` | Yes; already translated and sorted | W2, P2a |

The mesh hash in the file name is the value `XCRI` lists for the mesh. Compiling does not change it.

## Proposal

For a new cell C of plugin P, compiled into output O with the new FormID C′:

1. **Precombined meshes.** Copy each `meshes\precombined\<P>\<C>_<hash>_OC.nif` (and `_Physics.nif`) to
   `meshes\precombined\<O>\<C′>_<hash>_...nif` in the output folder. The root node's name holds the
   8-digit FormID. Change it to C′ as a same-size substitution in the NIF string table, and read it back
   before use, as previs copies are. The mesh hash stays.
2. **Cell record.** `XCRI` and `XPRI` are already written with the references' new FormIDs, sorted.
   `PCMB`, `VISI` and Version Control Info are kept as the source has them, so the game's timestamp
   comparison sees what it sees today.
3. **Previs.** Copy `vis\<P>\<C>.uvd` to `vis\<O>\<C′>.uvd` with its ID table translated into O's
   numbering. This reuses P2b: same-size substitution, read back, listed in `Multiplexer_Previs.tsv`.
4. **Manifest.** Every copy is listed in `Multiplexer_Precombined.tsv` (output file, source file, content
   hash, cell), like the voice, FaceGen and billboard relocations. A later export removes copies it no
   longer makes, and Switch back removes them with the output.

The originals stay installed with their mod. The game no longer reads them once P is switched off.

## What stays out (fail closed)

- **Plugins with "clean" precombines** (`<P> - Geometry.csg` or `<P>.cdx`): the meshes take their
  geometry from a file loaded only while P is active, and the format is undocumented.
- **Exterior cells whose previs cluster (`RVIS`) is another plugin's cell**, or a cell of P that does not
  compile into the same output: the cluster's file is read in that cell's numbering. Version 1 can
  start with **interior cells** only, then add exterior clusters that compile whole into one output.
- **ESL-flagged outputs:** new cells are never written to one today. How a light plugin's cell FormID
  appears in these paths is not established.
- Any mesh or previs file named by `XCRI` or `RVIS` that cannot be found, read, or rewritten exactly. The
  plugin then stays out, with the cell named (as today).

## What must be checked first (stage PC1, read only)

These need one sample each, from the load order where the keep-outs occur, before anything is written:

1. **The FormID in the file name: settled.** It is the cell's FormID with load order byte 00. The Previs Check
   builds that path (`meshes\precombined\<plugin>\<00xxxxxx>_<mesh>_OC.nif`), and on a real load order every
   cell that lists meshes had all of them there (P2a, "Precombined meshes").
2. **The root node name.** Does it always hold the same 8 digits as the file name? Does the game read it at
   all? Renaming is safe either way, but a mismatch must not be silently "fixed".
3. **Physics meshes.** One `_Physics.nif`: confirm it holds no FormIDs.
4. **Which keep-outs this frees: built.** Every compile now writes a **Precombined Cell Check** row (Info) per
   new cell that keeps its plugin out. Each row says:
   - whether the cell is interior;
   - whether a CSG or CDX file is installed;
   - how many of the meshes `XCRI` names are installed, and with physics meshes;
   - how many of the meshes read name the cell's FormID in their strings (point 2);
   - whether its previs file is its own and readable.

   It ends with "PC2 could move its files" or the reasons it stays. A summary row counts the cells and the
   plugins that would no longer be kept out by their cells. Nothing changes what compiles.

   A plugin with `- Geometry.csg` or `.cdx` installed is already kept out before compiling, by its filename
   reference, so its cells never reach this check. The check repeats the rule as a guard.

## Stages

| Stage | What | Changes what compiles? |
|---|---|---|
| PC1 | Precombined Cell Check rows (above), built; points 2 and 3 to be read from a real build's rows and one sample | No |
| PC2 | Interior new cells without CSG/CDX: mesh and previs copies, manifest, read-back, Switch back | Yes, for those plugins |
| PC3 | Exterior cells whose whole `RVIS` cluster compiles into one output | Yes |

## In-game test (each stage)

Walk through every cell PC2 frees, on a new game, with the output and without it (Switch back), and
compare:
- no flicker, no missing walls, no white fog;
- the same frame rate in heavy cells;
- placed objects where they were.

The Testing Guide's previs section applies as is.
