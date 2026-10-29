# Definition parser repair checkpoint

**Completed 2026-10-28:** The historical checkpoint below records the pause
state. Its operation-to-editor-role restrictions have been removed, and the final
build passed all 36 tests. Current implementation details and verification are in
[DefinitionParser.md](DefinitionParser.md).

Date: 2026-10-27. Paused at the user's request. **The repair is not finished and
is not ready for final user testing.** Resume only when the user requests it.

## Authorization and constraints

The user approved this parser change without requiring a proposed diff first:
"Proceed without a diff this time. This one change is approved as you see fit."
That approval applies to completing the existing import/export repair.

- Keep the `.wcp` definition files unchanged. Corrections to their contents are
  a separate task requiring careful review.
- Keep the "New" workflow's behavior unchanged; target selection is deferred.
- Leave the old reader/writer code in the repository, but route application
  import/export through the shared definition parser.
- Interpret definition-language operations generically. Do not add model-specific
  aliases, guessed field meanings, or fallbacks for missing information.
- Every `.wcp` file in the directory must be selectable for an import attempt,
  including files whose loading/parsing fails. Failures must be explicit.
- Export must retain the definition selected at successful import.
- Do not ask the user to test an intermediate diagnostics-only implementation.
- Do not change project plan/checklist wording or revert existing user changes.

The user specifically questioned whether the implementation was being tailored
to the current definitions. The remaining architectural issue below matters to
that concern; renamed-schema tests alone are not sufficient to declare the
interpreter fully generic.

## Working tree and existing changes

There were already substantial uncommitted changes when this agent started,
including `CMakeLists.txt`, `definitions/cs7000-m17-plus.wcp`, the definition
engine/catalog, `src/repeaterstore.cpp`, and two untracked test sources.
Those preexisting changes have not been reverted. No definition file or CMake
file was changed by this agent. No commit was made.

The earlier approved dialog/diagnostics patch is documented in
[DefinitionDebugging.md](DefinitionDebugging.md). Its incremental diff is at
`/tmp/wafflecps-approved-changes.diff`.

Snapshots immediately before the current parser work are in
`/tmp/wafflecps-parser-before/`. The current parser-work diff is saved at
`/tmp/wafflecps-parser-in-progress.diff`. These temporary files are convenient
review aids, not replacements for the source files in this working tree.

## Implemented so far

### Shared binary reader

In `src/codeplugdefinition.cpp`:

- Added `span_count` with empty-pattern scanning and checked ranges/counts.
- Added segmented tables, bitmap selection in either bit order, and selection
  by excluded probe bytes. Existing index-table selection remains supported.
- Added little/big endian BCD decoding and additional endian integer forms.
- Added matching BCD/endian scalar write support to the writer primitives.
- Separated recognition, binary validation/inspection, and editor import.
- `CodeplugDefinition::load()` now validates the binary schema/template without
  requiring the editor mapping to succeed.
- Added `CodeplugDefinition::inspect()` to expose decoded fields, records,
  physical indices, and addresses for independent comparison.
- Preserved bound checks, reference validation, source preservation, and output
  validation. Output parameters are assigned only on success.

These binary-language features correspond to the existing
`tools/inspect_codeplug.py` implementation. Production parsing remains native C++;
Python is used only as a test oracle.

### Editor and writer bindings

- Removed literal CS7000 table/field-name lookups from editor import.
- Added `EditorBindings`, which derives storage destinations from explicit
  construction writes and mode-default declarations.
- Editor import now joins names by declared storage indices and maps physical
  contact/channel references to editor indices.
- Writer dispatch uses declared `operation` values instead of fixed step labels.
  Forward references to comparison/branch/packing steps resolve declared labels.
- Writer source-record selection accounts for physical channel indices.
- Target comparison still uses the editor's canonical semantic values; those
  semantic names are distinct from a definition's binary storage field names.

### Selection, dialogs, and retained identity

In `src/codeplugdefinitions.cpp`:

- The catalog retains every `.wcp` file, including loading failures.
- Import filters identify each definition by model and filename.
- Definitions without an extension are shown explicitly as such with a `*`
  browsing filter. This assigns no extension and bypasses no parsing requirement.
