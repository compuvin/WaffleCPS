# Sample Codeplug Research

The private sample is in `Codeplugs/`, which Git ignores. Do not copy the file or its owner-specific identifiers into public fixtures.

## Confirmed from read-only inspection

- The sample is a 13,027,285-byte `.rdbx` binary file.
- Decoding its repeating 256-byte mask in memory reveals `CS7000-M17 PLUS` and CPS version `V1.02.20.00` in the file metadata.
- A fixed-width UTF-16LE name table contains 821 consecutive nonempty channel-name slots.
- Another populated area contains many zone names, including geographic repeater groups and separate simplex, GMRS, receive-only, and hotspot zones. The exact zone record count and membership layout remain unverified.
- Additional populated areas include radio settings and other record types. Their full meaning has not been established.

These observations show that a repeater-only editor would leave significant parts of a real codeplug without a home. They also give us a large test case for import performance and for the mixed-zone view.

## Work needed before importing or exporting

1. Locate channel records and verify name-to-record indexing, RX/TX frequency units, mode, color code, time slot, contact reference, and other channel settings against the manufacturer's CPS.
2. Decode zone records, membership indexes, order, and limits. Check whether zone names and membership are stored together or in separate tables.
3. Map contacts, receive groups, scan lists, and radio-specific settings, including unknown bytes that must survive a round trip.
4. Compare a small set of controlled CPS edits against binary copies to confirm field locations and checksums or length fields.
5. Test unchanged import and export on copies before trying radio communication. Never use the sole original file as an output target.

The sample identifies the **M17 Plus** variant. Compatibility with other CS7000 models or firmware versions is an open question; the adapter should check model metadata rather than treating all CS7000 files as interchangeable.
