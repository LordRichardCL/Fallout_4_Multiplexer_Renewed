# Changelog

Versions are tagged `vMAJOR.MINOR.PATCH-alpha` while the project is in alpha. Each entry says what was **tested** (and how), not only what was added; [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) keeps the current state. Pushing a version tag makes CI publish a GitHub pre-release with the Windows package and this version's section as its notes.

Release policy (alpha):
- A version is tagged only after a build on a real load order has 0 validation errors and its export passes the written-content check.
- Each entry has a **Tested** line: build verified, confirmed in game, or not yet.
- Anything not yet confirmed in game stays off by default (Advanced preset).

## 0.2.0-alpha (23 Nov 2026)

**Tested:** build verified on 22 Nov 2026 (Vortex, Fallout 4 1.10.163, 2,270 plugins: 1,401 compiled into 7 outputs and a patch, 310,224 new FormIDs, 0 validation errors, written plugins verified, Apply to Game switched the load order over). In game: first session (about 9 minutes from a save, normal quit, all 421 archives loaded, no Papyrus failures tied to compiling); a second build with every setting on (1,668 compiled) ran a new game for about 30 minutes without a crash, all 689 archives loaded. A third build the same night (1,668 compiled into 5 outputs and a patch, 791,783 new FormIDs, 0 validation errors, 153 full and 623 light plugin slots) started a new game and played through character creation and Vault 111 into the Commonwealth (about 21 minutes, six saves, normal quit): all 689 archives loaded, no script property pointed at a missing record in the outputs, and the script errors on output records are ones the source mods also have. A baseline comparison without the build is still to follow.

Safety and testing
- Presets from least to most change: **Safest** (the first-run default), **Recommended**, **Advanced**; an alpha notice on the Compilation page.
- **Ready to Test?** before Apply to Game: validation, export, plugin slots, your newest save, F4SE, a crash logger.
- **Switch Back** undoes the last Apply to Game in one click.
- **Check a Save** says whether a save would lose anything with the load order or the next build.
- FormID Stability and Returning Plugins stop a rebuild that would cost saves their records.

Diagnostics
- Crash logs (Buffout 4) and Papyrus logs name the original mod behind compiled records; readings warn when a log is older than the build.
- Support bundle: one zip with the reports, logs, readings and load order, personal details removed.
- Build comparison: what a rebuild changes against the previous build.
- The User Guide explains the Faster Loose Files warning that appears while the Diagnostic Recorder is installed (harmless: the recorder passes every file open on).

Faster
- An app update keeps the build cache's parts whose code did not change (plugin index, references, conflicts, merges, each versioned by the code that produces it), instead of deleting the whole cache; the results database keeps the last build's record. Damaged entries are recomputed; an unreadable cache file is set aside and recreated.
- Compile anyway (FormID changes accepted) keeps the compilation that just stopped instead of compiling again, when nothing else changed since; any other change compiles again.

Scanning
- Scan asks what to read, with the counts: what the game loads (always what the build uses) or also a preview of inactive plugins (would it compile, what would it conflict with). Settings → Scan; `--scan-scope`.
- Inactive plugins are hidden on the Plugins page until *Show inactive plugins* is on.

Conflict Centre
- Accept Automatic for one record or for every waiting one; a Review column and filters for records waiting for you and records with your choices; manual choices confirmed as saved; a roomier list.
- Applying a build to the game is no longer reported as a change that needs a rebuild.

Compiling
- More plugins compile: plugin INIs, unused masters (placeholders), LOD billboards, constants and cross-script calls in scripts, Master Redirection (experimental).
- Precombined Cell Check (read only) for new cells with precombine or previs data.
- About ten times faster analysis on large load orders; readers refuse damaged files instead of crashing.

Fewer files
- Each export writes its reports as three files in `Multiplexer Reports\` instead of eighteen: `Multiplexer Report.html` (every report, with a contents bar), `.txt` (the same as plain text) and `.json` (every row). The Reports page still exports a single report in any format.
- A smaller download: the Windows package leaves out libraries the app never uses (OpenGL and Direct3D shader compilers, unused database drivers) and carries the Visual C++ runtime as three DLLs instead of its 25 MB installer. CI starts the packaged app on its own before publishing, so a missing DLL stops the release.

Interface
- Dashboard steps are the actions; plugin slots; the status bar shows step n of N and the time left; the build status updates while the app is open.
- Diagnostics organised by situation; Validation explained in the User Guide.

## 0.1.0

First public code: scan, analyse, compile and export for Mod Organizer 2, Vortex and manual installs; reports; Diagnostic Recorder; Auto-Merger. **Tested:** builds on real load orders (see docs/RECORD_HANDLING_AUDIT.md); in game: early results only.
