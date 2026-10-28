#pragma once

namespace fo4mx::db {

inline constexpr int kSchemaVersion = 1;

// Core tables follow the specification (Plugins, Records, Assets, Conflicts,
// Dependencies, Masters, OverrideChains, FormMappings, GeneratedRecords,
// ValidationResults, CompilationGroups) plus detail tables the UI needs.
inline constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS Meta (
    key TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE IF NOT EXISTS Plugins (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    load_order INTEGER,
    load_order_hex TEXT,
    active INTEGER NOT NULL,
    base_game INTEGER NOT NULL,
    creation_club INTEGER NOT NULL,
    source_mod TEXT,
    path TEXT,
    file_size INTEGER,
    header_version REAL,
    author TEXT,
    description TEXT,
    flags INTEGER,
    localized INTEGER,
    record_count INTEGER,
    new_records INTEGER,
    override_count INTEGER,
    injected_count INTEGER,
    itm_count INTEGER,
    deleted_count INTEGER,
    conflict_count INTEGER,
    hard_conflict_count INTEGER,
    master_count INTEGER,
    status TEXT,
    eligibility TEXT,
    eligibility_reason TEXT,
    archives TEXT,
    record_types TEXT,
    support TEXT
);
CREATE INDEX IF NOT EXISTS idx_plugins_name ON Plugins(name);

CREATE TABLE IF NOT EXISTS Masters (
    plugin_id INTEGER NOT NULL,
    position INTEGER NOT NULL,
    master_name TEXT NOT NULL,
    master_plugin_id INTEGER,
    missing INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_masters_plugin ON Masters(plugin_id);
CREATE INDEX IF NOT EXISTS idx_masters_master ON Masters(master_plugin_id);

CREATE TABLE IF NOT EXISTS Records (
    id INTEGER PRIMARY KEY,
    plugin_id INTEGER NOT NULL,
    signature TEXT NOT NULL,
    origin_plugin_id INTEGER,
    object_id INTEGER NOT NULL,
    form_id TEXT NOT NULL,
    raw_form_id INTEGER NOT NULL,
    editor_id TEXT,
    flags INTEGER,
    data_size INTEGER,
    file_offset INTEGER,
    kind TEXT,
    is_winner INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_records_key ON Records(origin_plugin_id, object_id);
CREATE INDEX IF NOT EXISTS idx_records_plugin ON Records(plugin_id);
CREATE INDEX IF NOT EXISTS idx_records_edid ON Records(editor_id);

CREATE TABLE IF NOT EXISTS OverrideChains (
    origin_plugin_id INTEGER NOT NULL,
    object_id INTEGER NOT NULL,
    position INTEGER NOT NULL,
    plugin_id INTEGER NOT NULL,
    record_id INTEGER
);
CREATE INDEX IF NOT EXISTS idx_chains_key ON OverrideChains(origin_plugin_id, object_id);

CREATE TABLE IF NOT EXISTS Conflicts (
    id INTEGER PRIMARY KEY,
    origin_plugin_id INTEGER NOT NULL,
    object_id INTEGER NOT NULL,
    form_id TEXT NOT NULL,
    signature TEXT NOT NULL,
    editor_id TEXT,
    conflict_type INTEGER NOT NULL,
    type_name TEXT NOT NULL,
    severity TEXT NOT NULL,
    winner_plugin_id INTEGER,
    chain_length INTEGER,
    chain TEXT,
    winner_loses_changes INTEGER,
    summary TEXT,
    preserved INTEGER
);
CREATE INDEX IF NOT EXISTS idx_conflicts_type ON Conflicts(conflict_type);
CREATE INDEX IF NOT EXISTS idx_conflicts_key ON Conflicts(origin_plugin_id, object_id);

CREATE TABLE IF NOT EXISTS EligibilityBlockers (
    signature TEXT NOT NULL,
    name TEXT,
    plugins INTEGER,
    records INTEGER,
    sole_blocker INTEGER,
    unlocks INTEGER,
    category TEXT,
    support TEXT
);

CREATE TABLE IF NOT EXISTS ConflictFields (
    conflict_id INTEGER NOT NULL,
    field_index INTEGER NOT NULL,
    field TEXT NOT NULL,
    hard INTEGER NOT NULL,
    position INTEGER NOT NULL,
    plugin_id INTEGER NOT NULL,
    value TEXT,
    status TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_conflict_fields ON ConflictFields(conflict_id);

CREATE TABLE IF NOT EXISTS ConflictPlugins (
    conflict_id INTEGER NOT NULL,
    plugin_id INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_conflict_plugins ON ConflictPlugins(plugin_id);

CREATE TABLE IF NOT EXISTS Archives (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    mod_name TEXT,
    owner_plugin_id INTEGER,
    load_sequence INTEGER,
    type TEXT,
    version INTEGER,
    file_count INTEGER,
    path TEXT
);

CREATE TABLE IF NOT EXISTS Assets (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    asset_type TEXT NOT NULL,
    source_count INTEGER NOT NULL,
    winner_mod TEXT,
    winner_kind TEXT,
    winner_archive TEXT,
    winner_size INTEGER,
    winner_physical TEXT,
    reason TEXT
);
CREATE INDEX IF NOT EXISTS idx_assets_path ON Assets(path);
CREATE INDEX IF NOT EXISTS idx_assets_type ON Assets(asset_type, source_count);

CREATE TABLE IF NOT EXISTS AssetSources (
    asset_id INTEGER NOT NULL,
    position INTEGER NOT NULL,
    mod_name TEXT,
    kind TEXT NOT NULL,
    archive TEXT,
    physical TEXT,
    size INTEGER,
    priority INTEGER,
    is_winner INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_asset_sources ON AssetSources(asset_id);

CREATE TABLE IF NOT EXISTS NameReferences (
    plugin_id INTEGER NOT NULL,
    asset_path TEXT NOT NULL,
    source TEXT
);

CREATE TABLE IF NOT EXISTS Dependencies (
    from_origin INTEGER NOT NULL,
    from_object INTEGER NOT NULL,
    to_origin INTEGER NOT NULL,
    to_object INTEGER NOT NULL,
    via TEXT
);
CREATE INDEX IF NOT EXISTS idx_dep_from ON Dependencies(from_origin, from_object);
CREATE INDEX IF NOT EXISTS idx_dep_to ON Dependencies(to_origin, to_object);

CREATE TABLE IF NOT EXISTS Forms (
    origin_plugin_id INTEGER NOT NULL,
    object_id INTEGER NOT NULL,
    form_id TEXT NOT NULL,
    signature TEXT,
    editor_id TEXT,
    winner_plugin_id INTEGER,
    PRIMARY KEY (origin_plugin_id, object_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_forms_edid ON Forms(editor_id);

CREATE TABLE IF NOT EXISTS DependencyIssues (
    severity TEXT NOT NULL,
    category TEXT NOT NULL,
    plugin_id INTEGER,
    form_id TEXT,
    message TEXT,
    cause TEXT
);

CREATE TABLE IF NOT EXISTS CompilationGroups (
    output_index INTEGER PRIMARY KEY,
    output_name TEXT NOT NULL,
    record_count INTEGER,
    new_records INTEGER,
    overrides INTEGER,
    size_bytes INTEGER,
    light INTEGER,
    strategy TEXT,
    masters TEXT
);

CREATE TABLE IF NOT EXISTS FormMappings (
    source_plugin TEXT NOT NULL,
    source_plugin_id INTEGER,
    source_object_id INTEGER NOT NULL,
    output_plugin TEXT NOT NULL,
    output_index INTEGER NOT NULL,
    output_object_id INTEGER NOT NULL,
    signature TEXT,
    editor_id TEXT
);
CREATE INDEX IF NOT EXISTS idx_mappings_src ON FormMappings(source_plugin_id, source_object_id);

CREATE TABLE IF NOT EXISTS GeneratedRecords (
    id INTEGER PRIMARY KEY,
    output_index INTEGER NOT NULL,
    output_plugin TEXT NOT NULL,
    form_id TEXT NOT NULL,
    signature TEXT NOT NULL,
    editor_id TEXT,
    origin_plugin_id INTEGER,
    object_id INTEGER,
    is_new INTEGER NOT NULL,
    merged INTEGER NOT NULL,
    winner_plugin_id INTEGER,
    contributors TEXT,
    size_bytes INTEGER
);
CREATE INDEX IF NOT EXISTS idx_generated_output ON GeneratedRecords(output_index);
CREATE INDEX IF NOT EXISTS idx_generated_key ON GeneratedRecords(origin_plugin_id, object_id);

CREATE TABLE IF NOT EXISTS ValidationResults (
    id INTEGER PRIMARY KEY,
    stage TEXT NOT NULL,
    severity TEXT NOT NULL,
    category TEXT NOT NULL,
    message TEXT NOT NULL,
    plugin TEXT,
    form_id TEXT,
    cause TEXT
);
CREATE INDEX IF NOT EXISTS idx_validation_sev ON ValidationResults(severity);

CREATE TABLE IF NOT EXISTS Snapshots (
    kind TEXT PRIMARY KEY,
    created INTEGER NOT NULL,
    data TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS Changes (
    id INTEGER PRIMARY KEY,
    category TEXT NOT NULL,
    kind TEXT NOT NULL,
    name TEXT NOT NULL,
    detail TEXT,
    affects_build INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS RecordAssets (
    output_plugin TEXT NOT NULL,
    form_id TEXT NOT NULL,
    editor_id TEXT,
    asset_path TEXT NOT NULL,
    role TEXT NOT NULL,
    via TEXT,
    found INTEGER NOT NULL,
    source_kind TEXT,
    origin_mod TEXT,
    archive TEXT,
    archive_owner TEXT,
    physical TEXT,
    needs_registration INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_record_assets ON RecordAssets(output_plugin, form_id);
CREATE INDEX IF NOT EXISTS idx_record_assets_path ON RecordAssets(asset_path);

CREATE TABLE IF NOT EXISTS UserResolutions (
    origin_plugin TEXT NOT NULL,
    object_id INTEGER NOT NULL,
    winner_plugin TEXT NOT NULL,
    PRIMARY KEY (origin_plugin, object_id)
);
)SQL";

} // namespace fo4mx::db