- Choosing an individual filter forces exactly that definition. Automatic
  detection fails on ambiguity rather than choosing an arbitrary match.
- Read/write errors identify the selected definition filename.
- Disk provenance and temporary debug logs/message boxes remain enabled.
- The existing default-codeplug behavior uses editor-ready definitions so that
  merely exposing binary-only definitions does not change the current "New"
  behavior. No target-selection UI was added.

`ImportedCodeplug::definitionFile` retains the selected definition basename.
`RepeaterStore` saves/reloads it as optional imported-project metadata and uses
it for export and attached-codeplug reimport. Old project files without it can
still use detection. Project-source recovery now calls the shared parser rather
than `CodeplugReader::read()`. Model metadata is no longer restricted to a literal
CS7000 name. An empty version is accepted only when the retained source can be
read by a definition that actually has no version value.

## Historical architectural issue — resolved 2026-10-28

`EditorBindings` still ties editor roles to particular operation kinds:

- Channels: `rebuild_fixed_table`.
- Zones: `replace_variable_table`.
- Contacts: `prepare_overlapping_table` plus `write_overlapping_fields`.

Although storage names and command labels are now definition-driven, that role
assignment still reflects the current complete definition's construction shape.
It could require unrelated radios to use overlapping-contact/variable-zone
operations even when their structures do not need them. Do not describe that as
fully removing the CS7000 assumptions.

Related limitations to review together:

- `constructionRule()` requires an operation kind to be unique.
- Writer validation currently rejects repeated kinds of operation, even when
  separate tables could legitimately require the same primitive.
- Several writer handlers still fetch canonical channel/zone/contact arrays
  directly based on operation kind instead of an explicit generic table binding.
- Context validation assumes one variable-table replacement operation.

The next step is to establish a generic way to bind explicit target semantics to
storage and dispatch table operations without treating the current file's
construction shape as the language. Do not solve this with per-radio branches,
silent aliases, or edits to the supplied definitions. If the existing definition
language cannot express a required editor/export relationship, report the
missing requirement explicitly and discuss any format change separately.

## Definition status observed during this work

All four repository templates now pass shared binary loading/inspection.

Only CS7000 currently supplies the executable editor/export construction rules
used by this implementation. CS750, CS800, and OpenGD77 supply reset instructions
instead of executable writer version/operation order, so editor import/export
currently fails explicitly with that requirement. CS800 also declares no filename
extension. None of those gaps was patched by changing definition data.

## Tests and validation

The application and all test targets build successfully. `git diff --check`
passes. The earlier diagnostics-only patch passed all 36 CTest tests; that result
does **not** certify this newer parser work.

Tests added/extended during the parser work:

- `tests/definition_disk.cpp`: all four files appear as explicit filter choices;
  each template's C++ decoded globals/records/indices/addresses is compared with
  normalized `inspect_codeplug.py` output; reset-only editor operations fail
  without changing output; ambiguous definitions require explicit selection;
  export retains the explicitly selected definition. Existing external-file,
  hash, malformed-definition, and missing-extension proof checks remain.
- `tests/codeplug_definition_export.cpp`: a temporary copy renames binary tables,
  binary fields, all construction step labels, and the radio model marker, then
  exercises import/edit/export/reimport. Existing old-writer byte-parity and
  malformed-plan checks remain. This tests independence from names; it does not
  by itself resolve the construction-shape limitation above.
- `tests/codeplug_import.cpp`: filter assertions now expect all repository
  definitions; save/open checks assert retained definition filename metadata.

Two initial checks failed because diagnostic wording/order changed, not because
bad data was accepted: corrupt slot-count errors no longer included "limit",
and an unknown operation was reported through its dependent packing rule.
Messages and operation-name preflight were corrected before the current run.

Final targeted result at pause: **4/4 tests passed**, in 83.79 seconds:

- `codeplug_definition`
- `codeplug_definition_export`
- `definition_disk`
- `codeplug_import`

Command: `ctest --test-dir build --output-on-failure -j 3 -R
'^(definition_disk|codeplug_definition|codeplug_definition_export|codeplug_import)$'`.

The test log is saved at `/tmp/wafflecps-parser-checkpoint-tests.log`.
The full 36-test suite must be rerun after the remaining parser design work and
any resulting code changes. No test process remains running at this pause.
