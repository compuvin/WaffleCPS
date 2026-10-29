# Temporary definition-file diagnostics

Added: 2026-10-27.

These diagnostics were requested to demonstrate that import/export reads external
`.wcp` definition files rather than using embedded definition data. Keep this
inventory so the temporary instrumentation can be removed after the shared-parser
repair is complete and verified.

## Current status

The dialog and diagnostic changes remain enabled. Subsequent shared-parser work
removed the operation-to-editor-role restrictions recorded at the pause. Current
behavior, remaining definition-data gaps, and verification results are documented
in [DefinitionParser.md](DefinitionParser.md). The historical pause state remains
in [DefinitionParserCheckpoint.md](DefinitionParserCheckpoint.md).

All four current definitions can be selected and inspected by the shared binary
reader. CS750 now has executable import/export rules and definition-driven power
choices; see its research note for the remaining startup-channel mapping gap.
CS800 and OpenGD77 still fail editor import/export explicitly because their
construction sections lack executable writer version/operation order. Passing binary decoding tests does
not establish that those definitions support editor import/export.

Definitions remain unchanged by this diagnostic work. Definition corrections
are a separate task. The "New" workflow is outside the import/export repair.

## Flushed stdout logging

In `src/codeplugdefinition.cpp`, `CodeplugDefinition::load()` prints and flushes:

- The absolute path of the definition file being opened.
- Confirmation that `definition.json` was read from that file's archive.
- The SHA-256 of the exact `definition.json` bytes returned by the ZIP reader.

The hash covers the JSON archive member, not the complete `.wcp` package.

In `src/codeplugdefinitions.cpp`, `logDefinition()` prints and flushes:

- The selected definitions directory.
- Successful loads and their provenance.
- Rejected definition paths and loading errors.
- Definition operation errors.
- The cached disk definition selected for import, export-dialog construction,
  and export writing.

Search for `[definitions]`, `logDefinition`, `std::fprintf`, and `std::fflush`
when removing this instrumentation.

## Provenance state and report helpers

In `src/codeplugdefinition.h` and `.cpp`:

- `m_diskPath` stores the absolute source path.
- `m_definitionSha256` stores the JSON-member hash.
- `diskProvenance()` formats the path, definition ID, and hash.

In `src/codeplugdefinitions.h` and `.cpp`, `diskLoadReport()` assembles the
provenance report and rejected-definition list. It explains that definitions are
loaded once at first use and cached in memory. Restart the application after
changing files; the diagnostics do not implement hot reloading.

If provenance reporting is removed entirely, review these members, their
initialization, public declarations, and all callers together.

## Message boxes

In `src/repeaterstore.cpp`:

- `importCodeplugDialog()` shows **Definitions that failed to load** when needed.
  The former **Definitions read from files** dialog was removed on request;
  stdout provenance logging remains.
- `exportCodeplugDialog()` shows **Export definition read from file** for an
  imported codeplug, including the model and catalog provenance report.

These are temporary diagnostic interruptions. The separate rejected-definition
warning was part of the approved error-reporting repair; decide how to retain
visible loading errors before removing that warning. Do not silently hide failed
definitions again.

## Tests proving external-file behavior

`tests/definition_disk.cpp`, built as `definition_disk_test` and registered with
CTest as `definition_disk`, uses temporary definition files and fresh child
processes to check:

- Missing definition files cause failure rather than an embedded fallback.
- Changing the extension to `.disk` changes import and export filters without
  rebuilding the executable.
- Changing the radio-ID maximum changes writer validation without rebuilding.
- Restoring the file restores its original behavior and JSON hash.
- Removing the extension causes an explicit export-dialog failure.
- A malformed sibling definition appears in warnings and failed-import errors.
- Stdout identifies the actual temporary file and its expected JSON SHA-256.
- The provenance report includes the file, hash, and restart explanation.

The temporary `.wcp` files are created outside the repository definitions folder.
Repository definitions are not rewritten by these tests.

When removing diagnostics, remove or update assertions tied to stdout wording,
hash reporting, and diagnostic message text. The external-file behavior checks
are useful regression coverage and should be reviewed separately before removal.

## Validation of the diagnostic patch

The application built successfully and all 36 existing CTest tests passed,
including the extended disk-definition test. These results cover the diagnostic
patch; they do not prove multi-definition parser support or hardware correctness.

## Functional changes to preserve during cleanup

The following are functional import/export behavior, not temporary logging:

- Definition-derived import filters.
- Explicit reporting of rejected definitions and definition-specific failures.
- `CodeplugDefinition::exportExtension()` and
  `CodeplugDefinitions::exportFileFilter()`, which derive the export dialog from
  the matching representation and reject missing extensions.
- The explicit error for missing executable writer version/operation order.

Review the final diff against the instrumentation inventory before removing code.
