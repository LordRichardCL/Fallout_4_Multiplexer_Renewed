# Master Redirection: design for review

*Status: proposal, 20 Nov 2026. Nothing here is implemented. It needs the owner's sign-off on the decisions in the last section before any code is written.*

## The problem it solves

A plugin that stays in the load order keeps every master it lists there too. A framework (A) that is perfectly compilable stays out because one quest mod or patch (B) that cannot be compiled lists it as a master. The Plugins page's **Holding Back** view shows how many plugins each such B keeps out. In large load orders this is the most common reason frameworks, and everything that depends on them, stay in the load order.

Placeholders for unused masters (already built) solve the case where B lists A but uses none of A's records. Master Redirection solves the case where B **does** use A's records.

## The idea

Compile A as usual. Write a copy of B, called B′, into the output mod, with:
- the same file name as B, so everything that finds B by name (its archives, its scripts' `GetFormFromFile(..., "B.esp")`, MCM configs, FaceGen and voice folders) still finds it;
- A removed from its master list and replaced by the output plugins that now hold A's records;
- every FormID in B's records that names one of A's records translated to that record's new output and FormID, using the same Form Map as every compiled record;
- B's own records unchanged: same object IDs. Saves that know B's records keep them; only references to A's records change, exactly as they do for every compiled plugin.

The original B.esp is never changed. In Mod Organizer 2 the output mod overrides it; switching the output mod off restores it.

## Which plugins can be redirected (all must hold)

1. **B decodes completely.** Every record of every type is described field by field: no undescribed bytes, no heuristic spans, no record types the definitions do not know, no decode warnings. Any FormID in data the app cannot place would be left pointing at A, so this is the same proof the compiler already requires before moving a record.
2. **A compiles.** A is eligible on its own merits, and nothing else keeps it in the load order.
3. **Nothing finds A by name through B.** B's scripts and configs naming A's file are A's own filename references, already classified. A stays out unless they are Safe or rewritten by the Auto-Merger.
4. **B's precombine and previs data do not name A's references.** Precombined meshes and previs files store reference FormIDs. Version 1 keeps B out when they name any of A's references, rather than translating its geometry files.
5. **The new master list fits.** B′'s masters are B's own masters minus A, plus the outputs it needs. That must stay within 254 entries, and within the light-plugin rules if B is ESL-flagged.
6. **The output mod can override B.** This holds for Mod Organizer 2 with the output folder inside the mods folder, the same condition as archive loaders and placeholders. Other installs are covered by decision 2 below.

## The load-order problem (and the rule that solves it)

B′ lists the outputs as masters, so it must load **after** them. The outputs load after every plugin that stays. B therefore moves from its own place to the very end of the load order. Two things change if nothing is done:

- **B would win records it used to lose.** For a record B overrides that a later plugin also overrides (call it C, compiled or staying), the game showed C's version before. With B′ loaded last, it would show B's.
- **Records B lost to compiled plugins.** The compiler assumes the outputs load last, so they hold the final merged state. B′ after them would override that merged state with B's own version.

**Rule.** B′ never holds B's raw version of a record that another plugin later than B also changes. For each such record, B′ holds exactly the record the game shows today: the compiler's merged winner for that record, with B's changes in their usual place in the merge. Only records no later plugin touches keep B's own version, translated. B′ then reproduces today's game state as a last-loading plugin. The same written-content check that verifies the outputs checks B′ record by record against that expected state.

Consequence: B′ may need, as masters, plugins whose records the merged winners reference. That is counted in condition 5.

## What is written, and where

- `B.esp` (B′) in the output mod's folder, next to the outputs, and loaded after them. Apply to Game keeps B enabled and moves it after the outputs in `plugins.txt`. The Form Map and `Multiplexer_Redirected.tsv` list every translated FormID: record, field, old value, new value.
- A is compiled and switched off as usual.
- B's archives keep loading through B′'s name, since archives load by plugin name. Nothing about them changes.

## Checks (fail closed)

1. **Decoding:** B decodes completely (condition 1), or B is not redirected.
2. **Plan check before writing:** each record of B′ equals B's record with only the planned FormIDs changed, or equals today's merged winner (the rule above). Any other difference stops B's redirection and keeps A in the load order.
3. **Read-back after export:** B′ is read back, every FormID resolves, and the written-content check compares it with the plan, as for the outputs.
4. **Previous builds:** B′ is regenerated on every build from the B installed then. If B is updated, B′ follows; if B can no longer be redirected, A returns to the load order. Pin-back protection asks first.

## What the user sees

- **Plugins page:** B shows "Redirected: its masters point at the outputs (A.esp is compiled)". A shows "Compiled; redirected in B.esp".
- **Holding Back:** a new column, "Would compile with redirection", counting what each plugin holds back that redirection would free.
- **A switch:** "Redirect masters of plugins that stay" (advanced, off by default), plus a per-plugin opt-out on the Plugins page.
- **Reports:** a Dependency Report section listing each redirection with the masters replaced and the number of FormIDs translated. The Validation Report has an Info row per redirected plugin.
- **Undo:** switch the output mod off and A back on (Apply's backups and `Multiplexer_Instructions.txt` cover it). B's original file was never changed.

## Tests (planned)

- B uses one of A's keywords: B′ references the keyword's new FormID, and B's own records keep their FormIDs.
- B overrides a record that a later plugin C also overrides: B′ holds today's winner, not B's version.
- B has an undescribed subrecord: not redirected, and A stays.
- B's previs names one of A's references: not redirected.
- The master list would exceed 254: not redirected.
- A non-MO2 install (until decision 2 is settled): not redirected, and the reason says so.
- Export, read back and written-content check pass. Switching the output mod off leaves the original B intact.

## Decisions for the owner

1. **Opt-in.** Proposed: a global advanced switch, off by default, plus a per-plugin opt-out. Alternative: opt-in per plugin only.
2. **Vortex and manual installs.** In Vortex, B′ must win its file conflict with B. That is possible with a conflict rule, but the app cannot set rules itself. Proposed: Mod Organizer 2 only in version 1. Vortex would follow with instructions and a check, as for the archive winner copies.
3. **Records later plugins also change.** Proposed as above: B′ holds today's merged winner. The alternative, which is to leave B out whenever any such record exists, is simpler but would exclude most patches, the main target.
4. **Precombines and previs in B.** Proposed: keep B out in version 1 when they name A's references. Translating its previs files, as is already done for compiled plugins, could follow.
5. **Scope of the first version.** Proposed: one level only. A is compiled, B is redirected, and no plugin is both redirected and a master that needs redirecting in turn.
