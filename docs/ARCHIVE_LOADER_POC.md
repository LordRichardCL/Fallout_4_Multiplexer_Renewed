# Archive Loader (proof of concept)

`MultiplexerArchiveLoader.dll` is an experiment, not part of a normal build or export. It tests whether an
F4SE plugin can give Fallout 4 an archive list longer than the game's INI reading allows.

## The problem it tests

Fallout 4 reads `sResourceArchiveList2` (and every other archive list) with `GetPrivateProfileStringA`
into a 1,024-byte buffer and keeps the first 1,023 characters. The Diagnostic Recorder showed this in game
(`buffer 1024`, `returned 1023`, `truncated`), with every archive in the list before that point loaded
and none after it. The game reads all its INI files (`Fallout4.ini`, `Fallout4Custom.ini`, and
`Data\<plugin>.ini` for active plugins) before it opens its first archive, and F4SE plugins are loaded
before that.

## What it does

1. Reads the list to supply: `Data\F4SE\Plugins\MultiplexerArchives.txt` (one archive per line, `#`
   comments), or, without that file, `SResourceArchiveList2` from `Fallout4Custom.ini` as plain text.
2. Observes the game's INI reads of `sResourceArchiveList2` (a pass-through wrapper of the game's
   `GetPrivateProfileStringA` import), so it knows the exact value the game received.
3. When the game opens its first BA2 archive (every INI read is over by then), it finds the game's setting
   from what is certain rather than from an assumed memory layout: the copies of the received text in
   memory, the places that point at a copy, and, of those, the ones with a pointer to the setting's name
   (`sResourceArchiveList2`, any case, with or without `:Archive`) within 24 bytes. Only those are changed
   (at most four); anything else and nothing is changed. No game addresses, no Address Library. (0.1.0
   assumed CommonLibF4's `Setting` layout and found nothing in Fallout 4 1.10.163; 0.2.0 logs every
   pointer it finds and where the name was beside it.)
4. Points them at the full list. The game's own code then opens and registers every archive, in list
   order, as it does for a short list.
5. At game data ready (every archive is open by then) it puts the original values back.

Everything is written to `Documents\My Games\Fallout4\F4SE\MultiplexerArchiveLoader.log`.

## Testing it

1. Use a test profile. Keep the Diagnostic Recorder (1.2.0 or later) installed.
2. Put the long archive list in `Fallout4Custom.ini` (`[Archive]` `SResourceArchiveList2=...`), or list the
   archives in `MultiplexerArchives.txt`.
3. Copy `ArchiveLoaderPoC\MultiplexerArchiveLoader.dll` (from the Windows build) into
   `<game>\Data\F4SE\Plugins\`.
4. Start the game through F4SE to the main menu.
5. Read `MultiplexerArchiveLoader.log`, and open the session in Multiplexer's Diagnostics page. With the
   list supplied, the report shows the read as `CUT, the rest supplied`, every archive in `Data` verified,
   and the finding *The archive list was supplied in full*.

If the log says the setting could not be identified with certainty, nothing was changed. If the setting
was supplied but the report still shows archives past the cut not loaded, the game had already split the
list before its first archive opened: the next experiment then supplies the value at the last INI read.

To remove it, delete the DLL.

## Checked so far

In the game (1.10.163, 0.1.0): the setting name was found in the executable but no setting object with
the assumed layout, so nothing was changed (the recorder's report then showed archives past the cut
loading only because their plugins were active again; it now says so instead of crediting the list).


Under Wine, with a host program that keeps its settings like the game, reads the INI with a 1,024-byte
buffer and opens the archives from its setting: without the loader 36 of 81 archives opened, with it 81 of
81, and the original value was restored. Not yet run in the game itself.
