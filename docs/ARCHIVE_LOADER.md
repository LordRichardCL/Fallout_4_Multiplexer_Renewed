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

### Startup trace (1.1.0)

The log also records when things happen, so a loading problem can be placed in the startup sequence:

- every line has the wall-clock time and the seconds since `F4SEPlugin_Load` (`[14:37:30.168 +   0.153s]`);
- `Archive registration starts: the game opens its first archive, …`: the moment the list is supplied;
- one `BA2 #n  <archive>  thread <id>  [phase]` line for each archive the game opens, the first time it
  opens it. Phase `list`: named in `MultiplexerArchives.txt`, opened by the game's own list loading;
  `list, NOT supplied`: named there, but the list was not supplied; `not in the list`: the game's own
  lists, DLC, Creation Club and plugins' archives. `(open failed)` marks an archive the game could not
  open. Further opens of an archive are only counted;
- `First plugin file opened by the game: …`: archive registration is over and plugin data loading starts;
  then a count every 250 plugin files;
- every F4SE message (PostLoad, PostPostLoad, InputLoaded, both GameDataReady messages, GameLoaded, …) with
  the counts at that moment;
- a watchdog thread: when the game opens no file for 20 seconds before its data is ready, it writes the last
  archive opened (number, name, thread), the last file of any kind, and whether any plugin file has been
  opened yet (`still in archive registration` or `reading plugins`); then once a minute while it lasts. A
  very large plugin can take that long to read, so one note is not a hang; notes that keep coming after
  the last archive, with no plugin file opened, are.

Only the game's own file opens are seen (Fallout4.exe's imports), not those of other F4SE plugins. Trace
lines are not flushed one by one (that would slow loading); Windows keeps what was written even if the
game hangs or is ended.

### Startup sequence (traced, 1.10.163)

From the code and the Diagnostic Recorder's sessions of build `161126 223443` (times from one launch):

1. F4SE loads plugin DLLs in file order: Daytripper 4 logs `Installed ExtendedArchiveLimit patch`
   (13:51:25.659), then this loader's `F4SEPlugin_Load` (13:51:26.243): it reads the list file and wraps
   the three imports. It registers no archive itself, here or later.
2. F4SE PostLoad / PostPostLoad (+0.8 s).
3. The game reads its INIs (`sResourceArchiveList2` 14 times, 1,023 characters kept).
4. The game opens its first archive (`Fallout4 - Startup.ba2`, +4.2 s); inside that `CreateFileW`, before
   the real open, the loader points the setting at the full list.
5. The game registers its startup and base archives, then `sResourceArchiveList2` in list order with its
   own code, one after another on one thread (+7.6 to +10.7 s for 699 archives).
6. The game registers DLC, Creation Club and each active plugin's archives (from +10.8 s).
7. Plugin data loading, then GameDataReady; the loader restores the setting.

So the list is registered before every plugin's archives, through the game's INI-list loading, not the
plugin-archive step. Load order of the DLLs does not matter: the loader acts when the game opens its first
archive, long after every DLL has loaded.

### Open issue: hang with a long list (build `161126 223443`)

With 681 archives supplied (699 registered from the list), the game stopped every time at the 1,104th
archive it registered, whichever archive that was, with the compiled plugins on or off. With 341 supplied
(359 from the list) it loaded and registered 2,105 archives in all; earlier builds with up to 422 from the
list loaded. The total number of archives does not explain it. Not established: whether Daytripper 4's
`ExtendedArchiveLimit` covers archives registered from the INI list as it does plugins' archives (its source
is not published), or whether this is the engine's archive-index bug that *BA2 Archive Index Fix* (Nexus
110922, 1.10.163) describes as a loading hang. The trace above is meant to settle it.

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
