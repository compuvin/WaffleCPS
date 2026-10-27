"""Run: python3 -B -m unittest discover -s tests -p 'test_*codeplug.py'."""
from collections import Counter
from copy import deepcopy
from pathlib import Path
import unittest
import zipfile

from tools import inspect_codeplug as inspector
from tools.inspect_codeplug import GenerationError, create_default_codeplug, write_field, scope_for

ROOT = Path(__file__).resolve().parents[1]


def synthetic():
    data = bytearray(80)
    data[:5] = b"TEST\0"
    data[8:16] = b"OWNER\0\0\0"
    data[20:23] = (1234567).to_bytes(3, "little")
    data[24] = 2
    for i in range(2):
        at = 32 + i * 16
        data[at:at + 2] = (100 + i).to_bytes(2, "little")
        data[at + 2] = 0x3A
        data[at + 3] = 0xA5
        data[at + 4:at + 12] = b"OLD\0\0\0\0\0"
        data[at + 12:at + 16] = b"keep"
    data[72:80] = b"opaque!!"
    definition = {
        "definition_type": "codeplug", "definition_version": 1,
        "representations": [{"name": "plain", "decode": {"operation": "identity"}}],
        "recognition": {"model_field": "model", "model_value": "TEST"},
        "fields": {"model": {"at": 0, "type": "ascii", "bytes": 5},
                   "owner": {"at": 8, "type": "ascii", "bytes": 8},
                   "identifier": {"at": 20, "type": "u24le"},
                   "count": {"at": 24, "type": "u8", "maximum": 2}},
        "tables": {"items": {"at": 32, "count": {"field": "count"}, "stride_bytes": 16,
                              "fields": {"number": {"offset": 0, "type": "u16le"},
                                         "power": {"offset": 2, "type": "u8", "mask": "0xF0", "shift": 4},
                                         "name": {"offset": 4, "type": "ascii", "bytes": 8}}}},
        "default_generation": {"version": 1, "tables": [
            {"table": "items", "count_field": "count", "records": [
                {"copy": {"match": {"number": 100}},
                 "fields": {"number": 400, "power": 5, "name": "DEFAULT"}}]}],
            "globals": {"owner": "NOCALL", "identifier": 1}},
    }
    return bytes(data), definition


def package(model):
    path = ROOT / "definitions" / (model + ".wcp")
    definition = inspector.load_definition(path)
    with zipfile.ZipFile(path) as archive:
        source = archive.read(definition["creation"]["template"])
    return source, definition


def decode(source, definition):
    return inspector.decode_representation(source, definition["representations"][0])[0]


def encode(image, source, definition):
    rule = definition["representations"][0]["decode"]
    if rule["operation"] == "identity":
        return bytes(image)
    mask = source[inspector.integer(rule["mask_input_offset"]):][:inspector.integer(rule["mask_bytes"])]
    return bytes(value ^ mask[i % len(mask)] for i, value in enumerate(image))


