"""Run: python3 -B -m unittest discover -s tests -p test_generate_definition.py."""
from copy import deepcopy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

from tools import inspect_codeplug as inspector

SCRIPT = Path(inspector.__file__).resolve()


def fixture():
    data = bytearray(1024)
    data[:8] = b"NEWTEST\0"
    data[8:16] = b"OWNER\0\0\0"
    data[16:19] = (1234567).to_bytes(3, "little")
    data[24:27] = bytes([2, 2, 1])
    data[28:30] = len(data).to_bytes(2, "little")
    fields = {
        "model": {"at": 0, "type": "ascii", "bytes": 8, "require_terminator": True},
        "radio_name": {"at": 8, "type": "ascii", "bytes": 8},
        "radio_id": {"at": 16, "type": "u24le", "minimum": 1},
        "channel_count": {"at": 24, "type": "u8", "maximum": 2},
        "contact_count": {"at": 25, "type": "u8", "maximum": 2},
        "zone_count": {"at": 26, "type": "u8", "maximum": 1},
        "file_size": {"at": 28, "type": "u16le"},
    }
    tables = {
        "channels": {"at": 64, "count": {"field": "channel_count"}, "stride_bytes": 24, "fields": {
            "name": {"offset": 0, "type": "ascii", "bytes": 8},
            "rx": {"offset": 8, "type": "u32le", "unit": "Hz"},
            "tx": {"offset": 12, "type": "u32le", "unit": "Hz"},
            "mode": {"offset": 16, "type": "u8", "enum": {"0": "analog", "1": "digital"}},
            "contact": {"offset": 18, "type": "u16le", "reference": {"table": "contacts", "index_base": 1}}}},
        "contacts": {"at": 192, "count": {"field": "contact_count"}, "stride_bytes": 16, "fields": {
            "id": {"offset": 0, "type": "u24le", "minimum": 1},
            "name": {"offset": 3, "type": "ascii", "bytes": 8},
            "kind": {"offset": 11, "type": "u8", "enum": {"0": "private", "1": "group"}}}},
        "zones": {"at": 320, "count": {"field": "zone_count"}, "stride_bytes": 24, "fields": {
            "name": {"offset": 0, "type": "ascii", "bytes": 8},
            "member_count": {"offset": 8, "type": "u8", "maximum": 2},
            "members": {"offset": 10, "type": "u16le", "count": {"record_field": "member_count"}, "stride_bytes": 2,
                        "reference": {"table": "channels", "index_base": 1}}}},
    }
    profile = {"version": 1, "tables": [
        {"table": "channels", "count_field": "channel_count", "records": [
            {"copy": {"match": {"mode": 1}}, "fields": {"name": "DCH 1", "rx": 400000000, "tx": 400000000, "mode": 1, "contact": 1}}]},
        {"table": "zones", "count_field": "zone_count", "records": [
            {"fields": {"name": "Zone 1", "member_count": 1, "members": [1]}}]},
        {"table": "contacts", "count_field": "contact_count", "records": [
            {"fields": {"id": 9, "name": "Local", "kind": 1}}]},
    ], "globals": {"radio_name": "NOCALL", "radio_id": 1}}
    whole = {"id": "old-format", "definition_type": "codeplug", "definition_version": 1,
             "representations": [{"name": "plain", "extension": ".rdb", "decode": {"operation": "identity"}}],
             "recognition": {"model_field": "model", "model_value": "OLDTEST"},
             "fields": fields, "tables": tables,
             "layout_checks": {"relationships": [{"left": {"image_size": True}, "right": {"field": "file_size"}}]},
             "default_generation": profile}
    for i in range(2):
        at = 64 + 24 * i
        data[at:at + 8] = f"Ch {i + 1}".encode().ljust(8, b"\0")
        data[at + 8:at + 12] = (144000000 + i * 10000).to_bytes(4, "little")
        data[at + 12:at + 16] = (144000000 + i * 10000).to_bytes(4, "little")
        data[at + 16] = 1
        data[at + 18:at + 20] = (i + 1).to_bytes(2, "little")
        at = 192 + 16 * i
        data[at:at + 3] = (9 if i == 0 else 1234567).to_bytes(3, "little")
        data[at + 3:at + 11] = (b"Local" if i == 0 else b"Person").ljust(8, b"\0")
        data[at + 11] = 1 if i == 0 else 0
    data[320:328] = b"My Zone\0"
    data[328] = 2
    data[330:334] = bytes([1, 0, 2, 0])
    data[-8:] = b"opaque!!"
    donors = []
    for role in ("identity", "channels", "zones", "contacts"):
        donor = deepcopy(whole)
        donor["id"] = role + "-donor"
        donor["tables"] = {} if role == "identity" else {role: deepcopy(tables[role])}
        donor["default_generation"]["tables"] = [p for p in donor["default_generation"]["tables"] if p["table"] == role]
        if role != "identity":
            for name in ("radio_name", "radio_id"):
                donor["fields"].pop(name)
                donor["default_generation"]["globals"].pop(name)
        # Keep other field descriptors: some components can share pointer/count guards.
        donors.append(donor)
    return bytes(data), donors, whole


