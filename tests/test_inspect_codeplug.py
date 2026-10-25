"""Run: python3 -B -m unittest discover -s tests -p test_inspect_codeplug.py"""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "inspect_codeplug.py"
SPEC = importlib.util.spec_from_file_location("inspect_codeplug", SCRIPT)
inspector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspector)


def fixture():
    """A small, unrelated format: no CS7000 offsets or model identifiers."""
    alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz  "
    data = bytearray(256)
    data[:5] = b"TEST\0"
    data[8:12] = b"1.0\0"
    data[16] = 2
    data[17:19] = (64).to_bytes(2, "little")
    data[-4:] = (123456).to_bytes(4, "little")
    for i, (identifier, name, kind) in enumerate(((7, "First", 1), (9, "Second", 0))):
        base = 64 + i * 8
        data[base + 5] = identifier
        bits = 0
        for character in name.ljust(8):
            bits = (bits << 6) | alphabet.index(character)
        data[base + 6:base + 12] = bits.to_bytes(6, "big")
        data[base + 12] = kind
    data[112:117] = bytes([2, 1, 2, 1, 2])
    data[144:152] = "Å\0".encode("utf-16-le") + b"\xff\xff\xff\xff"
    data[152:160] = "B\0\0\0".encode("utf-16-le")
    definition = {
        "definition_type": "codeplug", "definition_version": 1, "id": "synthetic",
        "representations": [
            {"name": "encoded", "decode": {"operation": "repeating_xor", "mask_input_offset": 224,
                                            "mask_bytes": 8, "mask_phase_at_input_zero": 0,
                                            "input_equal_ranges": [{"first_offset": 224, "second_offset": 232, "bytes": 8}]}},
            {"name": "plain", "decode": {"operation": "identity"}},
        ],
        "recognition": {"model_field": "marker", "model_value": "TEST",
                        "cps_version_field": "version", "supported_cps_versions": ["1.0"],
                        "optional_decoded_marker": {"at": 200, "hex": "AA BB", "required": False}},
        "fields": {
            "marker": {"at": 0, "type": "ascii", "bytes": 5, "require_terminator": True},
            "version": {"at": 8, "type": "ascii", "bytes": 4, "require_terminator": True},
            "count": {"at": 16, "type": "u8"},
            "pointer": {"at": 17, "type": "u16le"},
            "trailer": {"at": {"subtract": [{"image_size": True}, 4]}, "type": "u32le", "minimum": 1},
        },
        "layout_checks": {"expected_addresses": {"pointer": 64},
                          "relationships": [{"left": {"field": "trailer"}, "right": 123456}]},
        "tables": {
            "items": {"at": {"field": "pointer"}, "count": {"field": "count"},
                      "stride_bytes": 8, "logical_span_bytes": 13, "fields": {
                          "conditional_id": {"offset": 5, "type": "u8", "only_if": {
                              "value": {"record_field": "kind"}, "in": [1]}},
                          "id": {"offset": 5, "type": "u8", "minimum": 1},
                          "packed_name": {"offset": 6, "type": "packed6", "bytes": 6, "characters": 8,
                                          "alphabet": alphabet, "trim": "ascii_spaces_both_ends"},
                          "kind": {"offset": 12, "type": "u8", "mask": 1, "enum": {"0": "person", "1": "group"}},
                      }},
            "lists": {"at": 112, "count": 2, "record_bytes": {
                "add": [1, {"multiply": [1, {"record_field": "length"}]}]}, "fields": {
                    "members": {"offset": 1, "type": "u8", "count": {"record_field": "length"},
                                "stride_bytes": 1, "reference": {"table": "items", "index_base": 1}},
                    "length": {"offset": 0, "type": "u8"},
                }},
            "names": {"at": 144, "count": {"field": "count"}, "stride_bytes": 8,
                      "fields": {"name": {"offset": 0, "type": "utf16le", "bytes": 8}},
                      "join": {"table": "items", "rule": "Records with the same zero-based storage index describe the same channel."}},
        },
    }
    return bytes(data), definition


