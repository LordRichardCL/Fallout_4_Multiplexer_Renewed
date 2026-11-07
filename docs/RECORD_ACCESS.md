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
| Values by name | named, typed members (integers, floats, flags, FormIDs, bytes) where defined: OBND, WEAP DNAM/FNAM/CRDT, ARMO DATA/FNAM, AMMO DATA/DNAM, MISC/BOOK/KEYM DATA, ALCH ENIT, GLOB, leveled lists, NPC_ ACBS, TREE, DUAL, ASTP, EYES; every other field by kind (text, FormIDs, string ids) or as bytes | `decodeValues` (`esp/FieldValues.hpp`), `fo4mx dump --values` |
| References | any record version on demand; "Referenced By" over every version of every record in the load order | `refs::referencesOf`, `refs::referencedBy`, `fo4mx references` |
| ESL | light detection, FE xxx numbering, slot limits, object id range | `PluginScanner`, validation |
| Unknown data | kept byte for byte, written back unchanged | `Record`, `PluginWriter` |

Not described: NOCM (Navmesh Obstacle Manager), whose data xEdit also shows only as unknown bytes. Its
records are preserved; a plugin with one stays in the load order.

The value layer is descriptive: names and types come from xEdit, are added type by type in
`buildValues` (`esp/RecordDefinitions.cpp`), and nothing in compiling depends on them (they are not part
of the definitions fingerprint, so adding them never invalidates cached work).
