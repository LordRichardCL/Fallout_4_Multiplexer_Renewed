# Compatibility

What has been confirmed, and how. **Fallout 4 Multiplexer Renewed is alpha software**: start with the **Safest** preset on a **new game**, and keep your saves backed up. This page is updated after every round of testing; [CHANGELOG.md](../CHANGELOG.md) says what changed in each version.

## Levels of evidence

| Level | Means |
|---|---|
| **Confirmed in game** | Played on compiled output, with the result checked in game |
| **Build verified** | Compiled, exported and switched over on a real load order with 0 validation errors; every written plugin read back and compared with the plan |
| **Tested** | Covered by the automated tests (synthetic installs), not yet on a real load order |
| **Experimental** | Works in tests; off by default (Advanced preset) until confirmed in game |
| **Not supported** | Detected and left in the load order (never compiled) |

## Features

| Feature | Level | Evidence |
|---|---|---|
| Scan, Analyse, Compile, Export | Build verified | 22 Nov 2026: Vortex, Fallout 4 1.10.163, 2,270 plugins (869 active after switching over); 1,401 compiled into 7 outputs and a patch, 310,224 new FormIDs, 0 errors |
| Apply to Game (Vortex) | Build verified | Same build: compiled plugins disabled, outputs enabled last, archive list written to Fallout4Custom.ini, every file backed up |
| Apply to Game (Mod Organizer 2) | Tested | Earlier real load orders (builds 63 to 83, see RECORD_HANDLING_AUDIT.md) |
| Switch Back | Tested | Automated tests; not yet used on a real install |
| Playing on compiled output | **First session** | 22 Nov 2026, same build: the game loaded the 869-plugin load order, reached the main menu, loaded a save and ran about 9 minutes, then quit normally (exit save, no crash). All 421 archives loaded (the F4SE archive loader supplied what the game's 1,023-character INI read cut off). Papyrus: scripts ran on 3,454 occasions on records in the outputs; no script failed to find a plugin by name; the 19 errors on output records (in 1,474 error lines) are script bindings the source mods also get wrong. A baseline log without the build is still to compare. Second session, same day, with every setting on (Vortex): 1,668 plugins compiled into 5 outputs and a patch, 0 validation errors, 153 full plugin slots; a **new game** played for about 30 minutes including workbenches, no crash; all 689 archives loaded; no script failed to find a plugin by name; the quest scripts that failed to bind are neither rewritten by the Auto-Merger nor moved by archive priority (still to compare with a baseline). |
| Safest preset | Build verified | |
| Recommended preset (patch choices, cell settings, previs translation, patch plugin) | First session | 22 Nov 2026 with every setting on: new game, about 30 minutes, no crash | Previs numbering confirmed in game on one cell (USAFSatellite01, see RECORD_HANDLING_AUDIT.md) |
| Auto-Merger (scripts and configs relinked) | Build verified | 22 Nov 2026: 393 files relinked, 0 errors |
| Light outputs | Experimental | |
| Placeholders for unused masters | Experimental | |
| Master Redirection | Experimental | Mod Organizer 2 only |
| F4SE archive loader | Build verified | 22 Nov 2026: 421 of 421 archives loaded in game (Diagnostic Recorder) |
| LOD billboards moved with their records | Experimental | |
| New cells with precombine or previs data | Not supported | Kept in the load order; Precombined Cell Check (read only) reports what a later version could move |
| "Clean" precombines (`- Geometry.csg`, `.cdx`) | Not supported | Kept in the load order |
| Plugins named by F4SE DLLs, Transfer Settlements blueprints, LooksMenu overlays | Not supported | Kept in the load order |

## Record types

Generated from `fo4mx record-types`.

**Compiled, described field by field** (compatible changes from several plugins are merged):
`ACTI` Activator, `ALCH` Ingestible, `AMDL` Aim Model, `AMMO` Ammunition, `AORU` Attraction Rule, `ARMA` Armor Addon, `ARMO` Armor, `ARTO` Art Object, `AVIF` Actor Value Information, `BOOK` Book, `CLFM` Color, `CMPO` Component, `COBJ` Constructible Object, `CONT` Container, `DFOB` Default Object, `DMGT` Damage Type, `DOOR` Door, `EFSH` Effect Shader, `ENCH` Enchantment, `EQUP` Equip Type, `EXPL` Explosion, `FACT` Faction, `FLOR` Flora, `FLST` FormID List, `FURN` Furniture, `GLOB` Global, `HAZD` Hazard, `INGR` Ingredient, `INNR` Instance Naming Rules, `IPCT` Impact, `IPDS` Impact Data Set, `KEYM` Key, `KSSM` Sound Keyword Mapping, `KYWD` Keyword, `LCRT` Location Reference Type, `LIGH` Light, `LSCR` Load Screen, `LVLI` Leveled Item, `LVLN` Leveled NPC, `MATO` Material Object, `MESG` Message, `MGEF` Magic Effect, `MISC` Misc. Item, `MSTT` Moveable Static, `MSWP` Material Swap, `NOTE` Note, `OMOD` Object Modification, `OTFT` Outfit, `PERK` Perk, `PROJ` Projectile, `RFCT` Visual Effect, `SCOL` Static Collection, `SNDR` Sound Descriptor, `SOUN` Sound Marker, `SPEL` Spell, `STAT` Static, `TERM` Terminal, `TRNS` Transform, `TXST` Texture Set, `WEAP` Weapon, `ZOOM` Zoom

**Compiled, winning version taken whole** (every FormID located and rewritten; fields not merged):
`AACT` Action, `ACHR` Placed NPC, `ADDN` Addon Node, `AECH` Audio Effect Chain, `ANIO` Animated Object, `ASPC` Acoustic Space, `ASTP` Association Type, `BNDS` Bendable Spline, `BPTD` Body Part Data, `CAMS` Camera Shot, `CELL` Cell, `CLAS` Class, `CLMT` Climate, `COLL` Collision Layer, `CPTH` Camera Path, `CSTY` Combat Style, `DEBR` Debris, `DIAL` Dialog Topic, `DLBR` Dialog Branch, `DLVW` Dialog View, `DOBJ` Default Object Manager, `DUAL` Dual Cast Data, `ECZN` Encounter Zone, `EYES` Eyes, `FSTP` Footstep, `FSTS` Footstep Set, `GDRY` God Rays, `GMST` Game Setting, `GRAS` Grass, `HDPT` Head Part, `IDLE` Idle Animation, `IDLM` Idle Marker, `IMAD` Image Space Adapter, `IMGS` Image Space, `INFO` Dialog response, `LAND` Landscape, `LAYR` Layer, `LCTN` Location, `LENS` Lens Flare, `LGTM` Lighting Template, `LTEX` Landscape Texture, `LVSP` Leveled Spell, `MATT` Material Type, `MOVT` Movement Type, `MUSC` Music Type, `MUST` Music Track, `NAVI` Navmesh Info Map, `NAVM` Navmesh, `NPC_` Non-Player Character, `OVIS` Object Visibility Manager, `PACK` Package, `PARW` Placed Arrow, `PGRE` Placed Projectile, `PHZD` Placed Hazard, `PKIN` Pack-In, `PMIS` Placed Missile, `QUST` Quest, `RACE` Race, `REFR` Placed Object, `REGN` Region, `RELA` Relationship, `REVB` Reverb Parameters, `RFGP` Reference Group, `SCCO` Scene Collection, `SCEN` Scene, `SCSN` Audio Category Snapshot, `SMBN` Story Manager Branch Node, `SMEN` Story Manager Event Node, `SMQN` Story Manager Quest Node, `SNCT` Sound Category, `SOPM` Sound Output Model, `SPGD` Shader Particle Geometry, `STAG` Animation Sound Tag Set, `TACT` Talking Activator, `TREE` Tree, `VTYP` Voice Type, `WATR` Water, `WRLD` Worldspace, `WTHR` Weather

**Candidates** (described, waiting for a real load order that contains them; a plugin with one stays):
`PBAR` Placed Barrier, `PBEA` Placed Beam, `PCON` Placed Cone/Voice, `PFLA` Placed Flame

**Not described yet** (stays in its plugin; a plugin with one is not compiled):
`NOCM` Navmesh Obstacle Manager, `PLYR` Player Reference