class DefinitionInferenceTests(unittest.TestCase):
    def test_unmapped_enum_warns_but_cli_still_generates(self):
        root = SCRIPT.parent.parent
        donor_path = root / "definitions" / "cs750.wcp"
        cs800_sample = root / "Codeplugs" / "CS800-KB1JDX 2017-01-22.rdb"
        with tempfile.TemporaryDirectory() as directory:
            definitions = Path(directory)
            (definitions / "cs750.wcp").write_bytes(donor_path.read_bytes())
            result = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), str(cs800_sample),
                 "--definitions", str(definitions), "--generate-definition"],
                input="cs800\n", capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((definitions / "cs800.wcp").exists())
            self.assertIn("Definition is incomplete:", result.stderr)
            self.assertIn("contacts.call_type has undeclared value 1 in 37 records.", result.stderr)
            self.assertIn("The definition will still be generated.", result.stderr)

    def test_mapped_enums_do_not_warn(self):
        path = SCRIPT.parent.parent / "definitions" / "cs750.wcp"
        donor = inspector.load_definition(path)
        with zipfile.ZipFile(path) as archive:
            source = archive.read(donor["creation"]["template"])
        self.assertEqual(inspector.undeclared_enum_values(source, donor), [])

    def test_complete_generation_preserves_writer_and_creation_metadata(self):
        path = SCRIPT.parent.parent / "definitions" / "cs750.wcp"
        donor = inspector.load_definition(path)
        with zipfile.ZipFile(path) as archive:
            source = archive.read(donor["creation"]["template"])
        draft = inspector.generation_definition(source, [donor])
        default = inspector.create_default_codeplug(source, draft)
        with tempfile.TemporaryDirectory() as directory:
            package = inspector.save_definition_package(directory, "Complete test", draft, default)
            loaded = inspector.load_definition(package)
        for key in ("construction", "channel_defaults", "base_image"):
            self.assertEqual(loaded["creation"][key], donor["creation"][key])
        self.assertEqual(loaded["fields"], donor["fields"])
        self.assertEqual(loaded["tables"], donor["tables"])
        self.assertEqual(loaded["preservation"], donor["preservation"])
        reset_only = deepcopy(donor)
        reset_only["creation"]["construction"] = {"default_generation": "reset only"}
        invalid = deepcopy(donor)
        invalid["creation"]["construction"]["contacts"]["operation"] = "invented_operation"
        different = deepcopy(donor)
        different["recognition"]["model_value"] = "DIFFERENT RADIO"
        for rejected in (reset_only, invalid, different):
            with self.assertRaisesRegex(inspector.DefinitionGenerationError, "No matching definition supplies executable"):
                inspector.generation_definition(source, [rejected])

    def test_cli_refuses_reset_only_without_prompt_or_output(self):
        source, donors, _ = fixture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path = root / "input.rdb"
            input_path.write_bytes(source)
            for i, donor in enumerate(donors):
                (root / f"donor{i}.wcp").write_text(json.dumps(donor))
            before = {p: p.read_bytes() for p in root.iterdir()}
            result = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), str(input_path),
                 "--definitions", str(root), "--generate-definition"],
                input="unused\n", capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("No matching definition supplies executable", result.stderr)
            self.assertNotIn("Definition name [", result.stderr)
            self.assertEqual(before, {p: p.read_bytes() for p in root.iterdir()})

    def test_identity_metadata_retained_only_for_matching_model_and_version(self):
        for model, version, retained in (("NEWTEST", "V2", True),
                                         ("OLDTEST", "V2", False),
                                         ("NEWTEST", "V1", False)):
            with self.subTest(model=model, version=version):
                source, donors, _ = fixture()
                data = bytearray(source)
                data[40:48] = b"HW-TEST\0"
                data[48:56] = b"V2\0\0\0\0\0\0"
                identity = donors[0]
                identity["recognition"].update(model_value=model, version_field="version",
                                                supported_versions=[version])
                identity["fields"]["version"] = {"at": 48, "type": "ascii", "bytes": 8}
                identity["fields"]["hardware_marker"] = {"at": 40, "type": "ascii", "bytes": 8}
                identity["preservation"] = {"scan_scope": "Verified research note"}
                draft = inspector.infer_definition(bytes(data), donors)
                self.assertEqual("identity__hardware_marker" in draft["fields"], retained)
                self.assertEqual("scan_scope" in draft["preservation"], retained)
                if retained:
                    reader = inspector.DefinitionReader(bytes(data), draft)
                    self.assertEqual(reader.globals.get("identity__hardware_marker")["value"], "HW-TEST")
                    self.assertEqual(draft["preservation"]["scan_scope"], "Verified research note")
                    regenerated = inspector.infer_definition(bytes(data), [draft])
                    self.assertIn("identity__hardware_marker", regenerated["fields"])
                    self.assertEqual(regenerated["preservation"], draft["preservation"])

    def test_matching_model_equality_check_is_retained(self):
        source, donors, _ = fixture()
        data = bytearray(source)
        data[40:48] = b"NEWTEST\0"
        identity = donors[0]
        identity["recognition"]["model_value"] = "NEWTEST"
        identity["fields"]["secondary_model"] = {"at": 40, "type": "ascii", "bytes": 8}
        identity["layout_checks"]["equal_fields"] = [["model", "secondary_model"]]
        draft = inspector.infer_definition(bytes(data), donors)
        self.assertIn(["identity__model", "identity__secondary_model"],
                      draft["layout_checks"]["equal_fields"])

    def test_power_on_messages_are_carried_and_cleared_in_packaged_default(self):
        for messages in (("Dah dit dah dit", "dah dah dit dah"), ("", ""),
                         ("ABCDEFGHIJKLMNOP", "Different text")):
            with self.subTest(messages=messages):
                source, donors, _ = fixture()
                data = bytearray(source)
                for n, message in enumerate(messages, 1):
                    at = 352 + (n - 1) * 16
                    donors[0]["fields"][f"power_on_message_{n}"] = {
                        "at": at, "type": "ascii", "bytes": 16,
                        "terminators": ["0x00", "0xFF"]}
                    data[at:at + 16] = message.encode().ljust(16, b"\xff")
                original = bytes(data)
                draft = inspector.infer_definition(original, donors)
                default = inspector.create_default_codeplug(original, draft)
                # The new resets change only the two message regions relative
                # to the existing default-generation behavior.
                baseline = inspector.create_default_codeplug(
                    original, inspector.infer_definition(original, fixture()[1]))
                self.assertEqual(default[:352], baseline[:352])
                self.assertEqual(default[384:], baseline[384:])
                self.assertEqual(default[352:384], b"\0" * 32)
                self.assertEqual(bytes(data), original)
                for n in (1, 2):
                    name = f"power_on_message_{n}"
                    self.assertIn(name, draft["fields"])
                    self.assertEqual(draft["default_generation"]["globals"][name], "")
                    self.assertNotIn(name, draft["recognition"].get("required_fields", {}))
                with tempfile.TemporaryDirectory() as directory:
                    path = inspector.save_definition_package(directory, "Message test", draft, default)
                    loaded = inspector.load_definition(path)
                    with zipfile.ZipFile(path) as archive:
                        packaged = archive.read(loaded["creation"]["template"])
                    reader = inspector.DefinitionReader(packaged, loaded)
                    for n in (1, 2):
                        self.assertEqual(reader.globals.get(f"power_on_message_{n}")["value"], "")
                    regenerated = inspector.infer_definition(original, [loaded])
                    for n in (1, 2):
                        self.assertIn(f"power_on_message_{n}", regenerated["fields"])

    def test_generation_preserves_transmit_and_receive_frequency_names(self):
        source, donors, _ = fixture()
        names = {"tx": "transmit_frequency", "rx": "receive_frequency"}
        for donor in donors:
            if "channels" in donor["tables"]:
                fields = donor["tables"]["channels"]["fields"]
                donor["tables"]["channels"]["fields"] = {
                    names.get(key, key): value for key, value in fields.items()}
            for plan in donor["default_generation"]["tables"]:
                if plan["table"] == "channels":
                    for record in plan["records"]:
                        record["fields"] = {names.get(key, key): value
                                            for key, value in record["fields"].items()}
        draft = inspector.infer_definition(source, donors)
        fields = draft["tables"]["channels"]["fields"]
        self.assertEqual(fields["transmit_frequency"]["offset"], 12)
        self.assertEqual(fields["receive_frequency"]["offset"], 8)
        plan = next(p for p in draft["default_generation"]["tables"] if p["table"] == "channels")
        self.assertEqual(plan["records"][0]["fields"]["transmit_frequency"], 400000000)
        self.assertEqual(plan["records"][0]["fields"]["receive_frequency"], 400000000)
        self.assertNotIn("tx", fields)
        self.assertNotIn("rx", fields)

    def test_different_secondary_model_and_version_allow_generation(self):
        source, donors, _ = fixture()
        data = bytearray(source)
        data[40:48] = b"OLDTEST\0"
        data[48:56] = b"V2\0\0\0\0\0\0"
        identity = donors[0]
        identity["fields"]["secondary_model"] = {"at": 40, "type": "ascii", "bytes": 8}
        identity["fields"]["header_version"] = {"at": 48, "type": "ascii", "bytes": 8}
        identity["recognition"].update(version_field="header_version", supported_versions=["V1"])
        identity["layout_checks"]["equal_fields"] = [["model", "secondary_model"]]
        draft = inspector.infer_definition(bytes(data), donors)
        self.assertEqual(draft["recognition"]["supported_versions"], ["V2"])
        checked = inspector.inspect_definition(bytes(data), draft)[0]
        self.assertEqual(checked["status"], "recognized")
        values = checked["decoded"]["fields"]
        self.assertEqual(values["identity__model"], "NEWTEST")
        self.assertEqual(values["identity__secondary_model"], "OLDTEST")
        self.assertNotIn(["identity__model", "identity__secondary_model"],
                         draft["layout_checks"].get("equal_fields", []))
        default = inspector.create_default_codeplug(bytes(data), draft)
        self.assertEqual(default[40:56], data[40:56])

    def test_generated_definition_remains_a_reusable_donor(self):
        source, donors, _ = fixture()
        draft = inspector.infer_definition(source, donors)
        regenerated = inspector.infer_definition(source, donors + [draft])
        self.assertEqual(regenerated["fields"], draft["fields"])
        self.assertEqual(regenerated["tables"], draft["tables"])

    def test_combines_partial_definitions_and_binds_observed_format_not_owner(self):
        source, donors, _ = fixture()
        self.assertTrue(all(inspector.inspect_definition(source, d)[0]["status"] != "recognized" for d in donors))
        before = deepcopy(donors)
        draft = inspector.infer_definition(source, donors)
        self.assertEqual(donors, before)
        self.assertEqual(draft["recognition"]["model_value"], "NEWTEST")
        self.assertNotIn("radio_name", draft["recognition"].get("required_fields", {}))
        checked = inspector.inspect_definition(source, draft)[0]
        self.assertEqual(checked["status"], "recognized", checked.get("errors"))
        self.assertTrue({"channels", "zones", "contacts"} <= draft["tables"].keys())
        default = inspector.create_default_codeplug(source, draft)
        values = inspector.inspect_definition(default, draft)[0]["decoded"]["fields"]
        self.assertEqual((values["radio_name"], values["radio_id"]), ("NOCALL", 1))
        self.assertEqual(default[-8:], b"opaque!!")
        self.assertEqual(source, fixture()[0])

    def test_missing_roles_and_blank_regions_are_not_enough(self):
        source, donors, _ = fixture()
        for omitted in ("identity", "channels", "zones", "contacts"):
            with self.subTest(omitted=omitted), self.assertRaisesRegex(inspector.DefinitionGenerationError, omitted):
                inspector.infer_definition(source, [d for d in donors if d["id"] != omitted + "-donor"])
        for source in (bytes(1024), bytes([255]) * 1024):
            with self.assertRaisesRegex(inspector.DefinitionGenerationError, "Insufficient"):
                inspector.infer_definition(source, donors)

    def test_broken_cross_component_reference_and_bad_default_are_refused(self):
        source, donors, _ = fixture()
        bad = bytearray(source)
        bad[330:332] = (100).to_bytes(2, "little")
        with self.assertRaisesRegex(inspector.DefinitionGenerationError, "consistent definition"):
            inspector.infer_definition(bytes(bad), donors)
        bad = deepcopy(donors)
        bad[1]["default_generation"]["tables"][0]["records"][0]["fields"]["contact"] = 100
        with self.assertRaisesRegex(inspector.DefinitionGenerationError, "Generated default failed"):
            inspector.infer_definition(source, bad)

    def test_equal_identity_matches_are_selected_independently_of_donor_order(self):
        source, donors, _ = fixture()
        data = bytearray(source)
        data[40:48] = b"OTHER\0\0\0"
        alternate = deepcopy(donors[0])
        alternate["fields"]["radio_name"]["at"] = 40
        draft = inspector.infer_definition(bytes(data), donors + [alternate])
        self.assertEqual(draft, inspector.infer_definition(bytes(data), [alternate] + donors[::-1]))
        self.assertEqual(inspector.inspect_definition(bytes(data), draft)[0]["status"], "recognized")

    def test_more_field_mappings_win_only_after_complete_validation(self):
        source, donors, _ = fixture()
        richer = deepcopy(donors[1])
        richer["fields"]["extra_setting"] = {"at": 400, "type": "u8"}
        richer["default_generation"]["globals"]["extra_setting"] = 0
        draft = inspector.infer_definition(source, donors + [richer])
        self.assertEqual(draft, inspector.infer_definition(source, [richer] + donors[::-1]))
        self.assertTrue(any(name.endswith("extra_setting") for name in draft["fields"]))
        invalid = deepcopy(richer)
        invalid["fields"]["extra_setting"]["maximum"] = 0
        invalid["default_generation"]["globals"]["extra_setting"] = 1
        fallback = inspector.infer_definition(source, donors + [invalid])
        self.assertFalse(any(name.endswith("extra_setting") for name in fallback["fields"]))

    def test_more_required_checks_break_equal_field_count_ties(self):
        source, donors, _ = fixture()
        stricter = deepcopy(donors[1])
        rule = {"left": {"field": "channel_count"}, "right": {"field": "channel_count"}}
        stricter["layout_checks"]["relationships"].append(rule)
        draft = inspector.infer_definition(source, donors + [stricter])
        mapped_rule = {"left": {"field": "channels__channel_count"},
                       "right": {"field": "channels__channel_count"}}
        self.assertIn(mapped_rule, draft["layout_checks"]["relationships"])
        self.assertEqual(draft, inspector.infer_definition(source, [stricter] + donors[::-1]))

    def test_equivalent_donor_descriptions_are_deduplicated(self):
        source, donors, _ = fixture()
        duplicates = deepcopy(donors)
        for donor in duplicates:
            donor["id"] += "-copy"
            for table in donor["tables"].values():
                table["meaning"] = "Different explanation of the same mapping."
        draft = inspector.infer_definition(source, donors + duplicates)
        self.assertEqual(inspector.inspect_definition(source, draft)[0]["status"], "recognized")

    def test_output_package_round_trip_and_existing_file_protection(self):
        source, donors, _ = fixture()
        draft = inspector.infer_definition(source, donors)
        default = inspector.create_default_codeplug(source, draft)
        with tempfile.TemporaryDirectory() as directory:
            path = inspector.save_definition_package(directory, "New Radio", draft, default)
            self.assertEqual(path.name, "new-radio.wcp")
            loaded = inspector.load_definition(path)
            self.assertEqual(loaded["id"], "new-radio")
            with zipfile.ZipFile(path) as archive:
                template = archive.read(loaded["creation"]["template"])
                self.assertEqual(template, default)
            self.assertEqual(inspector.inspect_definition(source, loaded)[0]["status"], "recognized")
            before = path.read_bytes()
            with self.assertRaisesRegex(inspector.DefinitionGenerationError, "already exists"):
                inspector.save_definition_package(directory, "New Radio", draft, default)
            self.assertEqual(path.read_bytes(), before)
            for name in ("../escape", "a/b", "a\\b", "!!!", ""):
                with self.subTest(name=name), self.assertRaises(inspector.DefinitionGenerationError):
                    inspector.save_definition_package(directory, name, draft, default)

    def test_name_prompt_accepts_default_and_reprompts_for_collision(self):
        source, donors, _ = fixture()
        draft = inspector.infer_definition(source, donors)
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(sys, "stdin", io.StringIO("\n")), mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
                self.assertEqual(inspector.prompt_definition_name(draft, directory), "NEWTEST")
                self.assertIn("[NEWTEST]", stderr.getvalue())
            (Path(directory) / "newtest.wcp").write_bytes(b"keep")
            with mock.patch.object(sys, "stdin", io.StringIO("\nChosen\n")), mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
                self.assertEqual(inspector.prompt_definition_name(draft, directory), "Chosen")
                self.assertIn("already exists", stderr.getvalue())
            with mock.patch.object(sys, "stdin", io.StringIO("")), mock.patch.object(sys, "stderr", io.StringIO()):
                with self.assertRaisesRegex(inspector.DefinitionGenerationError, "closed"):
                    inspector.prompt_definition_name(draft, directory)

    def test_cli_generation_prompts_without_output_argument_and_keeps_inputs(self):
        source, donors, _ = fixture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path = root / "input.rdb"
            input_path.write_bytes(source)
            definitions = root / "defs"
            definitions.mkdir()
            for i, donor in enumerate(donors):
                (definitions / f"donor{i}.wcp").write_text(json.dumps(donor))
            before = {p: p.read_bytes() for p in root.rglob("*") if p.is_file()}
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(input_path), "--definitions", str(definitions),
                                     "--generate-definition", "--default-only", "--json"], input="\n", capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Definition name [NEWTEST]", result.stderr)
            report = json.loads(result.stdout)
            path = definitions / "newtest.wcp"
            self.assertEqual(Path(report["generated_definition"]), path)
            self.assertTrue(path.exists())
            self.assertEqual(before, {p: p.read_bytes() for p in before})
            self.assertEqual({p for p in root.rglob("*") if p.is_file()}, set(before) | {path})

    def test_cli_generation_uses_input_extension(self):
        for donor_extension in (None, ".other"):
            with self.subTest(donor_extension=donor_extension), tempfile.TemporaryDirectory() as directory:
                source, donors, _ = fixture()
                root = Path(directory)
                input_path = root / "input.rdb"
                input_path.write_bytes(source)
                definitions = root / "defs"
                definitions.mkdir()
                for i, donor in enumerate(donors):
                    representation = donor["representations"][0]
                    if donor_extension is None:
                        representation.pop("extension")
                    else:
                        representation["extension"] = donor_extension
                    (definitions / f"donor{i}.wcp").write_text(json.dumps(donor))
                result = subprocess.run(
                    [sys.executable, "-B", str(SCRIPT), str(input_path),
                     "--definitions", str(definitions), "--generate-definition", "--default-only"],
                    input="\n", capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                with zipfile.ZipFile(definitions / "newtest.wcp") as archive:
                    definition = json.loads(archive.read("definition.json"))
                    self.assertEqual(definition["representations"][0]["extension"], ".rdb")
                    self.assertEqual(definition["creation"]["template"], "templates/default.rdb")
                    self.assertEqual(archive.read("templates/default.rdb"),
                                     inspector.create_default_codeplug(source, definition))

    def test_cli_generation_rejects_missing_or_invalid_input_extension(self):
        for filename in ("input", "input.bad-ext"):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as directory:
                source, donors, _ = fixture()
                root = Path(directory)
                input_path = root / filename
                input_path.write_bytes(source)
                definitions = root / "defs"
                definitions.mkdir()
                for i, donor in enumerate(donors):
                    (definitions / f"donor{i}.wcp").write_text(json.dumps(donor))
                before = {p: p.read_bytes() for p in root.rglob("*") if p.is_file()}
                result = subprocess.run(
                    [sys.executable, "-B", str(SCRIPT), str(input_path),
                     "--definitions", str(definitions), "--generate-definition", "--default-only"],
                    input="\n", capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("Input filename must have a valid codeplug extension", result.stderr)
                self.assertNotIn("Definition name [", result.stderr)
                self.assertEqual(before, {p: p.read_bytes() for p in root.rglob("*") if p.is_file()})

    def test_cli_insufficient_data_does_not_prompt_or_write(self):
        source, donors, _ = fixture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            definitions = root / "defs"
            definitions.mkdir()
            for i, donor in enumerate(donors[:-1]):
                (definitions / f"donor{i}.wcp").write_text(json.dumps(donor))
            input_path = root / "input.rdb"
            input_path.write_bytes(source)
            before = {p: p.read_bytes() for p in root.rglob("*") if p.is_file()}
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(input_path), "--definitions", str(definitions),
                                     "--generate-definition", "--default-only"], input="should-not-be-used\n", capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("Insufficient matching data", result.stderr)
            self.assertIn("contacts", result.stderr)
            self.assertNotIn("Definition name [", result.stderr)
            self.assertEqual(before, {p: p.read_bytes() for p in root.rglob("*") if p.is_file()})


    def test_owner_values_are_not_bound_as_format_requirements(self):
        source, donors, _ = fixture()
        draft = inspector.infer_definition(source, donors)
        changed = bytearray(source)
        changed[8:16] = b"ANOTHER\0"
        changed[16:19] = (7654321).to_bytes(3, "little")
        self.assertEqual(inspector.inspect_definition(bytes(changed), draft)[0]["status"], "recognized")

    def test_reset_rules_are_required_for_each_component(self):
        source, donors, _ = fixture()
        donors[-1].pop("default_generation")
        with self.assertRaisesRegex(inspector.DefinitionGenerationError, "contacts"):
            inspector.infer_definition(source, donors)

    def test_components_in_different_decoded_images_cannot_be_combined(self):
        source, donors, _ = fixture()
        data = bytearray(source)
        data[900] = 0x55
        data[25] ^= 0x55
        data[192:224] = bytes(value ^ 0x55 for value in source[192:224])
        donors[-1]["representations"] = [{"name": "xor", "decode": {
            "operation": "repeating_xor", "mask_input_offset": 900, "mask_bytes": 1}}]
        with self.assertRaisesRegex(inspector.DefinitionGenerationError, "incompatible decoded"):
            inspector.infer_definition(bytes(data), donors)

    def test_conflicting_core_storage_is_rejected_before_reset(self):
        source, donors, _ = fixture()
        contacts = donors[-1]["tables"]["contacts"]
        contacts["at"] = 64
        contacts["stride_bytes"] = 24
        contacts["fields"]["name"]["offset"] = 0
        contacts["fields"]["kind"]["offset"] = 16
        with self.assertRaisesRegex(inspector.DefinitionGenerationError, "Conflicting storage"):
            inspector.infer_definition(source, donors)

    def test_more_descriptive_required_format_marker_supplies_prompt_default(self):
        source, donors, _ = fixture()
        draft = inspector.infer_definition(source, donors)
        draft["recognition"]["model_value"] = "GD-77"
        draft["recognition"]["required_fields"] = {"format": "OpenGD77"}
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(sys, "stdin", io.StringIO("\n")), mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
                self.assertEqual(inspector.prompt_definition_name(draft, directory), "OpenGD77")
                self.assertIn("[OpenGD77]", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