class DefaultGeneratorTests(unittest.TestCase):
    def test_definition_driven_copy_masked_fields_counts_and_preservation(self):
        source, definition = synthetic()
        before = deepcopy(definition)
        result = create_default_codeplug(source, definition)
        checked = inspector.inspect_definition(result, definition)[0]
        self.assertEqual(checked["status"], "recognized")
        self.assertEqual(checked["decoded"]["fields"]["owner"], "NOCALL")
        self.assertEqual(checked["decoded"]["fields"]["identifier"], 1)
        self.assertEqual(checked["decoded"]["fields"]["count"], 1)
        self.assertEqual(result[34], 0x5A)  # Preserve the low nibble outside the mask.
        self.assertEqual(result[35], 0xA5)
        self.assertEqual(result[44:48], b"keep")
        self.assertEqual(result[48:64], bytes(16))
        self.assertEqual(result[72:], source[72:])
        self.assertEqual(definition, before)
        self.assertEqual(source, synthetic()[0])

    def test_idempotent_with_record_fallback_when_original_match_has_gone(self):
        source, definition = synthetic()
        definition["default_generation"]["tables"][0]["records"][0]["copy"]["fallback_hex"] = source[32:48].hex()
        first = create_default_codeplug(source, definition)
        self.assertEqual(create_default_codeplug(first, definition), first)

    def test_missing_profile_and_unrecognized_source_are_rejected(self):
        source, definition = synthetic()
        plain = deepcopy(definition)
        del plain["default_generation"]
        with self.assertRaisesRegex(GenerationError, "no supported"):
            create_default_codeplug(source, plain)
        with self.assertRaisesRegex(GenerationError, "fully recognized"):
            create_default_codeplug(b"WRONG" + source[5:], definition)

    def test_bad_defaults_bounds_and_array_lengths_fail_without_mutating_source(self):
        source, definition = synthetic()
        for field, value in (("number", 65536), ("number", -1), ("number", True),
                             ("power", 16), ("name", "TooLongForField"), ("absent", 1)):
            changed = deepcopy(definition)
            changed["default_generation"]["tables"][0]["records"][0]["fields"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(GenerationError):
                create_default_codeplug(source, changed)
        self.assertEqual(source, synthetic()[0])
        source, definition = package("cs750")
        changed = deepcopy(definition)
        zone = next(p for p in changed["default_generation"]["tables"] if p["table"] == "zone_slots")
        zone["records"][0]["fields"]["members"] = [1]
        with self.assertRaisesRegex(GenerationError, "array length"):
            create_default_codeplug(source, changed)

    def test_encoder_handles_bcd_utf16_packed_padding_and_rejects_invalid_types(self):
        image = bytearray(32)
        definition = {"fields": {}}
        reader = inspector.DefinitionReader(image, definition)
        scope = scope_for(reader, {}, None)
        write_field(image, reader, {"at": 0, "type": "bcdle", "bytes": 4}, 12345678, scope)
        self.assertEqual(image[:4], bytes.fromhex("78 56 34 12"))
        write_field(image, reader, {"at": 4, "type": "utf16le", "bytes": 8}, "ÅB", scope)
        self.assertEqual(image[4:12], "ÅB\0\0".encode("utf-16-le"))
        field = {"at": 12, "type": "packed6", "bytes": 3, "characters": 4,
                 "alphabet": "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz .", "padding_code": 62}
        write_field(image, reader, field, "A.", scope)
        self.assertEqual(reader.read_scalar(field, 12, "name")["value"], "A.  ")
        with self.assertRaises(GenerationError):
            write_field(image, reader, field, "?", scope)
        with self.assertRaises(inspector.DefinitionError):
            write_field(image, reader, {"at": 31, "type": "u24le"}, 1, scope)

    def test_creation_of_both_models_has_expected_defaults_and_no_broken_references(self):
        for model in ("cs750", "cs7000-m17-plus"):
            with self.subTest(model=model):
                source, definition = package(model)
                result = create_default_codeplug(source, definition)
                checked = inspector.inspect_definition(result, definition)[0]
                self.assertEqual(checked["status"], "recognized", checked.get("errors"))
                decoded = checked["decoded"]
                self.assertEqual(decoded["fields"]["radio_name"], "NOCALL")
                self.assertEqual(decoded["fields"]["radio_id"], 1)
                self.assertEqual(decoded["fields"]["contact_count"], 1)
                self.assertEqual(decoded["tables"]["contacts"]["records"][0]["fields"]["id"], 9)
                channels = "channel_slots" if model == "cs750" else "channels"
                self.assertEqual(len(decoded["tables"][channels]["records"]), 2)
                self.assertEqual([r["fields"]["name"] for r in decoded["tables"]["channel_names"]["records"]], ["ACH 1", "DCH 1"])
                self.assertEqual(len(decoded["tables"]["receive_groups"]["records"]), 1)
                self.assertEqual(len(decoded["tables"]["scan_lists"]["records"]), 1)
                self.assertTrue(all(c["status"] == "match" for c in checked["checks"] if c["category"] == "references"))
                self.assertEqual(create_default_codeplug(result, definition), result)

    def test_removed_contact_bodies_are_not_kept_in_cs750_output(self):
        source, definition = package("cs750")
        image = bytearray(source)
        image.extend((1234567).to_bytes(3, "little") + bytes(12) + b"\0")
        image[0x9568:0x956C] = (2).to_bytes(4, "little")
        image[0x2064:0x2068] = len(image).to_bytes(4, "little")
        before = bytes(image)
        result = create_default_codeplug(before, definition)
        self.assertEqual(len(result), 0x44000 + 16)
        self.assertEqual(len(before), len(result) + 16)
        self.assertEqual(bytes(image), before)

    def test_empty_lists_are_valid_as_input_and_as_defaults(self):
        for model in ("cs750", "cs7000-m17-plus"):
            source, definition = package(model)
            empty = deepcopy(definition)
            for plan in empty["default_generation"]["tables"]:
                if plan["table"] in ("scan_list_index", "scan_lists", "receive_group_index", "receive_groups", "receive_group_members"):
                    plan["records"] = []
                if plan["table"] in ("channel_slots", "channels"):
                    plan["records"][1]["fields"]["receive_group_index"] = 0
            with self.subTest(model=model):
                result = create_default_codeplug(source, empty)
                checked = inspector.inspect_definition(result, empty)[0]
                self.assertEqual(checked["status"], "recognized", checked.get("errors"))
                self.assertEqual(checked["decoded"]["tables"]["receive_groups"]["records"], [])
                self.assertEqual(checked["decoded"]["tables"]["scan_lists"]["records"], [])
                # Original profile must also work when no source lists are active.
                restored = create_default_codeplug(result, definition)
                self.assertEqual(inspector.inspect_definition(restored, definition)[0]["status"], "recognized")

    def test_bad_nonzero_group_and_list_member_references_are_rejected(self):
        for model in ("cs750", "cs7000-m17-plus"):
            source, definition = package(model)
            decoded = decode(source, definition)
            result = inspector.inspect_definition(source, definition)[0]["decoded"]["tables"]
            cases = []
            channel = result["channel_slots" if model == "cs750" else "channels"]["records"][1]["address"]
            cases.append((channel + 22, b"\xFA", "receive_group_index"))
            scan = result["scan_lists"]["records"][0]["address"]
            cases.append((scan + 66, b"\xFF\x7F", "scan_lists.members"))
            if model == "cs750":
                group = result["receive_groups"]["records"][0]["address"]
                cases.append((group + 36, b"\xFF\x7F", "receive_groups.members"))
            else:
                member = result["receive_group_members"]["records"][0]["address"]
                cases.append((member, b"\xFF\xFF\xFF\x7F", "receive_group_members.members"))
            for at, value, key in cases:
                with self.subTest(model=model, key=key):
                    bad = bytearray(decoded)
                    bad[at:at + len(value)] = value
                    damaged = encode(bad, source, definition)
                    checked = inspector.inspect_definition(damaged, definition)[0]
                    self.assertEqual(next(c for c in checked["checks"] if c["name"].endswith(key))["status"], "mismatch")
                    with self.assertRaisesRegex(GenerationError, "fully recognized"):
                        create_default_codeplug(damaged, definition)

    def test_unmapped_settings_survive_and_cs7000_zone_tail_relocates(self):
        source, definition = package("cs7000-m17-plus")
        image = bytearray(decode(source, definition))
        marker = b"opaque-settings"
        reader = inspector.DefinitionReader(image, definition)
        identity_at = reader.expression(definition["fields"]["radio_id"]["at"])
        image[identity_at + 24:identity_at + 24 + len(marker)] = marker
        changed = encode(image, source, definition)
        expand = deepcopy(definition)
        for plan in expand["default_generation"]["tables"]:
            if plan["table"] == "zones":
                plan["records"].append({"fields": {"name": "Extra zone", "member_count": 1, "members": [2]}})
            if plan["table"] == "zone_index":
                plan["records"] = [{"fields": {"relative_start": 40}}]
                plan["capacity"] = 1
        expanded = create_default_codeplug(changed, expand)
        self.assertEqual(len(expanded), len(source) + 38)
        restored = create_default_codeplug(expanded, definition)
        self.assertEqual(len(restored), len(source))
        restored_image = decode(restored, definition)
        identity_at = inspector.DefinitionReader(restored_image, definition).expression(definition["fields"]["radio_id"]["at"])
        self.assertEqual(restored_image[identity_at + 24:identity_at + 24 + len(marker)], marker)


    def test_sparse_receive_groups_parallel_counts_and_reordered_definition(self):
        source, definition = package("cs7000-m17-plus")
        image = bytearray(decode(source, definition))
        fields = inspector.DefinitionReader(image, definition).globals.decoded()[0]
        index = fields["receive_group_index_address"]
        names = fields["receive_group_records_address"]
        members = fields["receive_group_members_address"]
        name_record = bytes(image[names:names + 162])
        member_record = bytes(image[members:members + 256])
        image[names:names + 162] = bytes(162)
        image[members:members + 256] = bytes(256)
        image[names + 324:names + 486] = name_record
        image[members + 512:members + 768] = member_record
        image[index + 2] = 3
        channel = fields["channel_records_address"] + 48
        image[channel + 22] = 3
        sparse = encode(image, source, definition)
        reordered = deepcopy(definition)
        reordered["fields"] = dict(reversed(list(reordered["fields"].items())))
        reordered["tables"] = dict(reversed(list(reordered["tables"].items())))
        for table in reordered["tables"].values():
            table["fields"] = dict(reversed(list(table["fields"].items())))
        for plan in reordered["default_generation"]["tables"]:
            for record in plan["records"]:
                record["fields"] = dict(reversed(list(record.get("fields", {}).items())))
        checked = inspector.inspect_definition(sparse, reordered)[0]
        self.assertEqual(checked["status"], "recognized", checked.get("errors"))
        records = checked["decoded"]["tables"]["receive_group_members"]["records"]
        self.assertEqual([record["index"] for record in records], [2])
        self.assertEqual(records[0]["fields"]["members"], [1])
        normalized = create_default_codeplug(sparse, reordered)
        checked = inspector.inspect_definition(normalized, reordered)[0]
        self.assertEqual(checked["status"], "recognized")
        self.assertEqual(checked["decoded"]["tables"]["receive_groups"]["selected_indices"], [0])

    def test_cs750_scan_index_header_is_not_decoded_as_a_scan_record(self):
        source, definition = package("cs750")
        checked = inspector.inspect_definition(source, definition)[0]
        self.assertEqual(checked["status"], "recognized")
        decoded = checked["decoded"]
        self.assertEqual(decoded["fields"]["scan_list_count"], 1)
        self.assertEqual(decoded["fields"]["scan_list_slot_count"], 1)
        self.assertEqual(decoded["tables"]["scan_list_index"]["address"], 0x33502)
        self.assertEqual([r["address"] for r in decoded["tables"]["scan_lists"]["records"]], [0x2C000])
        self.assertEqual(int.from_bytes(source[0x33500:0x33502], "little"), 1)

    def test_duplicate_and_cyclic_selection_indexes_are_rejected(self):
        source, definition = synthetic()
        definition["fields"]["index_count"] = {"at": 25, "type": "u8"}
        indexes = {"at": 26, "count": {"field": "index_count"}, "stride_bytes": 1,
                   "fields": {"number": {"offset": 0, "type": "u8"}}}
        definition["tables"]["indexes"] = indexes
        definition["tables"]["items"]["validity"] = {"index_table": "indexes", "field": "number"}
        data = bytearray(source)
        data[25:28] = bytes([2, 1, 1])
        checked = inspector.inspect_definition(bytes(data), definition)[0]
        self.assertNotEqual(checked["status"], "recognized")
        self.assertTrue(any("Duplicate record selection index" in error["message"] for error in checked["errors"]))
        indexes["validity"] = {"index_table": "items", "field": "number"}
        checked = inspector.inspect_definition(bytes(data), definition)[0]
        self.assertEqual(checked["status"], "unreadable")
        self.assertIn("Cyclic table", checked["error"])

    def test_malformed_profiles_and_capacity_are_rejected(self):
        source, definition = synthetic()
        variants = [None, {"version": True}, {"version": 1}, {"version": 1, "tables": {}},
                    {"version": 1, "tables": [None]}]
        for profile in variants:
            changed = deepcopy(definition)
            changed["default_generation"] = profile
            with self.subTest(profile=profile), self.assertRaises(GenerationError):
                create_default_codeplug(source, changed)
        changed = deepcopy(definition)
        changed["default_generation"]["tables"].append(deepcopy(changed["default_generation"]["tables"][0]))
        with self.assertRaisesRegex(GenerationError, "Duplicate reset table"):
            create_default_codeplug(source, changed)
        changed = deepcopy(definition)
        changed["default_generation"]["tables"][0]["records"] = [None]
        with self.assertRaisesRegex(GenerationError, "record must be an object"):
            create_default_codeplug(source, changed)
        changed = deepcopy(definition)
        changed["default_generation"]["tables"][0]["capacity"] = 0
        with self.assertRaisesRegex(GenerationError, "No room"):
            create_default_codeplug(source, changed)


class OpenGD77DefaultTests(unittest.TestCase):
    def setUp(self):
        self.source, self.definition = package("opengd77-128k")

    def test_packaged_default_identity_fields_and_references(self):
        result = create_default_codeplug(self.source, self.definition)
        self.assertEqual(result, self.source)
        self.assertEqual(len(result), 131072)
        self.assertEqual(result[:7], b"MD-760P")
        checked = inspector.inspect_definition(result, self.definition)[0]
        self.assertEqual(checked["status"], "recognized", checked.get("errors"))
        decoded = checked["decoded"]
        self.assertEqual(decoded["fields"]["radio_name"], "NOCALL")
        self.assertEqual(decoded["fields"]["radio_id"], 1)
        self.assertEqual(result[0xE8:0xEC], b"\0\0\0\1")
        self.assertEqual(decoded["tables"]["channels"]["selected_indices"], [0])
        channel = decoded["tables"]["channels"]["records"][0]["fields"]
        self.assertEqual(channel["name"], "DCH 1")
        self.assertEqual(channel["receive_frequency"], 144025000)
        self.assertEqual(channel["transmit_frequency"], 144025000)
        self.assertEqual(channel["contact_number"], 1)
        self.assertEqual(decoded["tables"]["base_zones"]["records"][0]["fields"]["members"], [1] + [0] * 15)
        contact = decoded["tables"]["named_contacts"]["records"][0]["fields"]
        self.assertEqual((contact["name"], contact["id"], contact["call_type"]), ("Local", 9, "group"))

    def test_clears_disabled_bodies_and_preserves_unallocated_bitmap_bits_and_unknown_storage(self):
        data = bytearray(self.source)
        channels = self.definition["tables"]["channels"]["segments"]
        at = inspector.integer(channels[-1]["at"]) + 127 * 56
        data[at:at + 56] = b"X" * 56  # Disabled, so not interpreted as channel data.
        zone_at = 0x8030 + 249 * 48
        data[zone_at:zone_at + 48] = b"Y" * 48
        data[0x8010 + 31] |= 0xFC  # Six bits beyond the 250 allocated zone slots.
        contact_at = 0x17620 + 1023 * 24
        data[contact_at:contact_at + 24] = b"\0" + b"Z" * 23
        for i in range(1, 5):
            fields = {"name": f"Private {i}", "id": 1234500 + i, "call_type": 1}
            inspector.write_fields(data, inspector.DefinitionReader(data, self.definition),
                                   self.definition["tables"]["named_contacts"]["fields"], fields,
                                   0x17620 + 24 * i, i)
        data[0x1EE80:0x1EE8C] = b"opaque-data!"
        before = bytes(data)
        result = create_default_codeplug(before, self.definition)
        self.assertEqual(bytes(data), before)
        self.assertEqual(result[at:at + 56], bytes(56))
        self.assertEqual(result[zone_at:zone_at + 48], bytes(48))
        self.assertEqual(result[contact_at:contact_at + 24], bytes(24))
        self.assertEqual(result[0x17620 + 24:0x17620 + 5 * 24], bytes(4 * 24))
        self.assertEqual(result[0x8010 + 31] & 0xFC, 0xFC)
        self.assertEqual(result[0x1EE80:0x1EE8C], b"opaque-data!")
        self.assertEqual(inspector.inspect_definition(result, self.definition)[0]["status"], "recognized")

    def test_records_cross_banks_and_activate_only_their_own_slots(self):
        for count in (0, 129, 257, 1024):
            definition = deepcopy(self.definition)
            plan = next(p for p in definition["default_generation"]["tables"] if p["table"] == "channels")
            seed = deepcopy(plan["records"][0])
            plan["records"] = []
            for i in range(count):
                record = deepcopy(seed)
                record["fields"]["name"] = f"DCH {i + 1}"
                plan["records"].append(record)
            if count == 0:
                next(p for p in definition["default_generation"]["tables"] if p["table"] == "base_zones")["records"] = []
            with self.subTest(count=count):
                result = create_default_codeplug(self.source, definition)
                checked = inspector.inspect_definition(result, definition)[0]
                self.assertEqual(checked["status"], "recognized", checked.get("errors"))
                self.assertEqual(checked["decoded"]["tables"]["channels"]["selected_indices"], list(range(count)))
                for bank, segment in enumerate(definition["tables"]["channels"]["segments"]):
                    enabled = min(128, max(0, count - bank * 128))
                    bitmap = ((1 << enabled) - 1).to_bytes(16, "little")
                    at = inspector.integer(segment["validity"]["bitmap_at"])
                    self.assertEqual(result[at:at + 16], bitmap)
                for row in checked["decoded"]["tables"]["channels"]["records"]:
                    self.assertEqual(row["fields"]["name"], f"DCH {row['index'] + 1}")

    def test_sparse_source_bank_is_normalized_without_losing_the_record_seed(self):
        data = bytearray(self.source)
        source_at, target_at = 0x3790, 0x105F0
        data[target_at:target_at + 56] = data[source_at:source_at + 56]
        data[source_at:source_at + 56] = bytes(56)
        data[0x3780] = 0
        data[0x105E0] = 1
        data[0x8030 + 16:0x8030 + 18] = (513).to_bytes(2, "little")
        checked = inspector.inspect_definition(bytes(data), self.definition)[0]
        self.assertEqual(checked["status"], "recognized", checked.get("errors"))
        self.assertEqual(checked["decoded"]["tables"]["channels"]["selected_indices"], [512])
        result = create_default_codeplug(bytes(data), self.definition)
        checked = inspector.inspect_definition(result, self.definition)[0]
        self.assertEqual(checked["status"], "recognized")
        self.assertEqual(checked["decoded"]["tables"]["channels"]["selected_indices"], [0])
        self.assertEqual(result[target_at:target_at + 56], bytes(56))

    def test_segment_capacity_and_overlap_are_rejected(self):
        definition = deepcopy(self.definition)
        plan = next(p for p in definition["default_generation"]["tables"] if p["table"] == "channels")
        plan["records"] *= 1025
        with self.assertRaisesRegex(GenerationError, "No room"):
            create_default_codeplug(self.source, definition)
        definition = deepcopy(self.definition)
        segments = definition["tables"]["channels"]["segments"]
        segments[1] = deepcopy(segments[0])
        with self.assertRaisesRegex(GenerationError, "overlap"):
            create_default_codeplug(self.source, definition)

    def test_msb_bitmap_reset_preserves_bits_outside_capacity(self):
        source, definition = synthetic()
        data = bytearray(source)
        data[70] = 255
        definition["tables"]["items"]["count"] = 2
        definition["tables"]["items"]["validity"] = {"bitmap_at": 70, "bit_order": "msb_first"}
        plan = definition["default_generation"]["tables"][0]
        plan["reset_selection"] = True
        plan["clear_all_slots"] = True
        result = create_default_codeplug(bytes(data), definition)
        self.assertEqual(result[70], 0xBF)
        self.assertEqual(inspector.inspect_definition(result, definition)[0]["decoded"]["tables"]["items"]["selected_indices"], [0])

    def test_generator_lives_in_inspector_and_separate_module_is_removed(self):
        self.assertEqual(create_default_codeplug.__module__, "tools.inspect_codeplug")
        self.assertFalse((ROOT / "tools" / "default_codeplug.py").exists())


if __name__ == "__main__":
    unittest.main()
