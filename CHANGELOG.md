# Changelog

Versions are tagged `vMAJOR.MINOR.PATCH-alpha` while the project is in alpha. Each entry says what was **tested** (and how), not only what was added; [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) keeps the current state. Pushing a version tag makes CI publish a GitHub pre-release with the Windows package and this version's section as its notes.

Release policy (alpha):
- A version is tagged only after a build on a real load order has 0 validation errors and its export passes the written-content check.
- Each entry has a **Tested** line: build verified, confirmed in game, or not yet.
- Anything not yet confirmed in game stays off by default (Advanced preset).

## 0.2.0-alpha (unreleased)

**Tested:** build verified on 22 Nov 2026 (Vortex, Fallout 4 1.10.163, 2,270 plugins: 1,401 compiled into 7 outputs and a patch, 310,224 new FormIDs, 0 validation errors, written plugins verified, Apply to Game switched the load order over). In game: first session (about 9 minutes from a save, normal quit, all 421 archives loaded, no Papyrus failures tied to compiling); longer play and a baseline comparison to follow.

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

Compiling
- More plugins compile: plugin INIs, unused masters (placeholders), LOD billboards, constants and cross-script calls in scripts, Master Redirection (experimental).
- Precombined Cell Check (read only) for new cells with precombine or previs data.
- About ten times faster analysis on large load orders; readers refuse damaged files instead of crashing.

Interface
- Dashboard steps are the actions; plugin slots; the status bar shows step n of N and the time left; the build status updates while the app is open.
- Diagnostics organised by situation; Validation explained in the User Guide.

## 0.1.0

First public code: scan, analyse, compile and export for Mod Organizer 2, Vortex and manual installs; reports; Diagnostic Recorder; Auto-Merger. **Tested:** builds on real load orders (see docs/RECORD_HANDLING_AUDIT.md); in game: early results only.
