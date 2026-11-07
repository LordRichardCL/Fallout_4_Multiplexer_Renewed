# Record access compared with xEdit

What the backend can see in a Fallout 4 plugin, against xEdit's core (`Core/wbImplementation.pas`,
`wbInterface.pas`, `wbDefinitionsFO4.pas`).

| Capability | Multiplexer | Where |
|---|---|---|
| Plugin enumeration | every plugin in Data or MO2's virtual Data, inactive and ghosted included | `PluginScanner::discoverPlugins` |
| GRUP traversal, nested groups | every group with type and label; each record knows its world, cell, topic, quest, block and sub-block | `PluginFile::forEachGroup`, `forEachRecord`, `RecordLocation` |
| The whole tree | TES4 header, then each group with its children, as xEdit's tree view | `PluginFile::tree()`, `fo4mx dump --tree` |
| TES4 header | version, record count, next object id, author, description, masters; ONAM read (`overriddenForms`), INTV/INCC kept | `PluginHeader` |
| Records, signatures, FormIDs | every record of every scanned plugin, FormID resolved to (defining plugin, object id) | `Session::records`, `Session::resolve` |
| Override chains | every version of a record, in load order | `Session::groups`, database `OverrideChains` |
| Subrecords | parsed, with XXXX for large ones | `Record::parseSubrecords` |
| Compressed records | zlib, both ways | `Record::decompress` |
| Where FormIDs are | every FormID slot, string, list and count in 134 record types plus placed references; context-dependent fields from xEdit | `RecordDefinitions`, `FieldDecoder` |
| Values by name | named, typed members (integers, floats, flags with their names, enums, FormIDs, fixed byte runs, nested structs, fixed arrays) for 453 fields in 115 record types: generated from xEdit by `tools/gen_value_defs.py` into `esp/ValueDefinitions.inc`, plus hand-written ones (OBND, WEAP, ARMO, AMMO, leveled lists, NPC_ ACBS, ...); every other field by kind (text, FormIDs, string ids) or as bytes, and bytes a layout does not name as `(rest)` | `decodeValues` (`esp/FieldValues.hpp`), `fo4mx dump --values` |
| References | any record version on demand; "Referenced By" over every version of every record in the load order | `refs::referencesOf`, `refs::referencedBy`, `fo4mx references` |
| ESL | light detection, FE xxx numbering, slot limits, object id range | `PluginScanner`, validation |
| Unknown data | kept byte for byte, written back unchanged | `Record`, `PluginWriter` |

Not described: NOCM (Navmesh Obstacle Manager), whose data xEdit also shows only as unknown bytes. Its
records are preserved; a plugin with one stays in the load order.

How the generated layouts are made safe: the generator emits only fixed layouts and stops a struct at the
first member whose size or position is not fixed (strings, variable arrays, unions, version- or
game-dependent members), so the bytes after it show as `(rest)`; a subrecord xEdit describes two ways
within one record is left out. The program applies a generated layout only on fields it reads as numbers,
structs or arrays, never over a hand-written one, and only when its FormID positions are exactly this
app's own (one is refused today: CELL XCRI, a counted array handled specially). **Check Definitions**
reports, under **Value Layout** (Info), every field whose size in the load order differs from a complete
named layout, so the layer is checked against real data.

Regenerate after updating xEdit:

```
python3 -I tools/gen_value_defs.py <xEdit>/Core/wbDefinitionsFO4.pas src/core/esp/ValueDefinitions.inc
```

The value layer is descriptive: names and types come from xEdit, are added type by type in
`buildValues` (`esp/RecordDefinitions.cpp`), and nothing in compiling depends on them (they are not part
of the definitions fingerprint, so adding them never invalidates cached work).
