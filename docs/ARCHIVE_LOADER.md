# Archive Loader

`MultiplexerArchiveLoader.dll` is an F4SE plugin that gives Fallout 4 its archive list in full. Turn it on
in **Settings → Archives** (*Load compiled plugins' archives with the F4SE archive loader*), or with
`fo4mx --archive-loader`.

## Why

A compiled plugin is disabled, so the game no longer loads its BA2 archives with it. Multiplexer lists
them in `SResourceArchiveList2` (Fallout4Custom.ini) instead. The game reads that line into a 1,024-byte
buffer: past 1,023 characters every archive is silently never loaded. Without the loader, Multiplexer keeps
the list within that length, and plugins whose archives do not fit stay in the load order (*Archive list
full*). With it, they are compiled too.

## What changes when it is on

- **Compile:** no *Archive list full*. The report says how many archives load through the loader.
- **Export:** the package's `Data\F4SE\Plugins` holds `MultiplexerArchiveLoader.dll` and
  `MultiplexerArchives.txt` (the full list, in the game's order). Install the package as usual.
- **Apply to Game:** writes the whole list into `Fallout4Custom.ini` (the game reads the first 1,023
  characters itself, the loader the rest), and waits until this build's loader and list are in the game's
  Data folder.
- **Playing:** always start the game through F4SE (`f4se_loader.exe`, or your mod manager's F4SE entry).
  Without F4SE only the start of the list loads.
- **Diagnostics:** with the Diagnostic Recorder installed, the Archive compatibility section shows the read
  as `CUT, the rest supplied` and every archive verified; if the loader did not supply the list, the finding
  *The archive loader did not supply the archive list* says what to check.

Only on Fallout 4 1.10.163, where it was tested in game; on another version it logs that and changes
nothing, and Multiplexer keeps the list within the limit.

## How it works

1. Reads the list to supply: `Data\F4SE\Plugins\MultiplexerArchives.txt`, or, without that file,
   `SResourceArchiveList2` from `Fallout4Custom.ini` as plain text.
2. Observes the game's INI reads of `sResourceArchiveList2` (a pass-through wrapper of the game's
   `GetPrivateProfileStringA` import), so it knows the exact value the game received.
3. Changes nothing unless that value is the start of the list to supply: a list from another build, or an
   INI changed since, is left alone.
4. When the game opens its first BA2 archive (every INI read is over by then), it looks in the executable's
   own data for a pointer to that exact text with a pointer to the setting's name
   (`sResourceArchiveList2`, any case, with or without `:Archive`) within 24 bytes; only if none is found
   there does it search the rest of the game's memory the same way. At most four such settings are
   changed; anything else and nothing is changed. No game addresses, no Address Library.
5. Points them at the full list. The game's own code then opens and registers every archive, in list
   order, as it does for a short list.
6. At game data ready (every archive is open by then) it puts the original values back.

Everything is written to `Documents\My Games\Fallout4\F4SE\MultiplexerArchiveLoader.log`.

## Checked so far

In the game (1.10.163): with a 25,345-character list the setting was found in the executable's data and
supplied; archives listed past the cut whose plugins were disabled loaded (TrueStormsFO4, SSTFungalForest,
SS2WastelandReconstructionKit); the original value was restored and a save loaded.

In the game (1.10.163, 0.1.0): the setting name was found in the executable but no setting object with
the assumed layout, so nothing was changed (the recorder's report then showed archives past the cut
loading only because their plugins were active again; it now says so instead of crediting the list).


Under Wine, with a host program that keeps its settings like the game, reads the INI with a 1,024-byte
buffer and opens the archives from its setting: without the loader 36 of 81 archives opened, with it 81 of
81, and the original value was restored; a list from another build was left alone.
