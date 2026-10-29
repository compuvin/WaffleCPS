# Definition-driven codeplug import and export

Updated: 2026-10-28.

File import/export uses one native C++ definition interpreter. It loads packaged
version-2 `.wcp` files from the definitions directory. It has no model-specific
branches, table-name aliases, or substitutions for missing definition rules.
CS750 now supplies executable construction rules; its radio-specific addresses and policies remain in the definition.

## Selecting a definition

The import dialog lists every `.wcp` file found, including definitions that fail
to load. Selecting an individual definition forces that file; a failed attempt
reports its filename and error instead of trying another definition.

The aggregate filter and **All files** use content recognition. Ambiguous
recognition requires an explicit definition selection. The selected filename is
retained with the imported project and used for export after Save/Open.

A definition without a declared extension remains selectable with a `*` browsing
filter labeled **no extension declared**. This does not invent an extension or
relax parsing. Export requires an extension on the matched representation.

Definitions are loaded at first use and cached for the process lifetime. Restart
WaffleCPS after changing a definition. Temporary stdout logs and message boxes
identify the actual paths and JSON hashes; see
[DefinitionDebugging.md](DefinitionDebugging.md) for their cleanup inventory.

## Binary decoding and editor meaning are separate

Binary descriptors specify addresses, types, counts, selection, references, and
encoding. The reader supports the operators used by all four repository
definitions, including `span_count`, segmented tables, index/bitmap/probe
selection, BCD, endian integers, text, and packed names. Binary validation checks
the declared layout and references without requiring editor/export instructions.

`CodeplugDefinition::inspect()` exposes decoded globals and records independently
of editor import. Regression tests compare its values, raw values, physical
indices, and addresses with `tools/inspect_codeplug.py` for all four packaged
templates. Python is a test oracle, not a production parsing dependency.

The editor needs explicit meanings for channels, contacts, zones, and radio
identity. Those meanings come from construction writes' `from_target` values,
such as `transmit_frequency`, `receive_frequency`, `call_type`, and `members`.
Writes are grouped by their declared storage table. A complete, unique set of
target meanings identifies each editor role. Missing or ambiguous meanings are
errors. Physical field names alone are not treated as semantic aliases.

For example, an uninterpreted `frequency_a` field is not assumed to be transmit
or receive. A construction write must explicitly associate that storage field
with the corresponding target meaning. Channel mode can be declared by a write
or by the definition's explicit mode-default descriptor.

Record ordering and joins follow declared indexes and references. Physical
contact/channel indices are mapped to editor indices; they are not assumed to
be consecutive or interchangeable with editor positions.

## Construction operations do not determine radio/editor roles

The writer dispatches on `operation`, not on radio identity, binary table names,
or construction-step labels. Table-writing primitives can be repeated for
independent tables. Fixed records are not assumed to mean channels, variable
records are not assumed to mean zones, and overlapping records are not assumed
to mean contacts.

The implemented version-1 operations include fixed-table rebuilding,
variable-table replacement, pointer relocation, relative-index rewriting,
overlapping initialization/packing, field writes, representation encoding, and
target/output validation. Unsupported operations or policies fail explicitly.

Each variable-table replacement can declare its own named context values.
Context names must be unique, and a value must be produced before use. Record
sizes are evaluated using encoded target scalar values, including enum/scale
conversion, rather than values accidentally read from unrelated source bytes.
A size expression that depends on a stored value not supplied by the writes
fails explicitly.

Overlapping-table initialization and packing are paired by table and declared
step references. Destructive writes after packing are rejected. Source ranges,
field bounds, masks, reference integrity, and decoded export values are checked.
No result is returned to the caller on a failed operation.

The editor's canonical data types and supported command vocabulary still define
the application's capabilities. A future definition needing a new operation,
policy, or editor value type must declare it and receive explicit support;
missing functionality is not silently approximated.

## Status of the current definitions

| Definition | Binary template decoding | Editor import/export |
| --- | --- | --- |
| `cs7000-m17-plus.wcp` | Passes | Executable construction rules are supplied; existing sample import/export parity tests pass. |
| `cs750.wcp` | Passes | Executable construction rules; native import/export and three-level power roundtrips tested. Vendor CPS validation is pending. |
| `cs800.wcp` | Passes | Construction gap remains; `.rdb` is declared. |
| `opengd77-128k.wcp` | Passes | Fails explicitly: construction contains reset instructions, without executable writer version/operation order. |

Being selectable or passing binary inspection does not establish that a
definition supplies complete editor/export instructions. Improving those
definitions is a separate task. No per-model workaround was added for these
gaps.

## Scope and verification

The existing **New** workflow and radio transport were not redesigned. The old
reader/writer sources remain in the repository. Application file import/export
and retained-project source recovery use the shared definition interpreter.
Existing radio-specific transport code remains separate.

Coverage includes:

- All four disk definitions remain selectable and decode consistently with the
  Python inspector.
- Changed external extensions and writer limits affect behavior without an
  executable rebuild; missing files cannot fall back to embedded definitions.
- Explicit selection survives ambiguous recognition and project persistence.
- Existing CS7000 sample exports remain byte-identical to the previous writer.
- A temporary definition renames all storage fields, tables, step labels, and
  the model marker, then imports, edits, exports, and reimports.
- An independent layout uses repeated fixed-table operations for contacts,
  zones, and channels, with BCD and big-endian frequency fields.
- Another layout replaces two variable tables, shrinks one and grows the other,
  relocates pointers through separate named contexts, and preserves a marker in
  the shifted, otherwise uninterpreted data.
- Missing target meanings, undeclared record-size inputs, duplicate/forward
  contexts, malformed plans, and invalid references are rejected without
  replacing caller output.

Final verification on 2026-10-28: `cmake --build build -j 4` succeeded and
`ctest --test-dir build --output-on-failure` passed **36/36 tests** in 343.10
seconds. This includes 22 byte-for-byte comparisons against the existing writer
and the independent construction-layout tests above. `git diff --check` passed.
The final test log is saved at `/tmp/wafflecps-parser-final-tests.log` and the
parser repair diff at `/tmp/wafflecps-parser-repair.diff`.

The parser repair is ready for user testing. No physical-radio hardware testing
was performed during this repair.

## Fixed records, reference repair, and power settings

`rebuild_fixed_table` can omit an index table or writable count field when
storage has neither. A declared capacity bounds the target count. Optional
`clear_ranges` clear only declared portions within a selected source record.
`resize_to_end` is permitted only when the original table occupies the image
end; other sections must never be truncated as a side effect.

`remap_references` uses retained physical source indices to repair declared
receive-group/scan-list member lists and global zone references after table
rebuilds. List deletion policy is explicitly `remove_member`; scalar deletion
policy is `first_record`. The definition names every location and policy.
Image-size fields are also listed explicitly. There are no model branches.

Imported channels carry definition enum power values. The GUI choices come
from the bound power field's enum, and projects retain the selected value.
Legacy High/Low project values remain readable. Choices and capacities are
recovered from the retained source definition on project reopen.

`creation.default_for_new` selects an explicitly declared default when multiple
editor-capable definitions load. CS7000 declares it so the existing New workflow
continues to use its template; target-selection UI remains deferred.

`cs750_roundtrip_test` checks unchanged bytes, add/delete and contact-table
resize, invalid power rejection without output mutation, Medium power and
project persistence. `--samples` additionally exercises the supplied large
CS750 codeplug, reordered contacts and zone deletion.