def blank_match_fixture(pointer_value=0, filler=0):
    data = bytearray(256)
    data[16:20] = pointer_value.to_bytes(4, "little")
    data[20:24] = pointer_value.to_bytes(4, "little")
    data[28] = 4
    data[64:72] = data[80:88] = bytes([filler]) * 8
    definition = {
        "definition_type": "codeplug", "definition_version": 1,
        "representations": [{"name": "plain", "decode": {"operation": "identity", "input_equal_ranges": [
            {"first_offset": 64, "second_offset": 80, "bytes": 8}]}}],
        "recognition": {"model_field": "model", "model_value": "TEST",
                        "cps_version_field": "version", "supported_cps_versions": ["1.0"],
                        "optional_decoded_marker": {"at": 64, "hex": bytes([filler]).hex(), "required": False}},
        "fields": {
            "model": {"at": 0, "type": "ascii", "bytes": 5, "terminators": [0, 255]},
            "version": {"at": 8, "type": "ascii", "bytes": 4, "terminators": [0, 255]},
            "pointer_a": {"at": 16, "type": "u32le"},
            "pointer_b": {"at": 20, "type": "u32le"},
            "empty_count": {"at": 24, "type": "u8"},
            "other_count": {"at": 28, "type": "u8"},
        },
        "layout_checks": {
            "equal_fields": [["pointer_a", "pointer_b"]],
            "expected_addresses": {"pointer_a": pointer_value},
            "relationships": [
                {"left": {"max": [{"field": "empty_count"}, {"field": "other_count"}]},
                 "right": {"field": "other_count"}},
                {"left": {"image_size": True}, "right": {"image_size": True}},
            ],
        },
        "tables": {
            "empty_index": {"at": 32, "count": 0, "stride_bytes": 1, "fields": {
                "number": {"offset": 0, "type": "u8", "reference": {"table": "empty_slots", "index_base": 1}}}},
            "empty_slots": {"at": 32, "count": 0, "stride_bytes": 1, "fields": {
                "value": {"offset": 0, "type": "u8"}}},
            "empty_names": {"at": 32, "count": 0, "stride_bytes": 1, "fields": {
                "value": {"offset": 0, "type": "u8"}}, "join": {
                    "table": "empty_slots", "rule": "Records with the same zero-based storage index describe the same channel."}},
        },
    }
    return bytes(data), definition


