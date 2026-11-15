# PBAR, PBEA, PCON and PFLA: research and merge contracts

This answers the owner's working model (`docs/specs/PLACED_RECORDS_PBAR_PBEA_PCON_PFLA.txt`). Sources:
- xEdit's Fallout 4 definitions (`Core/wbDefinitionsFO4.pas`, branch dev-4.1.6);
- the owner's own load order: two Definition Checks, about 1,366 plugins including `Fallout4.esm` and every DLC;
- this app's record layouts, which the checks compare with real data.

Nothing below comes from guessing what the signatures stand for.

## Conclusion

**All four stay Candidates: not compiled, and no automatic merging.** A plugin that contains one stays in the load order.

- **The layout is a guess, even in xEdit.** xEdit defines all eight placed projectile types with one procedure, `ReferenceRecord` (PARW, PBAR, PBEA, PCON, PFLA, PGRE, PHZD, PMIS). Its source comment says: *"Skrim has its own ref record for every projectile type … I guess all of them have the same structure"*.
- **Four of the eight are confirmed by real data.** PGRE, PHZD, PMIS and PARW occur in the owner's load order; for example 4,553 PHZD and 1,095 PGRE records, all with 100% of their FormIDs resolving. They passed the group layout check and are compiled.
- **These four have no real data at all.** There are no PBAR, PBEA, PCON or PFLA records in any plugin of that load order, the base game and DLC included. So there is nothing to check the layout, the group placement or the reference rewriting against.
- **Promoting them now would rely on xEdit's guess.** That goes against the project rule: promote only what real data confirms, and fail closed otherwise.

What the app does with them now:
- They have a layout, the same as the confirmed placed types. It is used for conflict analysis and the Definition Check.
- A plugin with one of these records is not compiled ("Contains record types not yet compiled").
- The Definition Check reports each type it finds (decoded, FormIDs resolving or not). For a type in no plugin at all it now says so explicitly ("candidate not present in your load order"), so the absence of a line can no longer be mistaken for a check that was not run.
- When a load order that contains them is checked, the same gates as for PGRE apply: at least 99% of FormIDs resolve, no undescribed FormID fields, and an identical rebuild by the group layout check. A type that passes is then switched to Winner in `resources/record_catalogue.csv`, exactly as PGRE, PHZD, PMIS and PARW were.

## Answers to the working model's questions

1. **xEdit definition:** `ReferenceRecord(PBAR, 'Placed Barrier')`, `(PBEA, 'Placed Beam')`, `(PCON, 'Placed Cone/Voice')` and `(PFLA, 'Placed Flame')`. These are the only names xEdit gives; the app uses them and nothing more.

2. **Fields:**
   - record flags: Turn Off Fire 0x80, Persistent 0x400, Initially Disabled 0x800, Reflected By Auto Water 0x10000000, Don't Havok Settle 0x20000000, No Respawn 0x40000000;
   - EDID; VMAD (scripts); NAME, the projectile (PROJ or HAZD); XEZN, the encounter zone;
   - XHTW head-tracking weight; XFVC favour cost;
   - repeated XPWR (reflected/refracted by: reference, type); repeated XLKR (linked reference: keyword, reference);
   - XAPD plus repeated XAPR (activate parents: reference, delay);
   - XASP acoustic parent; XATP activation point; XAMC ammo count; XLKT linked ref transient;
   - XLYR layer; XMSP material swap; XRFG reference group; XCVR water current zone;
   - XESP enable parent; XOWN/XRNK ownership; XRGD/XRGB ragdoll; XHLT health %;
   - XEMI emittance; XMBR multibound reference; XIS2 ignored by sandbox;
   - XLRT location ref types (array); XLRL location reference;
   - XSCL scale; XLOD; DATA position and rotation (required); MNAM comments.

3. **Shared with REFR:** every one of them. REFR has the same fields plus its own (doors, map markers, lights, patrols and more).

4. **Unique to these types:** none. Only NAME differs: here it names a PROJ or HAZD, in REFR any base object.

