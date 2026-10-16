# Sample Codeplug Research

The private sample is in `Codeplugs/`, which Git ignores. Do not copy the file or its owner-specific identifiers into public fixtures.

## Confirmed from read-only inspection

- The sample is a 13,027,285-byte `.rdbx` binary file.
- Decoding its repeating 256-byte mask in memory reveals `CS7000-M17 PLUS` and CPS version `V1.02.20.00` in the file metadata.
- A fixed-width UTF-16LE name table contains 821 consecutive nonempty channel-name slots.
- Another populated area contains many zone names, including geographic repeater groups and separate simplex, GMRS, receive-only, and hotspot zones. The exact zone record count and membership layout remain unverified.
- Additional populated areas include radio settings and other record types. Their full meaning has not been established.

These observations show that a repeater-only editor would leave significant parts of a real codeplug without a home. They also give us a large test case for import performance and for the mixed-zone view.

## Verified first import

For the sample's CS7000-M17 Plus CPS V1.02.20.00 layout, WaffleCPS now decodes the 256-byte repeating XOR mask, 821 channel names and frequency records, and 82 zones with ordered one-based channel references. The digital marker, color code nibble, and time slot bit are inferred from the sample records. Six sample zones have channels sharing one non-simplex frequency pair and color code; they appear under repeater groups. The other 76 remain zones. The source bytes are archived unchanged in a `.waffle` project.

The two frequency fields are displayed in stored order while their RX/TX direction awaits independent verification. Other CPS versions are rejected because their offsets have not been checked. The importer is read only and does not decode contacts or other radio settings.

## Work needed before exporting

1. Verify the frequency field direction, mode and digital flag variants, contact references, and other channel settings against controlled CPS edits.
2. Confirm channel and zone limits and record offsets in other CPS versions.
3. Map contacts, receive groups, scan lists, and radio-specific settings, including unknown bytes that must survive a round trip.
4. Compare a small set of controlled CPS edits against binary copies to confirm field locations and checksums or length fields.
5. Test unchanged import and export on copies before trying radio communication. Never use the sole original file as an output target.

The sample identifies the **M17 Plus** variant. Compatibility with other CS7000 models or firmware versions is an open question; the adapter should check model metadata rather than treating all CS7000 files as interchangeable.