class InspectorTests(unittest.TestCase):
    def setUp(self):
        self.data, self.definition = fixture()

    def inspect(self, data=None, definition=None, **kwargs):
        return inspector.inspect_definition(self.data if data is None else data,
                                            self.definition if definition is None else definition, **kwargs)

    def test_plain_image_packed_overlap_arrays_and_order_independent_dependencies(self):
        results = self.inspect()
        self.assertEqual(results[0]["status"], "recognized")
        self.assertEqual(results[1]["equivalent_to"], "encoded")
        decoded = results[0]["decoded"]
        items = decoded["tables"]["items"]["records"]
        self.assertEqual([r["fields"]["packed_name"] for r in items], ["First", "Second"])
        self.assertEqual([r["fields"]["kind"] for r in items], ["group", "person"])
        self.assertEqual(items[0]["fields"]["conditional_id"], 7)
        self.assertNotIn("conditional_id", items[1]["fields"])
        self.assertEqual([r["fields"]["members"] for r in decoded["tables"]["lists"]["records"]], [[1, 2], [2]])
        self.assertEqual(decoded["tables"]["names"]["records"][0]["fields"]["name"], "Å")
        self.assertEqual(decoded["fields"]["trailer"], 123456)

    def test_xor_image_and_unknown_version_partial_match(self):
        mask = bytes([17, 29, 43, 59, 71, 83, 97, 109])
        encoded = bytes(value ^ mask[i % len(mask)] for i, value in enumerate(self.data))
        results = self.inspect(encoded)
        self.assertEqual(results[0]["status"], "recognized")
        self.assertNotEqual(results[1]["status"], "recognized")
        changed = bytearray(encoded)
        changed[8:12] = bytes(a ^ mask[(8 + i) % len(mask)] for i, a in enumerate(b"2.0\0"))
        result = self.inspect(bytes(changed))[0]
        self.assertEqual(result["status"], "partial")
        self.assertTrue(result["decoded"]["tables"]["items"]["complete"])
        self.assertTrue(any(c["name"] == "CPS version" and c["status"] == "mismatch" for c in result["checks"]))

    def test_mismatched_model_still_reports_structural_similarities(self):
        data = bytearray(self.data)
        data[:5] = b"ELSE\0"
        result = self.inspect(bytes(data))[0]
        self.assertEqual(result["status"], "partial")
        self.assertEqual(result["decoded"]["fields"]["marker"], "ELSE")

    def test_random_empty_and_truncated_inputs_do_not_claim_recognition(self):
        for data in (b"", bytes(12), bytes([255]) * 256, self.data[:100]):
            with self.subTest(size=len(data)):
                self.assertFalse(any(r["status"] == "recognized" for r in self.inspect(data)))
        self.assertEqual(self.inspect(bytes([255]) * 256)[1]["status"], "no_match")

    def test_bad_count_and_reference_are_reported_without_out_of_bounds_reads(self):
        data = bytearray(self.data)
        data[16] = 255
        result = self.inspect(bytes(data))[0]
        self.assertEqual(result["status"], "partial")
        self.assertEqual(result["decoded"]["tables"]["items"]["records"], [])
        data = bytearray(self.data)
        data[114] = 3
        result = self.inspect(bytes(data))[0]
        check = next(c for c in result["checks"] if c["name"] == "lists.members")
        self.assertEqual(check["status"], "mismatch")
        self.assertEqual(check["invalid"], [{"record": 0, "value": 3}])

    def test_cycles_and_unknown_operators_are_explicit_errors(self):
        definition = copy.deepcopy(self.definition)
        definition["fields"]["cycle_a"] = {"value": {"field": "cycle_b"}}
        definition["fields"]["cycle_b"] = {"value": {"field": "cycle_a"}}
        result = self.inspect(definition=definition)[0]
        self.assertEqual(result["status"], "partial")
        self.assertTrue(any("Cyclic" in e["message"] for e in result["errors"]))
        definition["fields"]["unsupported"] = {"value": {"execute": "anything"}}
        result = self.inspect(definition=definition)[0]
        self.assertTrue(any("Unsupported expression" in e["message"] for e in result["errors"]))

    def test_record_limit_and_invalid_text_leave_other_fields_available(self):
        result = self.inspect(max_records=1)[0]
        self.assertEqual(result["status"], "partial")
        self.assertTrue(any("Record limit" in e["message"] for e in result["errors"]))
        data = bytearray(self.data)
        data[144:146] = b"\x00\xd8"  # Unpaired UTF-16 surrogate before the terminator.
        result = self.inspect(bytes(data))[0]
        self.assertEqual(result["status"], "partial")
        self.assertEqual(result["decoded"]["tables"]["items"]["records"][0]["fields"]["id"], 7)

    def test_cli_reads_only_wcp_files_keeps_bad_definitions_and_never_changes_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            codeplug = root / "input.bin"
            codeplug.write_bytes(self.data)
            definitions = root / "defs"
            definitions.mkdir()
            (definitions / "good.wcp").write_text(json.dumps(self.definition))
            (definitions / "bad.wcp").write_text('{"broken":')
            (definitions / "ignored.wrd").write_text("not JSON")
            nested = definitions / "nested"
            nested.mkdir()
            other = copy.deepcopy(self.definition)
            other["id"] = "different-model"
            other["recognition"]["model_value"] = "OTHER"
            (nested / "other.wcp").write_text(json.dumps(other))
            before = hashlib.sha256(codeplug.read_bytes()).digest()
            process = subprocess.run([sys.executable, "-B", str(SCRIPT), str(codeplug),
                                      "--definitions", str(definitions), "--json"],
                                     capture_output=True, text=True, cwd=root)
            self.assertEqual(process.returncode, 0, process.stderr)
            report = json.loads(process.stdout)
            self.assertEqual(len(report["definitions"]), 3)
            self.assertIn("error", report["definitions"][0])
            self.assertEqual(report["definitions"][2]["results"][0]["status"], "partial")
            self.assertEqual(before, hashlib.sha256(codeplug.read_bytes()).digest())
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["defs", "input.bin"])

    def test_duplicate_json_keys_and_missing_definition_directory(self):
        with self.assertRaises(inspector.DefinitionError):
            json.loads('{"a":1,"a":2}', object_pairs_hook=inspector.unique_keys)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.bin"
            path.write_bytes(self.data)
            process = subprocess.run([sys.executable, "-B", str(SCRIPT), str(path),
                                      "--definitions", str(Path(directory) / "missing")],
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 2)
            self.assertIn("Definition directory not found", process.stderr)

    def test_invalid_definition_shapes_are_rejected_and_reference_checks_stay_incomplete(self):
        for key, value in (("tables", []), ("fields", []), ("representations", [3])):
            definition = copy.deepcopy(self.definition)
            definition[key] = value
            with self.subTest(key=key), self.assertRaises(inspector.DefinitionError):
                self.inspect(definition=definition)
        definition = copy.deepcopy(self.definition)
        definition["tables"]["items"]["fields"]["id"]["minimum"] = 8
        result = self.inspect(definition=definition)[0]
        self.assertFalse(result["decoded"]["tables"]["items"]["complete"])
        check = next(c for c in result["checks"] if c["name"] == "lists.members")
        self.assertEqual(check["status"], "unreadable")

    def test_span_count_keeps_empty_gaps_and_respects_the_end_boundary(self):
        data = bytearray(40)
        data[2] = 1
        data[18] = 2
        data[26] = 3  # A full record, but beyond the requested scan boundary.
        reader = inspector.DefinitionReader(bytes(data), self.definition)
        rule = {"at": 0, "end": 24, "stride_bytes": 8, "probe_offset": 2, "empty_hex": "00 00"}
        self.assertEqual(reader.expression({"span_count": rule}), 3)
        self.assertEqual(reader.expression({"span_count": dict(rule, end=20)}), 1)
        self.assertEqual(reader.expression({"span_count": dict(rule, at=8, end=16)}), 0)

    def test_span_count_rejects_invalid_ranges_probes_and_excessive_work(self):
        reader = inspector.DefinitionReader(bytes(40), self.definition)
        rule = {"at": 0, "end": 40, "stride_bytes": 8, "empty_hex": "00"}
        for changes in ({"stride_bytes": 0}, {"probe_offset": -1}, {"empty_hex": ""},
                        {"probe_offset": 8}, {"end": 41}, {"at": 8, "end": 0}):
            with self.subTest(changes=changes), self.assertRaises(inspector.DefinitionError):
                reader.expression({"span_count": dict(rule, **changes)})
        limited = inspector.DefinitionReader(bytes(40), self.definition, max_records=4)
        with self.assertRaisesRegex(inspector.DefinitionError, "Record limit"):
            limited.expression({"span_count": rule})

    def test_scanned_count_and_generic_version_are_definition_driven(self):
        definition = copy.deepcopy(self.definition)
        recognition = definition["recognition"]
        recognition["version_field"] = recognition.pop("cps_version_field")
        recognition["supported_versions"] = recognition.pop("supported_cps_versions")
        definition["fields"]["count"] = {"value": {"span_count": {
            "at": {"field": "pointer"}, "end": 80, "stride_bytes": 8,
            "probe_offset": 5, "empty_hex": "00"}}}
        result = self.inspect(definition=definition)[0]
        self.assertEqual(result["status"], "recognized")
        self.assertEqual(result["decoded"]["fields"]["count"], 2)
        check = next(c for c in result["checks"] if c["name"] == "version")
        self.assertEqual(check["status"], "match")
        self.assertFalse(any(c["name"] == "CPS version" for c in result["checks"]))

    def test_scan_excludes_both_zero_and_erased_slots_without_discarding_gaps(self):
        data = bytes([1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 255, 255, 255, 255])
        reader = inspector.DefinitionReader(data, self.definition)
        rule = {"at": 0, "end": 16, "stride_bytes": 4, "empty_hex_patterns": ["00", "FF"]}
        self.assertEqual(reader.expression({"span_count": rule}), 3)
        for patterns in ([], [""], ["00", "FF FF"]):
            with self.subTest(patterns=patterns), self.assertRaises(inspector.DefinitionError):
                reader.expression({"span_count": dict(rule, empty_hex_patterns=patterns)})
        with self.assertRaises(inspector.DefinitionError):
            reader.expression({"span_count": dict(rule, empty_hex="00")})

    def test_bcd_byte_order_scaling_and_invalid_digits(self):
        reader = inspector.DefinitionReader(bytes.fromhex("12 34 56 78"), self.definition)
        big = reader.read_scalar({"type": "bcdbe", "bytes": 4, "scale": 10}, 0, "value")
        little = reader.read_scalar({"type": "bcdle", "bytes": 4}, 0, "value")
        self.assertEqual(big, {"raw": 12345678, "value": 123456780})
        self.assertEqual(little["value"], 78563412)
        for storage in (b"\xfa", b"\x1a", b"\xff"):
            with self.subTest(storage=storage), self.assertRaisesRegex(inspector.DefinitionError, "BCD"):
                inspector.decode_bcd(storage, "big")
        reader.read_scalar({"type": "bcdbe", "bytes": 4, "maximum": 100}, 0, "value")
        self.assertTrue(reader.errors)

    def test_configurable_text_terminators_preserve_old_default(self):
        reader = inspector.DefinitionReader(b"Name\xff\xfe\xff\xff", self.definition)
        field = {"type": "ascii", "bytes": 8, "terminators": ["0x00", "0xFF"], "require_terminator": True}
        self.assertEqual(reader.read_scalar(field, 0, "name")["value"], "Name")
        with self.assertRaises(inspector.DefinitionError):
            reader.read_scalar(dict(field, terminators=[256]), 0, "name")
        with self.assertRaises(UnicodeError):
            reader.read_scalar({"type": "ascii", "bytes": 8}, 0, "name")

    def test_segment_bitmaps_keep_physical_indices_and_reject_disabled_references(self):
        data = bytearray(160)
        data[:5] = b"TEST\0"
        data[16], data[17] = 1, 2
        data[32:34], data[64:66] = bytes([10, 20]), bytes([30, 40])
        data[80:82] = bytes([1, 4])
        data[112:116], data[116:120], data[120:124] = b"\0\0\0\0", b"\xff\xff\xff\xff", b"A\0\0\0"
        definition = {
            "definition_type": "codeplug", "definition_version": 1,
            "representations": [{"name": "plain", "decode": {"operation": "identity"}}],
            "recognition": {"model_field": "marker", "model_value": "TEST", "required_fields": {"marker": "TEST"}},
            "fields": {"marker": {"at": 0, "type": "ascii", "bytes": 5}},
            "tables": {
                "targets": {"count": 4, "segments": [
                    {"at": 32, "count": 2, "stride_bytes": 1, "validity": {"bitmap_at": 16}},
                    {"at": 64, "count": 2, "stride_bytes": 1, "validity": {"bitmap_at": 17}},
                ], "fields": {"value": {"offset": 0, "type": "u8"}}},
                "references": {"at": 80, "count": 2, "stride_bytes": 1, "fields": {
                    "target": {"offset": 0, "type": "u8", "reference": {"table": "targets", "index_base": 1}}}},
                "named": {"at": 112, "count": 3, "stride_bytes": 4,
                          "validity": {"bytes": 1, "not_hex": ["00", "FF"]},
                          "fields": {"name": {"offset": 0, "type": "ascii", "bytes": 4}}},
            },
        }
        result = self.inspect(bytes(data), definition)[0]
        self.assertEqual(result["status"], "recognized")
        targets = result["decoded"]["tables"]["targets"]
        self.assertEqual(targets["selected_indices"], [0, 3])
        self.assertEqual([r["fields"]["value"] for r in targets["records"]], [10, 40])
        self.assertEqual(result["decoded"]["tables"]["named"]["selected_indices"], [2])
        self.assertEqual(len(targets["records"]), 2)
        self.assertEqual(targets["count"], 4)
        data[81] = 2
        result = self.inspect(bytes(data), definition)[0]
        self.assertEqual(result["status"], "partial")
        check = next(c for c in result["checks"] if c["name"] == "references.target")
        self.assertEqual(check["invalid"][0]["reason"], "Target slot is not selected")
        definition["tables"]["targets"]["count"] = 5
        result = self.inspect(bytes(data), definition)[0]
        self.assertTrue(any("Segment counts" in e["message"] for e in result["errors"]))

    def test_bitmap_order_bounds_and_required_format_marker(self):
        reader = inspector.DefinitionReader(bytes([128, 1, 2]), self.definition)
        self.assertTrue(reader.selected({"bitmap_at": 0, "bit_order": "msb_first"}, 0, 1))
        self.assertFalse(reader.selected({"bitmap_at": 0, "bit_order": "lsb_first"}, 0, 1))
        with self.assertRaises(inspector.DefinitionError):
            reader.selected({"bitmap_at": 3}, 0, 1)
        with self.assertRaises(inspector.DefinitionError):
            reader.selected({"bitmap_at": 0, "bit_order": "invalid"}, 0, 1)
        definition = copy.deepcopy(self.definition)
        definition["recognition"]["required_fields"] = {"marker": "DIFFERENT"}
        result = self.inspect(definition=definition)[0]
        self.assertEqual(result["status"], "partial")
        self.assertTrue(any(c["name"] == "marker" and c["status"] == "mismatch" for c in result["checks"]))

    def test_blank_erased_and_out_of_range_equalities_are_not_partial_matches(self):
        for pointer, filler in ((0, 0), (0xFFFFFFFF, 255), (4096, 0)):
            with self.subTest(pointer=pointer, filler=filler):
                data, definition = blank_match_fixture(pointer, filler)
                result = self.inspect(data, definition)[0]
                self.assertEqual(result["status"], "no_match")
                matched = [c for c in result["checks"] if c["status"] == "match"]
                self.assertTrue(matched)  # Diagnostics are retained rather than discarded.
                self.assertTrue(all(not c["informative"] for c in matched))
                self.assertEqual(next(c for c in matched if c["name"] == "empty_index.number")["checked"], 0)

    def test_valid_in_range_addresses_and_stored_file_size_keep_partial_matches(self):
        data, definition = blank_match_fixture(64)
        result = self.inspect(data, definition)[0]
        self.assertEqual(result["status"], "partial")
        self.assertTrue(next(c for c in result["checks"] if c["name"] == "pointer_a")["informative"])
        data, definition = blank_match_fixture()
        data = bytearray(data)
        data[32:36] = len(data).to_bytes(4, "little")
        definition["fields"]["stored_size"] = {"at": 32, "type": "u32le"}
        definition["layout_checks"]["relationships"].append(
            {"left": {"field": "stored_size"}, "right": {"image_size": True}})
        result = self.inspect(bytes(data), definition)[0]
        self.assertEqual(result["status"], "partial")
        self.assertTrue(next(c for c in result["checks"] if c["name"] == "relationship 3")["informative"])

    def test_erased_identifier_storage_is_not_informative_and_recognition_is_unchanged(self):
        data, definition = blank_match_fixture()
        data = bytearray(data)
        data[:5], data[8:12] = b"TEST\0", b"1.0\0"
        result = self.inspect(bytes(data), definition)[0]
        self.assertEqual(result["status"], "recognized")
        self.assertTrue(next(c for c in result["checks"] if c["name"] == "model")["informative"])
        data[:5], data[8:12] = bytes([255]) * 5, bytes([255]) * 4
        definition["recognition"]["model_value"] = ""
        definition["recognition"]["supported_cps_versions"] = [""]
        # Range failures keep it unrecognized even though blank identifiers compare equal.
        definition["layout_checks"]["expected_addresses"]["pointer_a"] = 64
        result = self.inspect(bytes(data), definition)[0]
        self.assertEqual(result["status"], "no_match")
        self.assertFalse(any(c["informative"] for c in result["checks"]))

    def test_ignored_partial_checks_remain_in_json_but_not_the_match_list(self):
        data, definition = blank_match_fixture()
        results = self.inspect(data, definition)
        report = {"input": "test.bin", "bytes": len(data), "definitions": [
            {"file": "test.wcp", "id": "test", "recognition": definition["recognition"], "results": results}]}
        rendered = inspector.text_report(report)
        self.assertIn("NO MATCH", rendered)
        self.assertIn("Partial-match evidence: 0 informative checks", rendered)
        self.assertNotIn("    Matches:", rendered)
        check = next(c for c in json.loads(json.dumps(report))["definitions"][0]["results"][0]["checks"]
                     if c["name"] == "pointer_a equals pointer_b")
        self.assertEqual(check["status"], "match")
        self.assertFalse(check["informative"])


if __name__ == "__main__":
    unittest.main()