5. **FormID fields:**
   - NAME, XEZN, XASP, XLYR, XMSP, XRFG, XEMI, XMBR, XLRL;
   - XPWR (at 0), XLKR (at 0 and 4), XAPR (at 0), XESP (at 0), XOWN (at 0);
   - XLRT (each entry); VMAD (script properties).

   All of them are in the app's placed-reference layout, which the confirmed types use.

6. **Optional:** everything except DATA (`SetRequired`).

7. **Repeated:** XPWR, XLKR, XAPR (each `wbRArrayS`, a sorted array) and the entries of XLRT.

8. **What kind of collections:**
   - XPWR, XLKR and XAPR are sorted arrays keyed on their first FormID (`wbStructSK([0])`): keyed, order not significant.
   - XLRT is a plain array (`wbArray`): order unknown.

9. **Placement:** DATA (position and rotation), the cell group the record sits in, XSCL, XLOD.

10. **Persistence and group:** the Persistent flag (0x400) decides whether the record goes in the cell's persistent or temporary child group. Initially Disabled (0x800) and the enable parent (XESP) decide whether it exists in the world at start.

11. **Independent fields (in principle):** XHTW, XFVC, XAMC, XHLT, XSCL, MNAM, XEZN, XLYR, XMSP, XEMI, XLRL.

12. **Taken whole:**
    - DATA (position and rotation together);
    - XESP (reference and flags);
    - XOWN with XRNK;
    - XRGD/XRGB;
    - XAPD with its XAPR list;
    - VMAD.

13. **Never merged automatically:** NAME (what is placed), DATA, the Persistent flag, the parent cell, VMAD, and every field whose meaning is not confirmed by data. That is all of them for these four types.

14. **Overrides:** xEdit, like this app, matches records by their resolved FormID (the master that defines them, plus the object ID), not by the raw number. The parent cell is context, not identity.

15. **Display:** under the cell's child groups (persistent and temporary), mixed with REFR and ACHR. The app reads them with that placement (`RecordPlacement::CellChild`) and would write them there.

16–18. **Examples in official plugins:** none. Both of the owner's Definition Checks, which cover `Fallout4.esm` and every DLC, found no PBAR, PBEA, PCON or PFLA records. So no real example can be shown; the test suite builds a synthetic PBAR to check the fail-closed path.

19. **What they are:** placed references of projectiles. NAME accepts PROJ and HAZD, and xEdit's comment ties them to Skyrim's per-projectile reference types (beam, flame, cone/voice, barrier). Nothing more is supported by a source.

20. **Uncertain:**
    - whether the Creation Kit can author these at all, or whether the game only creates them while playing (in saves, not plugins);
    - whether their layout really matches PGRE's;
    - how the game reacts to one written into a plugin.

## Merge contract (all four, the same)

| Part | Contract |
| --- | --- |
| Identity | Resolved FormID (defining master and object ID), its own override chain; never part of the cell record |
| Parent | The cell group it is in: context only. A move to another cell, or a change of the Persistent flag, is structural |
| Independent | none yet (see 11 for the candidates once confirmed) |
| Selectable, not merged | XEZN, XLYR, XMSP, XEMI, XLRL (FormIDs: target must exist in the output's masters) |
| Atomic | DATA; XESP; XOWN+XRNK; XRGD/XRGB; XAPD+XAPR; VMAD |
| Keyed collections | XPWR, XLKR, XAPR (keyed on their first FormID) |
| Ordered or unknown | XLRT |
| Structural | NAME; DATA; record flags (Persistent, Initially Disabled); the parent cell |
| FormID dependencies | as listed in 5 |
| Unknown | the whole layout, until a real record confirms it |
| Automatic compatibility | **Disabled** |

## How they fit the compiler once confirmed

The four would follow PGRE exactly:
- winner-only overrides and new records, written in their cell's persistent or temporary group;
- every FormID rewritten through the layout;
- the cell copied when the cell's own winner stays in the load order.

Field-level carrying forward (the compatibility patch analysis) is a separate, later pipeline. It uses the contract above, and only for fields a real load order has confirmed. Placed records and cell configuration stay separate conflict domains, as the working model asks.
