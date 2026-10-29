#!/usr/bin/env python3
"""Compare a codeplug with .wcp definitions and decode their known fields.

Examples:
    python3 tools/inspect_codeplug.py Codeplugs/example.rdbx
    python3 tools/inspect_codeplug.py image.bin --definitions another/directory --json
    python3 tools/inspect_codeplug.py Codeplugs/example.rdb --generate-definition

Reads plain JSON version 1 and ZIP-packaged version 2 .wcp definitions.
Inspection is read-only. --generate-definition combines verified mappings,
prompts for a name, and writes a new .wcp package in the definitions directory.
create_default_codeplug(source, definition) returns derived default bytes.
No USB access, .wrd definitions, or Waffle application changes are involved.
Blank/erased comparisons and empty tables do not establish a partial match.
JSON check results retain those comparisons with informative set to false.
Exit codes: 0 = recognized, 1 = partial/no match, 2 = input/definition error.
"""

import argparse
from copy import deepcopy
import hashlib
import io
import itertools
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
import zipfile
import zlib


class DefinitionError(ValueError):
    """A definition cannot interpret a value or byte range."""


def integer(value):
    if isinstance(value, bool):
        raise DefinitionError("Expected an integer, not a boolean")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.startswith("0x"):
        try:
            return int(value, 16)
        except ValueError:
            pass
    raise DefinitionError(f"Invalid integer: {value!r}")


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise DefinitionError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def byte_range(image, start, length):
    start, length = integer(start), integer(length)
    if start < 0 or length < 0 or start + length > len(image):
        raise DefinitionError(
            f"Byte range [{start}, {start + length}) is outside {len(image)} input bytes"
        )
    return image[start:start + length]


def integer_type(name):
    match = re.fullmatch(r"u(8|16|24|32|64)(le|be)?", name)
    if not match:
        return None
    width = int(match[1]) // 8
    if width > 1 and not match[2]:
        raise DefinitionError(f"Missing byte order for {name}")
    return width, "big" if match[2] == "be" else "little"


def decode_bcd(storage, byte_order):
    ordered = storage[::-1] if byte_order == "little" else storage
    value = 0
    for byte in ordered:
        for digit in (byte >> 4, byte & 15):
            if digit > 9:
                raise DefinitionError("Invalid BCD digit")
            value = value * 10 + digit
    return value


class Scope:
    def __init__(self, reader, fields, path, base=None, index=None):
        self.reader, self.fields, self.path, self.base = reader, fields, path, base
        self.index = index
        self.cache, self.failures, self.active = {}, {}, set()

    def get(self, name):
        if name in self.cache:
            return self.cache[name]
        if name in self.failures:
            raise DefinitionError(self.failures[name])
        if name in self.active:
            raise DefinitionError(f"Cyclic field dependency: {self.path}.{name}")
        if name not in self.fields:
            raise DefinitionError(f"Undefined field: {self.path}.{name}")
        self.active.add(name)
        path = f"{self.path}.{name}"
        try:
            result = self.reader.read_field(self.fields[name], self, path)
            self.cache[name] = result
            return result
        except (DefinitionError, UnicodeError, KeyError, TypeError, ValueError) as error:
            self.failures[name] = str(error)
            self.reader.error(path, str(error))
            raise DefinitionError(str(error)) from error
        finally:
            self.active.remove(name)

    def decoded(self):
        values, raw = {}, {}
        for name in self.fields:
            try:
                result = self.get(name)
                if result is not None:
                    values[name], raw[name] = result["value"], result["raw"]
            except DefinitionError:
                continue
        return values, raw


class DefinitionReader:
    def __init__(self, image, definition, max_records=100000):
        self.image, self.definition = image, definition
        self.max_records, self.used_records = max_records, 0
        self.errors, self._error_keys = [], set()
        self.globals = Scope(self, definition["fields"], "fields")
        self.tables = {}
        self._selection_indexes = {}

    def error(self, path, message):
        if (path, message) not in self._error_keys:
            self.errors.append({"path": path, "message": message})
            self._error_keys.add((path, message))

    def expression(self, value, scope=None):
        if not isinstance(value, dict):
            return integer(value)
        if len(value) != 1:
            raise DefinitionError(f"An expression must have exactly one operator: {value!r}")
        op, operand = next(iter(value.items()))
        if op == "image_size" and operand is True:
            return len(self.image)
        if op == "record_index" and operand is True:
            if scope is None or scope.index is None:
                raise DefinitionError("record_index used outside a table record")
            return scope.index
        if op == "span_count":
            return self.span_count(operand, scope)
        if op in ("field", "record_field"):
            owner = self.globals if op == "field" else scope
            if owner is None or (op == "record_field" and owner.base is None):
                raise DefinitionError("record_field used outside a record")
            result = owner.get(operand)
            if result is None:
                raise DefinitionError(f"Conditional field is unavailable: {operand}")
            return result["raw"]
        if op not in ("add", "subtract", "multiply", "max"):
            raise DefinitionError(f"Unsupported expression operator: {op}")
        if not isinstance(operand, list) or not operand or (op == "subtract" and len(operand) != 2):
            raise DefinitionError(f"Invalid operands for {op}")
        args = [integer(self.expression(item, scope)) for item in operand]
        return {"add": sum, "multiply": math.prod, "max": max,
                "subtract": lambda a: a[0] - a[1]}[op](args)

    def span_count(self, rule, scope):
        if not isinstance(rule, dict):
            raise DefinitionError("span_count must be an object")
        start = integer(self.expression(rule["at"], scope))
        end = integer(self.expression(rule["end"], scope))
        stride = integer(rule["stride_bytes"])
        probe = integer(rule.get("probe_offset", 0))
        if "empty_hex_patterns" in rule:
            if "empty_hex" in rule or not isinstance(rule["empty_hex_patterns"], list):
                raise DefinitionError("Specify one empty pattern or an array of empty patterns")
            empty = [bytes.fromhex(pattern) for pattern in rule["empty_hex_patterns"]]
        else:
            empty = [bytes.fromhex(rule["empty_hex"])]
        if stride <= 0 or probe < 0 or not empty or not empty[0]:
            raise DefinitionError("Invalid slot scan")
        size = len(empty[0])
        if any(len(pattern) != size for pattern in empty):
            raise DefinitionError("Empty scan patterns must have equal lengths")
        if probe + size > stride:
            raise DefinitionError("Scan probe exceeds its record")
        byte_range(self.image, start, end - start)
        self.reserve((end - start) // stride, "slot scan")
        span = 0
        for index, offset in enumerate(range(start, end - stride + 1, stride)):
            if byte_range(self.image, offset + probe, size) not in empty:
                span = index + 1
        return span

    def reserve(self, count, path):
        count = integer(count)
        if count < 0:
            raise DefinitionError(f"Negative record count at {path}")
        if count > self.max_records - self.used_records:
            raise DefinitionError(
                f"Record limit exceeded at {path}; raise --max-records if appropriate"
            )
        self.used_records += count
        return count

    def read_field(self, field, scope, path):
        if "only_if" in field:
            condition = field["only_if"]
            if self.expression(condition["value"], scope) not in condition["in"]:
                return None
        if "value" in field:
            value = self.expression(field["value"], scope)
            return {"raw": value, "value": value}
        at = (self.expression(field["at"], scope) if "at" in field
              else scope.base + integer(self.expression(field["offset"], scope)))
        if "count" in field:
            count = integer(self.expression(field["count"], scope))
            stride = integer(self.expression(field["stride_bytes"], scope))
            if stride <= 0:
                raise DefinitionError("Array stride must be positive")
            scalar = {k: v for k, v in field.items() if k not in ("count", "stride_bytes")}
            size = self.storage_size(scalar)
            byte_range(self.image, at, (count - 1) * stride + size if count else 0)
            self.reserve(count, path)
            results = [self.read_scalar(scalar, at + i * stride, f"{path}[{i}]")
                       for i in range(count)]
            return {"raw": [r["raw"] for r in results], "value": [r["value"] for r in results]}
        return self.read_scalar(field, at, path)

    @staticmethod
    def storage_size(field):
        encoding = integer_type(field["type"])
        return encoding[0] if encoding else integer(field["bytes"])

    def read_scalar(self, field, at, path):
        name = field["type"]
        encoding = integer_type(name)
        storage = byte_range(self.image, at, self.storage_size(field))
        if name in ("bcdle", "bcdbe"):
            if not storage:
                raise DefinitionError("BCD storage must not be empty")
            raw = decode_bcd(storage, "little" if name == "bcdle" else "big")
        elif encoding:
            width, order = encoding
            mask = integer(field.get("mask", (1 << (width * 8)) - 1))
            shift = integer(field.get("shift", 0))
            if not 0 <= mask < 1 << (width * 8) or not 0 <= shift < width * 8:
                raise DefinitionError("Invalid mask or shift")
            raw = (int.from_bytes(storage, order) & mask) >> shift
            for bound, compare in (("minimum", lambda a, b: a < b), ("maximum", lambda a, b: a > b)):
                if bound in field and compare(raw, integer(field[bound])):
                    self.error(path, f"Value {raw} violates {bound} {field[bound]}")
        elif name in ("ascii", "utf16le"):
            terminators = [integer(v) for v in field.get("terminators", [0])]
            if name == "ascii":
                if any(not 0 <= value <= 255 for value in terminators):
                    raise DefinitionError("ASCII terminator must be a byte")
                end = next((i for i, byte in enumerate(storage) if byte in terminators), -1)
                content = storage if end < 0 else storage[:end]
            else:
                if len(storage) % 2:
                    raise DefinitionError("UTF-16LE storage has an odd byte length")
                if any(not 0 <= value <= 65535 for value in terminators):
                    raise DefinitionError("UTF-16LE terminator must be a code unit")
                end = next((i for i in range(0, len(storage), 2)
                            if int.from_bytes(storage[i:i + 2], "little") in terminators), -1)
                content = storage if end < 0 else storage[:end]
            if field.get("require_terminator") and end < 0:
                raise DefinitionError("Required text terminator is missing")
            raw = content.decode("ascii" if name == "ascii" else "utf-16-le", errors="strict")
        elif name == "packed6":
            characters, alphabet = integer(field["characters"]), field["alphabet"]
            if characters < 0 or characters * 6 > len(storage) * 8 or len(alphabet) != 64:
                raise DefinitionError("Invalid packed6 length or alphabet")
            bits = int.from_bytes(storage, "big")
            raw = "".join(alphabet[(bits >> (len(storage) * 8 - 6 * (i + 1))) & 63]
                          for i in range(characters))
        else:
            raise DefinitionError(f"Unsupported field type: {name}")
        if name in ("bcdle", "bcdbe"):
            for bound, compare in (("minimum", lambda a, b: a < b), ("maximum", lambda a, b: a > b)):
                if bound in field and compare(raw, integer(field[bound])):
                    self.error(path, f"Value {raw} violates {bound} {field[bound]}")
        value = field.get("enum", {}).get(str(raw), raw)
        if "scale" in field:
            if not isinstance(value, int):
                raise DefinitionError("Scale requires a numeric value")
            value *= integer(field["scale"])
        if "trim" in field:
            supported = ("ascii_spaces_both_ends",
                         "Remove ASCII spaces from both ends of the decoded display string; retain all original packed bits.")
            if field["trim"] not in supported or not isinstance(value, str):
                raise DefinitionError("Unsupported text trim rule")
            value = value.strip(" ")
        return {"raw": raw, "value": value}

    def selected(self, validity, index, cursor):
        if validity is None:
            return True
        if "bitmap_at" in validity:
            at = integer(self.expression(validity["bitmap_at"]))
            byte = byte_range(self.image, at + index // 8, 1)[0]
            order = validity.get("bit_order", "lsb_first")
            if order not in ("lsb_first", "msb_first"):
                raise DefinitionError("Unknown bitmap bit order")
            shift = index % 8 if order == "lsb_first" else 7 - index % 8
            return bool(byte & (1 << shift))
        if "index_table" in validity:
            source = self.tables.get(validity["index_table"])
            if source is None or not source["complete"]:
                raise DefinitionError("Record selection index table is incomplete")
            key = (validity["index_table"], validity["field"])
            if key not in self._selection_indexes:
                numbers = [record["raw_values"][validity["field"]] for record in source["records"]]
                if len(numbers) != len(set(numbers)):
                    raise DefinitionError("Duplicate record selection index")
                self._selection_indexes[key] = set(numbers)
            number = index + integer(validity.get("index_base", 1))
            return number in self._selection_indexes[key]
        if "not_hex" in validity:
            at = cursor + integer(validity.get("offset", 0))
            size = integer(validity["bytes"])
            patterns = [bytes.fromhex(value) for value in validity["not_hex"]]
            if size <= 0 or not patterns or any(len(pattern) != size for pattern in patterns):
                raise DefinitionError("Invalid selection probe")
            return byte_range(self.image, at, size) not in patterns
        raise DefinitionError("Unsupported record selection rule")

    def table_order(self):
        tables = self.definition.get("tables", {})
        ordered, visited, active = [], set(), set()

        def visit(name):
            if name in active:
                raise DefinitionError("Cyclic table selection dependency")
            if name in visited:
                return
            if name not in tables:
                raise DefinitionError(f"Undefined selection index table: {name}")
            active.add(name)
            table = tables[name]
            for segment in table.get("segments", [table]):
                validity = segment.get("validity", {})
                if "index_table" in validity:
                    visit(validity["index_table"])
            active.remove(name)
            visited.add(name)
            ordered.append(name)

        for name in tables:
            visit(name)
        return ordered

    def decode_available_fields_and_tables(self):
        fields, raw_fields = self.globals.decoded()
        for name in self.table_order():
            table = self.definition["tables"][name]
            result = {"records": [], "complete": False}
            self.tables[name] = result
            path = f"tables.{name}"
            try:
                total = integer(self.expression(table["count"]))
                result["count"] = total
                if total < 0:
                    raise DefinitionError("Negative table count")
                segments = table.get("segments", [table])
                index_base, valid_records = 0, True
                if any("validity" in segment for segment in segments):
                    result["selected_indices"] = []
                for segment in segments:
                    count = integer(self.expression(segment["count"]))
                    cursor = integer(self.expression(segment["at"]))
                    if count < 0 or index_base + count > total:
                        raise DefinitionError("Segment count exceeds the table slot count")
                    if len(segments) == 1:
                        result["address"] = cursor
                    if "stride_bytes" in segment:
                        stride = integer(self.expression(segment["stride_bytes"]))
                        span = integer(segment.get("logical_span_bytes", stride))
                        if stride <= 0 or span <= 0:
                            raise DefinitionError("Table stride and logical span must be positive")
                        byte_range(self.image, cursor, (count - 1) * stride + span if count else 0)
                    else:
                        stride = None
                        byte_range(self.image, cursor, 0)
                    validity = segment.get("validity")
                    if validity is not None and stride is None:
                        raise DefinitionError("Selected records require a fixed stride")
                    if validity and "bitmap_at" in validity:
                        byte_range(self.image, integer(self.expression(validity["bitmap_at"])), (count + 7) // 8)
                    self.reserve(count, path)
                    for local_index in range(count):
                        index = index_base + local_index
                        if not self.selected(validity, local_index, cursor):
                            cursor += stride
                            continue
                        if "selected_indices" in result:
                            result["selected_indices"].append(index)
                        scope = Scope(self, table["fields"], f"{path}[{index}]", cursor, index)
                        errors_before = len(self.errors)
                        values, raw = scope.decoded()
                        complete = not scope.failures and len(self.errors) == errors_before
                        valid_records = valid_records and complete
                        result["records"].append({"index": index, "address": cursor,
                                                   "fields": values, "raw_values": raw, "complete": complete})
                        step = stride if stride is not None else integer(self.expression(table["record_bytes"], scope))
                        if step <= 0:
                            raise DefinitionError("Variable record length must be positive")
                        byte_range(self.image, cursor, step)
                        cursor += step
                    index_base += count
                if index_base != total:
                    raise DefinitionError("Segment counts do not equal the table slot count")
                result["complete"] = valid_records
            except (DefinitionError, KeyError, TypeError, ValueError) as error:
                self.error(path, str(error))
        return {"fields": fields, "raw_fields": raw_fields, "tables": self.tables}

    def reference_checks(self):
        checks = []
        for table_name, table in self.definition.get("tables", {}).items():
            for field_name, field in table["fields"].items():
                if "reference" not in field:
                    continue
                reference = field["reference"]
                target = self.tables.get(reference["table"])
                checked, invalid = 0, []
                source = self.tables[table_name]
                selected = set(target["selected_indices"]) if target and "selected_indices" in target else None
                if target is not None and "count" in target:
                    for record in source["records"]:
                        values = record["raw_values"].get(field_name, [])
                        values = values if isinstance(values, list) else [values]
                        for value in values:
                            if value == reference.get("null_value"):
                                continue
                            checked += 1
                            index = value - integer(reference["index_base"])
                            if not 0 <= index < target["count"]:
                                invalid.append({"record": record["index"], "value": value})
                            elif selected is not None and target["complete"] and index not in selected:
                                invalid.append({"record": record["index"], "value": value,
                                                "reason": "Target slot is not selected"})
                complete = source["complete"] and target is not None and target["complete"]
                checks.append({"category": "references", "name": f"{table_name}.{field_name}",
                               "status": "mismatch" if invalid else "match" if complete else "unreadable",
                               "informative": False,
                               "required": True, "checked": checked, "invalid": invalid,
                               **({} if complete else {"error": "Source or target table could not be fully decoded"})})
        for name, table in self.definition.get("tables", {}).items():
            if "join" not in table:
                continue
            target_name = table["join"]["table"]
            source, target = self.tables[name], self.tables.get(target_name)
            supported_rule = "Records with the same zero-based storage index describe the same channel."
            if table["join"].get("rule") != supported_rule:
                status, error = "unreadable", "Unsupported join rule"
            elif not target or not source["complete"] or not target["complete"]:
                status = "unreadable"
                error = "Source or target table could not be fully decoded"
            else:
                status = "match" if source["count"] == target["count"] else "mismatch"
                error = "Joined tables have different record counts" if status == "mismatch" else None
            checks.append({"category": "join", "name": f"{name} -> {target_name}",
                           "status": status, "required": True,
                           "informative": False,
                           "mapping": "Corresponding records retain the same storage index",
                           **({"error": error} if error else {})})
        return checks


def comparison(category, name, read, expected, required=True, membership=False):
    result = {"category": category, "name": name, "expected": expected, "required": required,
              "informative": False}
    try:
        actual = read()
        result.update(actual=actual, status="match" if (actual in expected if membership else actual == expected)
                      else "mismatch")
    except (DefinitionError, KeyError, TypeError, ValueError) as error:
        result.update(status="unreadable", error=str(error))
    return result


def nonblank_field(reader, name, address=False):
    """Whether a field supplies data beyond an empty or erased value."""
    try:
        result = reader.globals.get(name)
        if result is None:
            return False
        value = result["raw"]
        if isinstance(value, str):
            if not value.strip("\0\xff\uffff \t\r\n"):
                return False
        elif isinstance(value, int) and not isinstance(value, bool):
            if value <= 0 or (address and value >= len(reader.image)):
                return False
        else:
            return False
        field = reader.definition["fields"][name]
        if "at" in field:
            storage = byte_range(reader.image, reader.expression(field["at"]), reader.storage_size(field))
            return bool(storage) and not all(b == 0 for b in storage) and not all(b == 255 for b in storage)
        # Computed values have no storage to inspect. Reject all-ones whole-byte sentinels.
        return not (isinstance(value, int) and value.bit_length() % 8 == 0
                    and (value & (value + 1)) == 0)
    except (DefinitionError, KeyError, TypeError, ValueError):
        return False


def expression_fields(expression):
    if isinstance(expression, dict):
        if "field" in expression:
            yield expression["field"]
        else:
            for value in expression.values():
                yield from expression_fields(value)
    elif isinstance(expression, list):
        for value in expression:
            yield from expression_fields(value)


def informative_relationship(reader, rule):
    try:
        names = set(expression_fields(rule["left"])) | set(expression_fields(rule["right"]))
        # Pure constants/tautologies and comparisons fed by blank counts are not clues.
        if not names or not all(nonblank_field(reader, name) for name in names):
            return False
        values = [reader.expression(rule[side]) for side in ("left", "right")]
        return all(isinstance(v, int) and not isinstance(v, bool)
                   and 0 < v <= len(reader.image) for v in values)
    except (DefinitionError, KeyError, TypeError, ValueError):
        return False


def mark_informative(check, informative):
    check["informative"] = check["status"] == "match" and informative
    return check


def check_recognition(reader, definition):
    recognition = definition["recognition"]
    model = recognition["model_field"]
    checks = [mark_informative(comparison("recognition", "model", lambda: reader.expression({"field": model}),
                                         recognition["model_value"]), nonblank_field(reader, model))]
    if "version_field" in recognition or "cps_version_field" in recognition:
        generic_version = "version_field" in recognition
        version_field = recognition["version_field" if generic_version else "cps_version_field"]
        versions = recognition["supported_versions" if generic_version else "supported_cps_versions"]
        checks.append(mark_informative(comparison("recognition", "version" if generic_version else "CPS version",
                                                 lambda: reader.expression({"field": version_field}), versions, membership=True),
                                       nonblank_field(reader, version_field)))
    for name, expected in recognition.get("required_fields", {}).items():
        checks.append(mark_informative(comparison("recognition", name, lambda n=name: reader.expression({"field": n}), expected),
                                       nonblank_field(reader, name)))
    if "optional_decoded_marker" in recognition:
        marker = recognition["optional_decoded_marker"]
        expected = bytes.fromhex(marker["hex"])
        checks.append(mark_informative(comparison("recognition", "decoded marker",
                                                 lambda: byte_range(reader.image, integer(marker["at"]), len(expected)).hex(),
                                                 expected.hex(), marker.get("required", False)),
                                       bool(expected) and not all(b == 0 for b in expected)
                                       and not all(b == 255 for b in expected)))
    return checks


def check_layout(reader, definition):
    checks, layout = [], definition.get("layout_checks", {})
    for first, second in layout.get("equal_fields", []):
        checks.append(mark_informative(comparison("layout", f"{first} equals {second}",
                                                 lambda a=first, b=second: reader.expression({"field": a}) == reader.expression({"field": b}), True),
                                       nonblank_field(reader, first, address=True)
                                       and nonblank_field(reader, second, address=True)))
    for name, expected in layout.get("expected_addresses", {}).items():
        checks.append(mark_informative(comparison("layout", name, lambda n=name: reader.expression({"field": n}), integer(expected)),
                                       nonblank_field(reader, name, address=True)))
    for index, rule in enumerate(layout.get("relationships", [])):
        checks.append(mark_informative(comparison("layout", f"relationship {index + 1}",
                                                 lambda r=rule: reader.expression(r["left"]) == reader.expression(r["right"]), True),
                                       informative_relationship(reader, rule)))
    return checks


def decode_representation(data, representation):
    rule = representation["decode"]
    checks = []
    for index, pair in enumerate(rule.get("input_equal_ranges", [])):
        checks.append(comparison("encoding", f"equal input ranges {index + 1}",
                                 lambda p=pair: byte_range(data, integer(p["first_offset"]), integer(p["bytes"]))
                                 == byte_range(data, integer(p["second_offset"]), integer(p["bytes"])), True))
    if rule["operation"] == "identity":
        return data, checks
    if rule["operation"] == "repeating_xor":
        size = integer(rule["mask_bytes"])
        if size <= 0:
            raise DefinitionError("XOR mask must not be empty")
        mask = byte_range(data, integer(rule["mask_input_offset"]), size)
        phase = integer(rule.get("mask_phase_at_input_zero", 0))
        return bytes(value ^ mask[(index + phase) % size] for index, value in enumerate(data)), checks
    raise DefinitionError(f"Unsupported representation operation: {rule['operation']}")


MAX_DEFINITION_BYTES = 4 * 1024 * 1024
MAX_PACKAGE_BYTES = 64 * 1024 * 1024


def parse_definition(data):
    if len(data) > MAX_DEFINITION_BYTES:
        raise DefinitionError("Definition JSON exceeds the size limit")
    definition = json.loads(data.decode("utf-8"), object_pairs_hook=unique_keys)
    if not isinstance(definition, dict):
        raise DefinitionError("Definition must be a JSON object")
    validate_definition(definition)
    return definition


def load_definition(path):
    """Load JSON or an archive; validate its template without extracting files.

    Creation rules are data for a future writer. Inspection still interprets only
    the decoding fields and does not generate, edit, or program a codeplug.
    """
    path = Path(path)
    with path.open("rb") as stream:
        archive = stream.read(2) == b"PK"
    if not archive:
        with path.open("rb") as stream:
            definition = parse_definition(stream.read(MAX_DEFINITION_BYTES + 1))
        if definition["definition_version"] != 1:
            raise DefinitionError("Version 2 definitions must be packaged in a .wcp ZIP archive")
        return definition
    try:
        with zipfile.ZipFile(path) as package:
            entries = package.infolist()
            if len(entries) > 32 or sum(info.file_size for info in entries) > MAX_PACKAGE_BYTES:
                raise DefinitionError("Definition package exceeds the size limit")
            names = set()
            for info in entries:
                name = info.filename
                if (name in names or not name or "\\" in name
                        or PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts):
                    raise DefinitionError("Duplicate or invalid package member path")
                names.add(name)
                if info.flag_bits & 1 or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    raise DefinitionError("Package members must be unencrypted stored or deflated files")
            if "definition.json" not in names:
                raise DefinitionError("Package is missing definition.json")
            if package.getinfo("definition.json").file_size > MAX_DEFINITION_BYTES:
                raise DefinitionError("Definition JSON exceeds the size limit")
            definition = parse_definition(package.read("definition.json"))
            if definition["definition_version"] != 2:
                raise DefinitionError("Packaged definitions must use definition_version 2")
            creation = definition["creation"]
            template_path = creation["template"]
            if template_path not in names or package.getinfo(template_path).is_dir():
                raise DefinitionError("Package is missing its default codeplug template")
            if package.getinfo(template_path).file_size != creation["template_bytes"]:
                raise DefinitionError("Default codeplug template size does not match the definition")
            template = package.read(template_path)
            if hashlib.sha256(template).hexdigest() != creation["template_sha256"]:
                raise DefinitionError("Default codeplug template SHA-256 does not match the definition")
            return definition
    except (zipfile.BadZipFile, zlib.error, RuntimeError, NotImplementedError, EOFError) as error:
        raise DefinitionError(f"Invalid .wcp package: {error}") from error


def validate_definition(definition):
    if (definition.get("definition_type") != "codeplug"
            or type(definition.get("definition_version")) is not int
            or definition["definition_version"] not in (1, 2)):
        raise DefinitionError("Expected a version 1 or 2 codeplug definition")
    for key in ("fields", "recognition", "tables", "layout_checks"):
        if not isinstance(definition.get(key, {}), dict):
            raise DefinitionError(f"{key} must be an object")
    if "fields" not in definition or "recognition" not in definition:
        raise DefinitionError("Definition must contain fields and recognition")
    if not isinstance(definition["recognition"].get("required_fields", {}), dict):
        raise DefinitionError("Recognition required_fields must be an object")
    representations = definition.get("representations")
    if not isinstance(representations, list) or not representations:
        raise DefinitionError("representations must be a nonempty array")
    for item in representations:
        if not isinstance(item, dict) or not isinstance(item.get("decode"), dict):
            raise DefinitionError("Each representation must contain a decode object")
        ranges = item["decode"].get("input_equal_ranges", [])
        if not isinstance(ranges, list) or not all(isinstance(r, dict) for r in ranges):
            raise DefinitionError("input_equal_ranges must be an array of objects")
    if definition["definition_version"] == 2:
        creation = definition.get("creation")
        if not isinstance(creation, dict):
            raise DefinitionError("Version 2 definitions must contain a creation object")
        template = creation.get("template")
        if (not isinstance(template, str) or not template or "\\" in template
                or PurePosixPath(template).is_absolute() or ".." in PurePosixPath(template).parts
                or template == "definition.json"):
            raise DefinitionError("Creation template must name a package member")
        if creation.get("representation") not in [r.get("name") for r in representations]:
            raise DefinitionError("Creation template representation is not defined")
        size = creation.get("template_bytes")
        if type(size) is not int or not 0 < size <= MAX_PACKAGE_BYTES:
            raise DefinitionError("Invalid creation template size")
        digest = creation.get("template_sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise DefinitionError("Invalid creation template SHA-256")
        for key in ("channel_defaults", "construction"):
            if key in creation and not isinstance(creation[key], dict):
                raise DefinitionError(f"Creation {key} must be an object")
    field_maps = [definition["fields"]]
    for table in definition.get("tables", {}).values():
        if not isinstance(table, dict) or not isinstance(table.get("fields"), dict):
            raise DefinitionError("Each table must contain a fields object")
        if "join" in table and not isinstance(table["join"], dict):
            raise DefinitionError("join must be an object")
        if "segments" in table:
            if not isinstance(table["segments"], list) or not table["segments"]:
                raise DefinitionError("segments must be a nonempty array")
            if not all(isinstance(segment, dict) for segment in table["segments"]):
                raise DefinitionError("Each segment must be an object")
        for segment in table.get("segments", [table]):
            if "validity" in segment and not isinstance(segment["validity"], dict):
                raise DefinitionError("validity must be an object")
        field_maps.append(table["fields"])
    for fields in field_maps:
        for field in fields.values():
            if not isinstance(field, dict):
                raise DefinitionError("Each field must be an object")
            for key in ("enum", "only_if", "reference"):
                if key in field and not isinstance(field[key], dict):
                    raise DefinitionError(f"Field {key} must be an object")
    layout = definition.get("layout_checks", {})
    if not isinstance(layout.get("expected_addresses", {}), dict):
        raise DefinitionError("expected_addresses must be an object")
    for key in ("equal_fields", "relationships"):
        if not isinstance(layout.get(key, []), list):
            raise DefinitionError(f"{key} must be an array")


def inspect_definition(data, definition, max_records=100000):
    validate_definition(definition)
    results, equivalent = [], {}
    for representation in definition["representations"]:
        result = {"representation": representation.get("name", "unnamed")}
        results.append(result)
        try:
            image, encoding = decode_representation(data, representation)
            digest = hashlib.sha256(image).hexdigest()
            result["decoded_sha256"] = digest
            if digest in equivalent:
                previous = equivalent[digest]
                checks = encoding + [c for c in previous["checks"] if c["category"] != "encoding"]
                result.update(equivalent_to=previous["representation"], checks=checks,
                              errors=previous["errors"])
            else:
                reader = DefinitionReader(image, definition, max_records)
                checks = encoding + check_recognition(reader, definition) + check_layout(reader, definition)
                result["decoded"] = reader.decode_available_fields_and_tables()
                checks += reader.reference_checks()
                result.update(checks=checks, errors=reader.errors)
                equivalent[digest] = result
            required_ok = all(c["status"] == "match" for c in checks if c["required"])
            similarities = any(c["status"] == "match" and c.get("informative", False) for c in checks)
            result["status"] = ("recognized" if required_ok and not result["errors"] else
                                "partial" if similarities else "no_match")
        except (DefinitionError, KeyError, TypeError, ValueError, UnicodeError) as error:
            result.update(status="unreadable", error=str(error))
    return results


def text_report(report):
    lines = [f"Input: {report['input']} ({report['bytes']:,} bytes)"]
    for definition in report["definitions"]:
        lines.append(f"\nDefinition: {definition.get('id', definition['file'])}")
        if "error" in definition:
            lines.append(f"  Definition error: {definition['error']}")
        for result in definition.get("results", []):
            lines.append(f"  {result['representation']}: {result['status'].upper().replace('_', ' ')}")
            if "equivalent_to" in result:
                lines.append(f"    Same decoded bytes as {result['equivalent_to']}; no unique representation is implied.")
                continue
            required = [c for c in result.get("checks", []) if c["required"]]
            if required:
                lines.append("    Required checks: " + ", ".join(
                    f"{sum(c['status'] == status for c in required)} {label}"
                    for status, label in (("match", "matched"), ("mismatch", "different"), ("unreadable", "unreadable"))))
            if result["status"] in ("partial", "no_match"):
                count = sum(c.get("informative", False) for c in result.get("checks", []))
                lines.append(f"    Partial-match evidence: {count} informative checks")
            compact = result["status"] == "no_match"
            for status, label in (("match", "Matches"), ("mismatch", "Differences"), ("unreadable", "Unreadable")):
                selected = [c for c in required if c["status"] == status
                            and (not compact or c["category"] == "recognition")
                            and (status != "match" or result["status"] == "recognized" or c.get("informative", False))]
                if selected:
                    lines.append(f"    {label}: " + "; ".join(c["name"] + (" (optional)" if not c["required"] else "")
                                                               for c in selected))
                    if status != "match":
                        for check in selected[:8]:
                            detail = (f"{len(check['invalid'])} invalid references" if check.get("invalid") else
                                      check.get("error", f"expected {check.get('expected')!r}, got {check.get('actual')!r}"))
                            lines.append(f"      {check['name']}: {detail}")
            decoded = result.get("decoded", {})
            if decoded and not compact:
                lines.append("    Tables: " + ", ".join(f"{name} {len(table['records'])}/{table.get('count', '?')} "
                                                        + ("selected slots" if "selected_indices" in table else "records")
                                                        for name, table in decoded["tables"].items()))
                fields = decoded["fields"]
                recognition = definition.get("recognition", {})
                for key in (recognition.get("model_field"),
                            recognition.get("version_field", recognition.get("cps_version_field"))):
                    if key in fields:
                        lines.append(f"    {key}: {json.dumps(fields[key], ensure_ascii=True)}")
                for key in ("radio_name", "radio_id"):
                    value = (json.dumps(fields[key], ensure_ascii=True)
                             if key in fields else "unavailable")
                    lines.append(f"    {key}: {value}")
            for error in ([] if compact else result.get("errors", [])[:8]):
                lines.append(f"    Decode issue: {error['path']}: {error['message']}")
            if not compact and len(result.get("errors", [])) > 8:
                lines.append("    Further decode issues are included in --json output.")
            if "error" in result:
                lines.append(f"    {result['error']}")
            if result["status"] == "partial":
                lines.append("    Similarities found; this is not confirmed format compatibility.")
    lines.append("\n" + ("Input file unchanged. " if "generated_definition" in report else "Read-only inspection. ")
                 + "Use --json for decoded fields, records, and all check results.")
    return "\n".join(lines)



class GenerationError(ValueError):
    """The definition/source cannot establish a consistent default image."""


def require(condition, message):
    if not condition:
        raise GenerationError(message)


def raw_value(field, value):
    enum = field.get("enum", {})
    if isinstance(value, str) and value in enum.values():
        keys = [key for key, label in enum.items() if label == value]
        require(len(keys) == 1, "Ambiguous enum default")
        value = int(keys[0])
    if (integer_type(field["type"]) or field["type"] in ("bcdle", "bcdbe")) and isinstance(value, str) and value.startswith("0x"):
        value = integer(value)
    if "scale" in field:
        scale = integer(field["scale"])
        require(type(value) is int and scale > 0 and value % scale == 0, "Default does not fit field scale")
        value //= scale
    return value


def write_field(image, reader, field, value, scope):
    require("value" not in field, "Cannot write a computed field")
    if "only_if" in field:
        condition = field["only_if"]
        require(reader.expression(condition["value"], scope) in condition["in"], "Default targets an inactive field")
    at = (reader.expression(field["at"], scope) if "at" in field else
          scope.base + reader.expression(field["offset"], scope))
    if "count" in field:
        count = reader.expression(field["count"], scope)
        require(isinstance(value, list) and len(value) == count, "Default array length differs from its count")
        stride = integer(field["stride_bytes"])
        require(stride > 0, "Invalid default array stride")
        scalar = {k: v for k, v in field.items() if k not in ("count", "stride_bytes", "at", "offset")}
        for i, element in enumerate(value):
            write_field(image, reader, dict(scalar, at=at + i * stride), element, scope)
        return
    size = reader.storage_size(field)
    old = byte_range(image, at, size)
    kind = field["type"]
    numeric = integer_type(kind)
    value = raw_value(field, value)
    if numeric or kind in ("bcdle", "bcdbe"):
        require(type(value) is int, "Numeric default must be an integer")
        require(value >= integer(field.get("minimum", 0)), "Default is below field minimum")
        if "maximum" in field:
            require(value <= integer(field["maximum"]), "Default exceeds field maximum")
        if numeric:
            width, order = numeric
            mask = integer(field.get("mask", (1 << (width * 8)) - 1))
            shift = integer(field.get("shift", 0))
            require(0 <= mask < 1 << (width * 8) and 0 <= shift < width * 8, "Invalid numeric mask/shift")
            require(value >= 0 and (value << shift) & ~mask == 0, "Default does not fit numeric storage")
            encoded = ((int.from_bytes(old, order) & ~mask) | (value << shift)).to_bytes(width, order)
        else:
            digits = str(value).zfill(size * 2)
            require(value >= 0 and len(digits) == size * 2, "Default does not fit BCD storage")
            encoded = bytes.fromhex(digits)
            if kind == "bcdle":
                encoded = encoded[::-1]
    elif kind in ("ascii", "utf16le"):
        require(isinstance(value, str), "Text default must be a string")
        require("\0" not in value, "Embedded NUL in text default")
        encoded = value.encode("ascii" if kind == "ascii" else "utf-16-le")
        terminator = 1 if kind == "ascii" else 2
        require(len(encoded) + (terminator if field.get("require_terminator") else 0) <= size,
                "Default text is too long")
        encoded = encoded.ljust(size, b"\0")
    elif kind == "packed6":
        require(isinstance(value, str), "Packed default must be a string")
        alphabet = field["alphabet"]
        count = integer(field["characters"])
        require(len(alphabet) == 64 and 0 <= count * 6 <= size * 8 and len(value) <= count,
                "Invalid packed default size")
        padding = integer(field.get("padding_code", alphabet.find(" ")))
        space = integer(field.get("space_code", alphabet.find(" ")))
        require(0 <= padding < 64 and 0 <= space < 64, "Packed alphabet has no space code")
        bits = 0
        for char in value:
            code = space if char == " " else alphabet.find(char)
            require(0 <= code < 64, "Default character is absent from packed alphabet")
            bits = (bits << 6) | code
        for _ in range(count - len(value)):
            bits = (bits << 6) | padding
        shift = size * 8 - count * 6
        mask = ((1 << (count * 6)) - 1) << shift
        encoded = ((int.from_bytes(old, "big") & ~mask) | (bits << shift)).to_bytes(size, "big")
    else:
        raise GenerationError(f"Unsupported default encoding: {kind}")
    image[at:at + size] = encoded


def scope_for(reader, fields, base, index=None):
    return Scope(reader, fields, "default", base, index)


def write_fields(image, reader, fields, values, base=None, index=None):
    require(isinstance(values, dict), "Record defaults must be an object")
    # Count scalars must be available before arrays, independent of JSON key order.
    for arrays in (False, True):
        for name, value in values.items():
            require(name in fields, f"Undefined default field: {name}")
            if ("count" in fields[name]) != arrays:
                continue
            current = DefinitionReader(image, reader.definition)
            scope = scope_for(current, fields, base, index)
            if isinstance(value, dict):
                value = current.expression(value, scope)
            write_field(image, current, fields[name], value, scope)


def build_record(original, definition, table, records, spec, index, reader):
    require(isinstance(spec, dict), "Default record must be an object")
    values = spec.get("fields", {})
    source = spec.get("copy")
    seed = None
    if source is not None:
        require(isinstance(source, dict) and isinstance(source.get("match", {}), dict), "Invalid record copy rule")
        require(all(key in table["fields"] for key in source.get("match", {})), "Copy rule refers to an undefined field")
        for record in records:
            if all((record["raw_values"].get(key) in value if isinstance(value, list) else record["raw_values"].get(key) == value) for key, value in source.get("match", {}).items()):
                if "stride_bytes" in table:
                    size = integer(table["stride_bytes"])
                else:
                    original_scope = scope_for(DefinitionReader(original, definition), table["fields"], record["address"], record["index"])
                    size = original_scope.reader.expression(table["record_bytes"], original_scope)
                seed = byte_range(original, record["address"], size)
                break
        if seed is None and "fallback_hex" in source:
            seed = bytes.fromhex(source["fallback_hex"])
        require(seed is not None, "No source record or fallback for requested default mode/list")
    if "stride_bytes" in table:
        size = integer(table["stride_bytes"])
    else:
        # Size expressions may depend on the new record's scalar defaults.
        sizing = scope_for(reader, table["fields"], 0, index)
        for key, value in values.items():
            require(key in table["fields"], f"Undefined default field: {key}")
            if "count" not in table["fields"][key]:
                raw = raw_value(table["fields"][key], value)
                sizing.cache[key] = {"raw": raw, "value": raw}
        size = reader.expression(table["record_bytes"], sizing)
    require(0 < size <= MAX_PACKAGE_BYTES, "Invalid default record size")
    require(seed is None or len(seed) == size, "Copied record size differs from default record size")
    record = bytearray(seed if seed is not None else bytes(size))
    for fill in spec.get("fills", []):
        at, length, value = (integer(fill[key]) for key in ("offset", "bytes", "value"))
        require(0 <= value <= 255, "Record fill must be a byte")
        byte_range(record, at, length)
        record[at:at + length] = bytes([value]) * length
    return record


def relocate(image, reader, rule, old_start, old_end, delta):
    require(isinstance(rule, list), "Relocations must be an array")
    pointers = []
    for item in rule:
        if "field" in item:
            field = reader.definition["fields"][item["field"]]
            require(field["type"] == "u32le", "Relocation field must be u32le")
            addresses = [reader.expression(field["at"])]
        else:
            start = reader.expression(item["at"])
            count = integer(item["count"])
            stride = integer(item["stride_bytes"])
            require(0 <= count <= 1024 and stride >= 4, "Invalid pointer directory")
            addresses = [start + i * stride for i in range(count)]
        for at in addresses:
            require(at + 4 <= old_start, "Pointer directory must precede the resized table tail")
            value = int.from_bytes(byte_range(image, at, 4), "little")
            if value:
                require(value >= old_end, "Relocation pointer precedes the following section")
                require(0 <= value + delta < len(image), "Relocation pointer exceeds resized image")
                pointers.append((at, value + delta))
    require(len({at for at, _ in pointers}) == len(pointers), "Duplicate relocation pointer")
    for at, value in pointers:
        image[at:at + 4] = value.to_bytes(4, "little")



def reset_bitmap_selection(image, reader, layouts, active_count):
    """Select the first active_count slots; preserve bits outside slot capacity."""
    global_index = 0
    for layout in layouts:
        validity = layout.get("validity", {})
        require("bitmap_at" in validity, "Selection reset requires a bitmap validity rule")
        count = reader.expression(layout["count"])
        at = reader.expression(validity["bitmap_at"])
        order = validity.get("bit_order", "lsb_first")
        require(order in ("lsb_first", "msb_first"), "Invalid bitmap bit order")
        byte_range(image, at, (count + 7) // 8)
        for local_index in range(count):
            shift = local_index % 8 if order == "lsb_first" else 7 - local_index % 8
            mask = 1 << shift
            target = at + local_index // 8
            image[target] = ((image[target] & ~mask) |
                             (mask if global_index + local_index < active_count else 0))
        global_index += count
    require(active_count <= global_index, "Default selection exceeds bitmap capacity")


def reset_segmented_table(image, original, definition, table, old, plan, reader):
    """Write banked fixed-stride tables using their declared physical slots."""
    require(not plan.get("resize"), "Segmented tables cannot be resized")
    require(plan.get("reset_selection") is True, "Segmented reset requires explicit selection reset")
    layouts = table["segments"]
    slots, ranges, bitmaps = [], [], []
    total = reader.expression(table["count"])
    require(0 <= total <= 100000, "Segmented table exceeds slot limit")
    for layout in layouts:
        require("stride_bytes" in layout, "Segmented reset requires fixed strides")
        count = reader.expression(layout["count"])
        stride = integer(layout["stride_bytes"])
        span = integer(layout.get("logical_span_bytes", stride))
        at = reader.expression(layout["at"])
        require(0 <= count <= total and stride > 0 and 0 < span <= stride,
                "Invalid segmented reset extent")
        byte_range(image, at, (count - 1) * stride + span if count else 0)
        if count:
            ranges.append((at, at + (count - 1) * stride + span))
        validity = layout.get("validity", {})
        require("bitmap_at" in validity, "Segmented reset requires bitmap selection")
        bitmap = reader.expression(validity["bitmap_at"])
        byte_range(image, bitmap, (count + 7) // 8)
        bitmaps.append((bitmap, bitmap + (count + 7) // 8))
        slots.extend((at + i * stride, stride, span) for i in range(count))
    require(len(slots) == total, "Segment slot counts differ from table capacity")
    require(len({stride for _, stride, _ in slots}) <= 1,
            "Segmented reset requires uniform record strides")
    ranges.sort()
    require(all(left[1] <= right[0] for left, right in zip(ranges, ranges[1:])),
            "Segmented record storage overlaps")
    require(all(end <= start2 or end2 <= start for start, end in ranges for start2, end2 in bitmaps),
            "Bitmap overlaps segmented record storage")
    specs = plan["records"]
    require(len(specs) <= total, "No room for segmented default records")
    # Snapshot seeds before clearing; copy reads the immutable original image.
    records = []
    for i, spec in enumerate(specs):
        proxy = dict(table, stride_bytes=slots[i][1])
        records.append(build_record(original, definition, proxy, old["records"], spec, i, reader))
    original_indexes = {row["index"] for row in old["records"]}
    for i, (at, stride, span) in enumerate(slots):
        if plan.get("clear_all_slots") or i in original_indexes:
            image[at:at + span] = bytes(span)
    reset_bitmap_selection(image, reader, layouts, len(specs))
    for i, (record, spec) in enumerate(zip(records, specs)):
        at = slots[i][0]
        image[at:at + len(record)] = record
        write_fields(image, reader, table["fields"], spec.get("fields", {}), at, i)


def create_default_codeplug(source: bytes, definition: dict) -> bytes:
    """Apply a definition reset profile; return checked bytes without writing files."""
    try:
        require(isinstance(source, (bytes, bytearray)), "Source must be bytes")
        require(0 < len(source) <= MAX_PACKAGE_BYTES, "Source exceeds size limit")
        require(isinstance(definition, dict), "Definition must be an object")
        validate_definition(definition)
        definition = deepcopy(definition)
        profile = definition.get("default_generation")
        require(isinstance(profile, dict) and type(profile.get("version")) is int and profile["version"] == 1, "Definition has no supported default-generation profile")
        results = inspect_definition(bytes(source), definition)
        candidate = next((i for i, result in enumerate(results) if result["status"] == "recognized"), None)
        require(candidate is not None, "Source must be fully recognized before default generation")
        representation = definition["representations"][candidate]
        original, _ = decode_representation(bytes(source), representation)
        image = bytearray(original)
        tables = results[candidate]["decoded"]["tables"]
        expected = []
        plans = profile.get("tables", [])
        require(isinstance(plans, list) and all(isinstance(plan, dict) for plan in plans), "Reset tables must be an array of objects")
        require(plans or profile.get("globals"), "Default profile does not define any resets")
        require(len({plan["table"] for plan in plans}) == len(plans), "Duplicate reset table")
        for plan in plans:
            for flag in ("resize", "clear_all_slots", "reset_selection"):
                require(type(plan.get(flag, False)) is bool, f"{flag} must be a boolean")
            name = plan["table"]
            require(name in definition["tables"], f"Undefined reset table: {name}")
            table = definition["tables"][name]
            specs = plan["records"]
            require(isinstance(specs, list) and len(specs) <= 10000, "Invalid default record list")
            reader = DefinitionReader(image, definition)
            old = tables[name]
            if "segments" in table:
                reset_segmented_table(image, original, definition, table, old, plan, reader)
                if "count_field" in plan:
                    write_fields(image, reader, definition["fields"], {plan["count_field"]: len(specs)})
                expected.append((name, specs))
                continue
            at = reader.expression(table["at"])
            old_at = old["address"]
            if "stride_bytes" in table:
                stride = integer(table["stride_bytes"])
                require(len(specs) <= integer(plan.get("capacity", old["count"])), "No room for default records")
                clear_ranges = [(at + row["address"] - old_at, integer(table.get("logical_span_bytes", stride)))
                                for row in old["records"]]
                if plan.get("clear_all_slots"):
                    clear_ranges = [(at + i * stride, integer(table.get("logical_span_bytes", stride)))
                                    for i in range(old["count"])]
                for start, size in clear_ranges:
                    byte_range(image, start, size)
                    image[start:start + size] = bytes(size)
            else:
                require(plan.get("resize") is True, "Variable records require an explicit resize rule")
                stride = None
            records = [build_record(original, definition, table, old["records"], spec, i, reader)
                       for i, spec in enumerate(specs)]
            if plan.get("resize"):
                require("validity" not in table, "Cannot resize a selected-slot table")
                old_size = 0
                for row in old["records"]:
                    if stride:
                        old_size += stride
                    else:
                        old_reader = DefinitionReader(original, definition)
                        old_size += old_reader.expression(table["record_bytes"], scope_for(old_reader, table["fields"], row["address"], row["index"]))
                if stride and old["count"]:
                    old_size += integer(table.get("logical_span_bytes", stride)) - stride
                new_size = sum(len(record) for record in records)
                require(stride is None or table.get("logical_span_bytes", stride) == stride,
                        "Overlapping records cannot be resized")
                byte_range(image, at, old_size)
                require(at + old_size == len(image) or "relocate" in plan,
                        "Resizing a non-tail table requires relocation rules")
                require(len(image) - old_size + new_size <= MAX_PACKAGE_BYTES, "Resized image exceeds limit")
                old_end = at + old_size
                image[at:old_end] = b"".join(records)
                if "relocate" in plan:
                    relocate(image, reader, plan["relocate"], at, old_end, new_size - old_size)
            else:
                cursor = at
                for record in records:
                    byte_range(image, cursor, len(record))
                    image[cursor:cursor + len(record)] = record
                    cursor += len(record)
            cursor = at
            for i, (record, spec) in enumerate(zip(records, specs)):
                write_fields(image, reader, table["fields"], spec.get("fields", {}), cursor, i)
                cursor += len(record)
            if plan.get("reset_selection"):
                require(not plan.get("resize"), "Cannot resize bitmap-selected storage")
                reset_bitmap_selection(image, reader, [table], len(specs))
            if "count_field" in plan:
                write_fields(image, reader, definition["fields"], {plan["count_field"]: len(specs)})
            expected.append((name, specs))
        reader = DefinitionReader(image, definition)
        write_fields(image, reader, definition["fields"], profile.get("globals", {}))
        rule = representation["decode"]
        if rule["operation"] == "identity":
            encoded = bytes(image)
        elif rule["operation"] == "repeating_xor":
            mask = byte_range(source, integer(rule["mask_input_offset"]), integer(rule["mask_bytes"]))
            phase = integer(rule.get("mask_phase_at_input_zero", 0))
            encoded = bytes(value ^ mask[(i + phase) % len(mask)] for i, value in enumerate(image))
        else:
            raise GenerationError("Unsupported output encoding")
        checked = inspect_definition(encoded, definition)[candidate]
        require(checked["status"] == "recognized", "Generated default failed structural/reference validation: " +
                str(checked.get("errors", [])[:2]) + str([c["name"] for c in checked.get("checks", []) if c["required"] and c["status"] != "match"]))
        for name, specs in expected:
            rows = {row["index"]: row for row in checked["decoded"]["tables"][name]["records"]}
            for i, spec in enumerate(specs):
                require(i in rows and rows[i]["complete"], "Generated default record is inactive or incomplete")
                fields = definition["tables"][name]["fields"]
                for key, value in spec.get("fields", {}).items():
                    raw = [raw_value(fields[key], item) for item in value] if isinstance(value, list) else raw_value(fields[key], value)
                    actual = rows[i]["raw_values"].get(key)
                    if fields[key]["type"] == "packed6":
                        field = fields[key]
                        padding = integer(field.get("padding_code", field["alphabet"].find(" ")))
                        raw = value.ljust(integer(field["characters"]), field["alphabet"][padding])
                        if "trim" in field:
                            raw = raw.strip(" ")
                        actual = rows[i]["fields"].get(key)
                    require(actual == raw, f"Generated field differs: {name}.{key}")
        for name, value in profile.get("globals", {}).items():
            wanted = reader.expression(value) if isinstance(value, dict) else raw_value(definition["fields"][name], value)
            require(checked["decoded"]["raw_fields"].get(name) == wanted, f"Generated global differs: {name}")
        return encoded
    except GenerationError:
        raise
    except (DefinitionError, KeyError, TypeError, ValueError, OverflowError, UnicodeError) as error:
        raise GenerationError(str(error)) from error



class DefinitionGenerationError(ValueError):
    """The input does not establish one consistent, generation-capable layout."""


def _field_dependencies(value):
    if isinstance(value, dict):
        if set(value) == {"field"} and isinstance(value["field"], str):
            yield value["field"]
        else:
            for child in value.values():
                yield from _field_dependencies(child)
    elif isinstance(value, list):
        for child in value:
            yield from _field_dependencies(child)


def _table_role(name, table):
    fields = table.get("fields", {})
    explicit = table.get("role")
    if explicit in ("channels", "contacts", "zones"):
        return explicit
    enums = [set(field.get("enum", {}).values()) for field in fields.values()]
    if sum(field.get("unit") == "Hz" for field in fields.values()) >= 2 and any(
            {"analog", "digital"} <= values for values in enums):
        return "channels"
    if "name" in fields and "id" in fields and any({"group", "private"} <= values for values in enums):
        return "contacts"
    if (any(token in ("zone", "zones") for token in name.split("_"))
            and "name" in fields and "reference" in fields.get("members", {})):
        return "zones"
    return None


def _table_references(table):
    for field in table["fields"].values():
        if "reference" in field:
            yield field["reference"]["table"]


def _close_fields(definition, names):
    result = set()
    pending = list(names)
    while pending:
        name = pending.pop()
        if name in result:
            continue
        if name not in definition["fields"]:
            raise DefinitionGenerationError(f"Undefined global dependency: {name}")
        result.add(name)
        pending.extend(_field_dependencies(definition["fields"][name]))
    return result


def _augment_guard_fields(definition, fields):
    """Keep independent pointer aliases/size guards connected to this component."""
    fields = set(fields)
    changed = True
    while changed:
        changed = False
        layout = definition.get("layout_checks", {})
        related = [set(pair) for pair in layout.get("equal_fields", [])]
        related += [set(_field_dependencies(rule)) for rule in layout.get("relationships", [])]
        for names in related:
            if names & fields and names <= definition["fields"].keys() and not names <= fields:
                fields = _close_fields(definition, fields | names)
                changed = True
    return fields


def _guard_subset(definition, fields):
    layout = definition.get("layout_checks", {})
    result = {}
    equal = [pair for pair in layout.get("equal_fields", []) if set(pair) <= fields]
    if equal:
        result["equal_fields"] = equal
    expected = {name: value for name, value in layout.get("expected_addresses", {}).items() if name in fields}
    if expected:
        result["expected_addresses"] = expected
    relationships = [rule for rule in layout.get("relationships", [])
                     if set(_field_dependencies(rule)) <= fields]
    if relationships:
        result["relationships"] = relationships
    return result


def _check_bundle_globals(definition, reader, fields, guards):
    for name in sorted(fields):
        reader.globals.get(name)
    if any(error["path"].startswith("fields.") and error["path"].split(".", 1)[1] in fields
           for error in reader.errors):
        raise DefinitionGenerationError("Global values violate their field constraints")
    checks = check_layout(reader, {"layout_checks": guards})
    if any(c["required"] and c["status"] != "match" for c in checks):
        raise DefinitionGenerationError("Layout relationships do not match")


def _identity_bundle(definition, reader, roots):
    recognition = deepcopy(definition["recognition"])
    matching_identity = all(c["status"] == "match" for c in
                            check_recognition(reader, definition) if c["required"])
    names = {"radio_name", "radio_id", recognition["model_field"]}
    if matching_identity:
        table_dependencies = set(_field_dependencies(definition["tables"]))
        table_dependencies.update(_field_dependencies(
            definition.get("default_generation", {}).get("tables", [])))
        table_fields = _close_fields(definition, table_dependencies)
        names.update(set(definition["fields"]) - table_fields)
    message_fields = {
        name for name in ("power_on_message_1", "power_on_message_2")
        if name in definition["fields"]
    }
    names.update(message_fields)
    version = recognition.get("version_field", recognition.get("cps_version_field"))
    if version:
        names.add(version)
    names.update(recognition.get("required_fields", {}))
    # Keep redundant model/format markers as independent checks where specified.
    for pair in definition.get("layout_checks", {}).get("equal_fields", []):
        if recognition["model_field"] in pair:
            names.update(pair)
    fields = _augment_guard_fields(definition, _close_fields(definition, names))
    for name in ("radio_name", recognition["model_field"]):
        value = reader.globals.get(name)["value"]
        if not isinstance(value, str) or not value.strip() or not value.isprintable() or not any(c.isalnum() for c in value):
            raise DefinitionGenerationError("Radio identity/format text is blank or invalid")
    identifier = reader.globals.get("radio_id")["raw"]
    if type(identifier) is not int or not 1 <= identifier <= 0xFFFFFF:
        raise DefinitionGenerationError("Radio ID is not a valid 24-bit identifier")
    recognition["model_value"] = reader.globals.get(recognition["model_field"])["value"]
    if version:
        value = reader.globals.get(version)["value"]
        if not isinstance(value, str) or not value.strip() or not value.isprintable() or not any(c.isalnum() for c in value):
            raise DefinitionGenerationError("Format version is blank or invalid")
        recognition["supported_versions" if "version_field" in recognition else "supported_cps_versions"] = [value]
    if any(c["required"] and c["status"] != "match" for c in check_recognition(reader, {"recognition": recognition})):
        raise DefinitionGenerationError("Required format signatures do not match")
    profile = definition.get("default_generation", {})
    defaults = profile.get("globals", {})
    if not all(name in defaults for name in ("radio_name", "radio_id")):
        raise DefinitionGenerationError("Radio identity reset values are not defined")
    guards = _guard_subset(definition, fields)
    # Related formats may contain different primary and secondary model text.
    guards["equal_fields"] = [
        pair for pair in guards.get("equal_fields", [])
        if recognition["model_field"] not in pair
        or reader.globals.get(pair[0])["value"] == reader.globals.get(pair[1])["value"]
    ]
    _check_bundle_globals(definition, reader, fields, guards)
    return {"role": "identity", "definition": definition, "root": None, "roots": roots,
            "tables": set(), "fields": fields, "guards": guards, "recognition": recognition,
            "matching_identity": matching_identity,
            "plans": [], "globals": {
                **{name: defaults[name] for name in ("radio_name", "radio_id")},
                **{name: "" for name in message_fields}}}


def _component_bundle(role, root, definition, reader, decoded, roots, known_format):
    profile = definition.get("default_generation", {})
    if profile.get("version") != 1:
        raise DefinitionGenerationError("No supported reset profile")
    plans = profile.get("tables", [])
    by_name = {plan["table"]: plan for plan in plans}
    if root not in by_name:
        raise DefinitionGenerationError("Root table has no reset rule")
    tables = definition["tables"]
    owned = {root}
    core = set(roots)
    changed = True
    while changed:
        changed = False
        count_fields = set()
        selectors = set()
        for name in owned:
            count_fields.update(_field_dependencies(tables[name].get("count")))
            for segment in tables[name].get("segments", [tables[name]]):
                validity = segment.get("validity", {})
                if "index_table" in validity:
                    selectors.add(validity["index_table"])
        for name, table in tables.items():
            if name in owned or name in core:
                continue
            fields = table["fields"]
            references = set(_table_references(table))
            outbound = any(name in set(_table_references(tables[n])) for n in owned)
            auxiliary = (len(fields) == 1 or "join" in table or not {"name", "members"} & fields.keys())
            attached = ((table.get("join", {}).get("table") in owned)
                        or (auxiliary and bool(references & owned))
                        or (auxiliary and bool(set(_field_dependencies(table.get("count"))) & count_fields))
                        or name in selectors)
            # Channels own referenced receive groups and inbound scan-list members.
            if role == "channels":
                attached = attached or outbound or bool(references & owned)
                for segment in table.get("segments", [table]):
                    if segment.get("validity", {}).get("index_table") in selectors:
                        attached = True
            if attached:
                owned.add(name)
                changed = True
    for name in owned:
        if name not in by_name:
            raise DefinitionGenerationError(f"No reset rule for related table {name}")
        if name not in decoded["tables"] or not decoded["tables"][name]["complete"]:
            raise DefinitionGenerationError(f"Related table {name} could not be decoded completely")
    rows = decoded["tables"][root]["records"]
    root_fields = tables[root]["fields"]
    informative = False
    if role == "channels":
        frequencies = [name for name, f in root_fields.items() if f.get("unit") == "Hz"]
        modes = [name for name, f in root_fields.items() if {"analog", "digital"} <= set(f.get("enum", {}).values())]
        joined = [name for name in owned if tables[name].get("join", {}).get("table") == root]
        if len(frequencies) < 2 or not modes or ("name" not in root_fields and not joined):
            raise DefinitionGenerationError("Channel name, mode and frequency mappings are incomplete")
        for row in rows:
            values = row["fields"]
            name = values.get("name")
            if not name:
                for table_name in joined:
                    name = next((r["fields"].get("name") for r in decoded["tables"][table_name]["records"]
                                 if r["index"] == row["index"]), None)
                    if name:
                        break
            if (isinstance(name, str) and name.strip() and any(values.get(k) in ("analog", "digital") for k in modes)
                    and len(frequencies) >= 2 and all(type(values.get(k)) is int and 10_000_000 <= values[k] <= 10_000_000_000 for k in frequencies)):
                informative = True
                break
    elif role == "contacts":
        kinds = [key for key, field in root_fields.items() if {"private", "group"} <= set(field.get("enum", {}).values())]
        if not kinds or not {"id", "name"} <= root_fields.keys():
            raise DefinitionGenerationError("Contact name, ID and call-type mappings are incomplete")
        informative = any(isinstance(row["fields"].get("name"), str) and any(c.isalnum() for c in row["fields"]["name"])
                          and type(row["fields"].get("id")) is int and 1 <= row["fields"]["id"] <= 0xFFFFFF
                          and any(row["fields"].get(k) in ("group", "private", "all_call") for k in kinds) for row in rows)
    else:
        if "name" not in root_fields or "reference" not in root_fields.get("members", {}):
            raise DefinitionGenerationError("Zone name and membership mappings are incomplete")
        informative = any(isinstance(row["fields"].get("name"), str) and row["fields"]["name"].strip()
                          and any(value > 0 for value in row["raw_values"].get("members", [])) for row in rows)
    if not informative and not (known_format and not rows):
        raise DefinitionGenerationError("No informative populated records establish this layout")
    selected_plans = [plan for plan in plans if plan["table"] in owned]
    globals_ = {key: value for key, value in profile.get("globals", {}).items() if key not in ("radio_name", "radio_id")}
    names = set(globals_) | set(_field_dependencies(globals_))
    for name in owned:
        names.update(_field_dependencies(tables[name]))
    for plan in selected_plans:
        names.update(_field_dependencies(plan))
        if "count_field" in plan:
            names.add(plan["count_field"])
    fields = _augment_guard_fields(definition, _close_fields(definition, names))
    guards = _guard_subset(definition, fields)
    _check_bundle_globals(definition, reader, fields, guards)
    for check in reader.reference_checks():
        source_table = check["name"].split(".", 1)[0]
        if check["category"] == "references" and source_table in owned:
            field = tables[source_table]["fields"][check["name"].split(".", 1)[1]]
            if field["reference"]["table"] in owned and check["status"] != "match":
                raise DefinitionGenerationError(f"Internal reference check failed: {check['name']}")
    return {"role": role, "definition": definition, "root": root, "roots": roots,
            "tables": owned, "fields": fields, "guards": guards,
            "plans": selected_plans, "globals": globals_}


def _remap_mapping(value, fields, tables):
    if isinstance(value, list):
        return [_remap_mapping(child, fields, tables) for child in value]
    if not isinstance(value, dict):
        return value
    if set(value) == {"field"} and isinstance(value["field"], str):
        return {"field": fields[value["field"]]}
    result = {}
    for key, child in value.items():
        if key in ("table", "index_table") and isinstance(child, str):
            result[key] = tables[child]
        elif key == "count_field":
            result[key] = fields[child]
        else:
            result[key] = _remap_mapping(child, fields, tables)
    return result


def _base_mapping_name(name):
    while "__" in name and name.split("__", 1)[0] in ("identity", "channels", "zones", "contacts"):
        name = name.split("__", 1)[1]
    return name


def _bundle_signature(bundle, aliases):
    # Normalize generated namespaces so saved definitions remain reusable donors.
    def structural(value):
        if isinstance(value, dict):
            return {key: structural(child) for key, child in value.items()
                    if not (key in ("meaning", "rule", "order") and isinstance(child, str))}
        if isinstance(value, list):
            return [structural(child) for child in value]
        return value
    normalized = _assemble_definition([bundle], {"name": "signature"}, aliases)
    return json.dumps(structural({key: normalized[key] for key in
                                 ("fields", "tables", "default_generation", "layout_checks", "recognition")}), sort_keys=True)


def _assemble_definition(bundles, representation, aliases):
    draft = {"definition_type": "codeplug", "definition_version": 1, "id": "generated",
             "notation": {}, "representations": [deepcopy(representation)],
             "recognition": {}, "fields": {}, "tables": {}, "layout_checks": {},
             "default_generation": {"version": 1, "tables": [], "globals": {}},
             "preservation": {"retain_complete_original_image": True, "retain_unknown_bytes_and_bits": True,
                              "unknown_sections": "Only the declared reset profile is applied. Unmapped settings are preserved; this is not a complete factory reset or complete removal of personal settings."}}
    draft["representations"][0].setdefault("name", "generated_image")
    for bundle in bundles:
        role = bundle["role"]
        source = bundle["definition"]
        field_map = {key: key if role == "identity" and key in (
                         "radio_name", "radio_id", "power_on_message_1", "power_on_message_2")
                     else role + "__" + _base_mapping_name(key)
                     for key in bundle["fields"]}
        table_map = dict(aliases)
        table_map.update(bundle["roots"])
        for name in bundle["tables"]:
            table_map[name] = role if name == bundle["root"] else role + "__" + _base_mapping_name(name)
        for name in sorted(bundle["fields"]):
            mapped = _remap_mapping(source["fields"][name], field_map, table_map)
            if field_map[name] in draft["fields"] and draft["fields"][field_map[name]] != mapped:
                raise DefinitionGenerationError(f"Conflicting field mapping: {name}")
            draft["fields"][field_map[name]] = mapped
        for name in sorted(bundle["tables"]):
            draft["tables"][table_map[name]] = _remap_mapping(source["tables"][name], field_map, table_map)
        draft["default_generation"]["tables"].extend(_remap_mapping(bundle["plans"], field_map, table_map))
        draft["default_generation"]["globals"].update({field_map[name]: _remap_mapping(value, field_map, table_map)
                                                       for name, value in bundle["globals"].items()})
        guards = bundle["guards"]
        for key in ("equal_fields", "relationships"):
            if key in guards:
                values = ([[field_map[name] for name in pair] for pair in guards[key]] if key == "equal_fields"
                          else _remap_mapping(guards[key], field_map, table_map))
                existing = draft["layout_checks"].setdefault(key, [])
                for value in values:
                    if value not in existing:
                        existing.append(value)
        if "expected_addresses" in guards:
            draft["layout_checks"].setdefault("expected_addresses", {}).update(
                {field_map[name]: value for name, value in guards["expected_addresses"].items()})
        for key, value in source.get("notation", {}).items():
            if key not in draft["notation"]:
                draft["notation"][key] = deepcopy(value)
            elif isinstance(value, dict) and isinstance(draft["notation"][key], dict):
                draft["notation"][key].update(deepcopy(value))
        if role == "identity":
            if bundle.get("matching_identity") and "preservation" in source:
                draft["preservation"].update(deepcopy(source["preservation"]))
            recognition = deepcopy(bundle["recognition"])
            for key in ("model_field", "version_field", "cps_version_field"):
                if key in recognition:
                    recognition[key] = field_map[recognition[key]]
            if "required_fields" in recognition:
                recognition["required_fields"] = {field_map[name]: value for name, value in recognition["required_fields"].items()}
            draft["recognition"] = recognition
    return draft


def _check_disjoint_core(image, draft, result):
    ranges = {}
    for role in ("channels", "contacts", "zones"):
        table = draft["tables"][role]
        reader = DefinitionReader(image, draft)
        spans = []
        for segment in table.get("segments", [table]):
            count = reader.expression(segment["count"])
            at = reader.expression(segment["at"])
            if "stride_bytes" in segment:
                stride = integer(segment["stride_bytes"])
                size = (count - 1) * stride + integer(segment.get("logical_span_bytes", stride)) if count else 0
            else:
                size = sum(reader.expression(table["record_bytes"], scope_for(reader, table["fields"], row["address"], row["index"]))
                           for row in result["decoded"]["tables"][role]["records"])
            if size:
                spans.append((at, at + size))
        ranges[role] = spans
    reader = DefinitionReader(image, draft)
    for name in ("radio_name", "radio_id"):
        field = draft["fields"][name]
        at = reader.expression(field["at"])
        ranges[name] = [(at, at + reader.storage_size(field))]
    keys = list(ranges)
    for i, left in enumerate(keys):
        for right in keys[i + 1:]:
            if any(a < d and c < b for a, b in ranges[left] for c, d in ranges[right]):
                raise DefinitionGenerationError(f"Conflicting storage: {left} overlaps {right}")


def infer_definition(data, definitions, max_records=100000):
    """Combine complete, informative layout components; rank valid candidates.

    Matching occurs per identity/channel/zone/contact component, not by choosing
    the definition with the largest partial-match score. Cross-component links
    and the reset profile are checked only after assembling the complete graph.
    """
    if type(max_records) is not int or max_records <= 0:
        raise DefinitionGenerationError("Record limit must be positive")
    if not isinstance(data, (bytes, bytearray)) or not 0 < len(data) <= MAX_PACKAGE_BYTES:
        raise DefinitionGenerationError("Input is empty or exceeds the generation size limit")
    # Stable donor order also makes equivalent-bundle deduplication and the
    # choice of representation independent of filenames/input order.
    definitions = sorted((d for d in definitions if isinstance(d, dict)),
                         key=lambda d: json.dumps(d, sort_keys=True))
    alias_roles = {}
    for definition in definitions:
        try:
            for name, table in definition.get("tables", {}).items():
                role = _table_role(name, table)
                if role:
                    alias_roles.setdefault(name, set()).add(role)
        except (AttributeError, KeyError, TypeError, ValueError):
            continue
    aliases = {name: next(iter(roles)) for name, roles in alias_roles.items() if len(roles) == 1}
    groups = {}
    diagnostics = {role: set() for role in ("identity", "channels", "zones", "contacts")}
    for definition in definitions:
        if not isinstance(definition, dict):
            continue
        try:
            validate_definition(definition)
            roots = {name: role for name, table in definition["tables"].items()
                     if (role := _table_role(name, table))}
            existing = inspect_definition(data, definition, max_records)
            for i, representation in enumerate(definition["representations"]):
                try:
                    image, encoding = decode_representation(data, representation)
                    if any(check["required"] and check["status"] != "match" for check in encoding):
                        continue
                    reader = DefinitionReader(image, definition, max_records)
                    decoded = reader.decode_available_fields_and_tables()
                    digest = hashlib.sha256(image).hexdigest()
                    group = groups.setdefault(digest, {"image": image, "representation": representation,
                                                       "roles": {key: {} for key in diagnostics}})
                    try:
                        bundle = _identity_bundle(definition, reader, roots)
                        group["roles"]["identity"].setdefault(_bundle_signature(bundle, aliases), bundle)
                    except (DefinitionGenerationError, DefinitionError, KeyError, ValueError, TypeError) as error:
                        diagnostics["identity"].add(str(error))
                    for root, role in roots.items():
                        try:
                            bundle = _component_bundle(role, root, definition, reader, decoded, roots,
                                                       existing[i]["status"] == "recognized")
                            group["roles"][role].setdefault(_bundle_signature(bundle, aliases), bundle)
                        except (DefinitionGenerationError, DefinitionError, KeyError, ValueError, TypeError) as error:
                            diagnostics[role].add(str(error))
                except (DefinitionError, KeyError, ValueError, TypeError, UnicodeError, RecursionError):
                    continue
        except (DefinitionError, KeyError, TypeError, ValueError, RecursionError):
            continue
    roles = ("identity", "channels", "zones", "contacts")
    complete = [group for group in groups.values() if all(group["roles"][role] for role in roles)]
    if not complete:
        missing = [role for role in roles if not any(group["roles"][role] for group in groups.values())]
        if not missing:
            raise DefinitionGenerationError("Mappings use incompatible decoded representations; no consistent combination is available")
        details = []
        for role in missing:
            reason = next(iter(sorted(diagnostics[role])), "no informative complete mapping with reset rules")
            details.append(f"{role}: {reason}")
        raise DefinitionGenerationError("Insufficient matching data. " + "; ".join(details))
    successes, failures, tried = {}, set(), 0
    for group in complete:
        candidates = [list(group["roles"][role].values()) for role in roles]
        for bundles in itertools.product(*candidates):
            tried += 1
            if tried > 256:
                raise DefinitionGenerationError("Too many competing mappings to evaluate completely")
            try:
                draft = _assemble_definition(bundles, group["representation"], aliases)
                if draft["tables"]["zones"]["fields"]["members"]["reference"]["table"] != "channels":
                    raise DefinitionGenerationError("Zone membership does not reference the channel mapping")
                if not any(field.get("reference", {}).get("table") == "contacts"
                           for field in draft["tables"]["channels"]["fields"].values()):
                    raise DefinitionGenerationError("Channel-to-contact mapping is not defined")
                checked = inspect_definition(data, draft, max_records)[0]
                if checked["status"] != "recognized":
                    failed = [c["name"] for c in checked.get("checks", []) if c["required"] and c["status"] != "match"]
                    raise DefinitionGenerationError("Combined mappings failed validation: " + ", ".join(failed[:5]))
                _check_disjoint_core(group["image"], draft, checked)
                create_default_codeplug(data, draft)
                signature = tuple(_bundle_signature(bundle, aliases) for bundle in bundles)
                successes.setdefault(signature, draft)
            except (DefinitionGenerationError, GenerationError, DefinitionError, KeyError, TypeError, ValueError, RecursionError) as error:
                failures.add(str(error))
    if not successes:
        detail = next(iter(sorted(failures)), "No consistent reset profile could be established")
        raise DefinitionGenerationError("Cannot establish a consistent definition. " + detail)
    def rank(draft):
        result = inspect_definition(data, draft, max_records)[0]
        return (
            len(draft["fields"]) + sum(
                len(table["fields"]) for table in draft["tables"].values()),
            sum(check.get("required", False) and check["status"] == "match"
                for check in result["checks"]),
            json.dumps(draft, sort_keys=True),
        )

    return max(successes.values(), key=rank)


def _definition_filename(name):
    if not isinstance(name, str) or not name.strip() or any(c in name for c in ("/", "\\", "\0")):
        raise DefinitionGenerationError("Enter a definition name, without a directory path")
    value = name.strip()
    if value.lower().endswith(".wcp"):
        value = value[:-4]
    slug = re.sub(r"[^a-z0-9]+", "-", value.casefold()).strip("-")
    if not slug or len(slug) > 100:
        raise DefinitionGenerationError("Definition name must contain letters/numbers and fit within 100 filename characters")
    return slug + ".wcp"


def prompt_definition_name(draft, directory):
    suggested = draft["recognition"]["model_value"].strip()
    normalized = re.sub(r"[^a-z0-9]", "", suggested.casefold())
    for marker in draft["recognition"].get("required_fields", {}).values():
        if isinstance(marker, str) and marker.isprintable():
            label = re.sub(r"[^a-z0-9]", "", marker.casefold())
            if normalized and normalized in label and len(label) > len(normalized):
                suggested = marker
                normalized = label
    while True:
        print(f"Definition name [{suggested}]: ", end="", file=sys.stderr, flush=True)
        line = sys.stdin.readline()
        if not line:
            raise DefinitionGenerationError("No definition name supplied; input was closed")
        name = line.strip() or suggested
        try:
            filename = _definition_filename(name)
            if (Path(directory) / filename).exists():
                raise DefinitionGenerationError(f"{filename} already exists; enter another name")
            return name
        except DefinitionGenerationError as error:
            print(str(error), file=sys.stderr)


def generation_definition(data, definitions, max_records=100000, default_only=False):
    """Reuse complete matching rules without transferring writes across models."""
    if default_only:
        return infer_definition(data, definitions, max_records)
    candidates = []
    for donor in definitions:
        plan = donor.get("creation", {}).get("construction", {})
        order = plan.get("order")
        if plan.get("version") != 1 or not isinstance(order, list) or not order:
            continue
        if any(not isinstance(label, str) or not isinstance(plan.get(label), dict) for label in order):
            continue
        operations = [plan[label].get("operation", label if label == "validate_target" else None) for label in order]
        supported = {"validate_target", "return_original_if_equal", "decode_representation", "write_global_fields",
                     "branch_if_tables_equal", "rebuild_fixed_table", "replace_variable_table", "relocate_pointers",
                     "rewrite_relative_index", "prepare_overlapping_table", "write_overlapping_fields",
                     "encode_base_representation", "decode_and_compare_target", "remap_references"}
        required = {"validate_target", "return_original_if_equal", "decode_representation",
                    "encode_base_representation", "decode_and_compare_target"}
        if (len(set(order)) != len(order) or not set(operations) <= supported
                or any(operations.count(op) != 1 for op in required)
                or "write_global_fields" not in operations):
            continue
        for result, representation in zip(inspect_definition(data, donor, max_records), donor["representations"]):
            if result["status"] != "recognized":
                continue
            draft = deepcopy(donor)
            draft["creation"]["representation"] = representation["name"]
            resets = draft.setdefault("default_generation", {}).setdefault("globals", {})
            for name in ("power_on_message_1", "power_on_message_2"):
                if name in draft["fields"]:
                    resets[name] = ""
            try:
                create_default_codeplug(data, draft)
            except (GenerationError, DefinitionError):
                continue
            score = (len(draft["fields"]) + sum(len(t["fields"]) for t in draft["tables"].values()),
                     sum(c.get("required", False) and c["status"] == "match" for c in result["checks"]),
                     json.dumps(draft, sort_keys=True))
            candidates.append((score, draft))
    if not candidates:
        draft = infer_definition(data, definitions, max_records)
        compatible = []
        for donor in definitions:
            if donor.get("tables") != draft.get("tables"):
                continue
            if any(draft["fields"].get(name) != field
                   for name, field in donor.get("fields", {}).items()
                   if name in draft["fields"]):
                continue
            creation = donor.get("creation", {})
            plan = creation.get("construction", {})
            if (not isinstance(plan, dict) or plan.get("version") != 1
                    or not isinstance(plan.get("order"), list)
                    or not plan["order"]
                    or not isinstance(creation.get("base_image"), dict)
                    or not isinstance(creation.get("channel_defaults"), dict)):
                continue
            if not any(r.get("decode") == draft["representations"][0].get("decode")
                       for r in donor.get("representations", [])):
                continue
            generated = deepcopy(draft)
            generated["creation"] = {
                "base_image": deepcopy(creation["base_image"]),
                "channel_defaults": deepcopy(creation["channel_defaults"]),
                "construction": deepcopy(plan),
                "representation": draft["representations"][0]["name"],
            }
            references = generated["creation"]["construction"].get("references", {})
            if isinstance(references, dict):
                references["globals"] = [
                    rule for rule in references.get("globals", [])
                    if isinstance(rule, dict) and rule.get("field") in generated["fields"]
                ]
            try:
                create_default_codeplug(data, generated)
            except (GenerationError, DefinitionError, KeyError, TypeError, ValueError):
                continue
            compatible.append((json.dumps(generated["creation"], sort_keys=True), generated))
        if compatible:
            return max(compatible, key=lambda item: item[0])[1]
        raise DefinitionGenerationError(
            "No matching definition supplies executable import/export rules. "
            "Cannot infer safe write rules from decoded bytes alone. "
            "Use --default-only only if you want an inspection/reset-only package; "
            "that package will not support editor import/export.")
    return max(candidates, key=lambda item: item[0])[1]


def undeclared_enum_values(image, definition):
    """Report observed raw enum values without assigning them a meaning."""
    decoded = DefinitionReader(image, definition).decode_available_fields_and_tables()
    warnings = []

    def inspect_fields(prefix, schemas, raw_records):
        for name, schema in schemas.items():
            enum = schema.get("enum")
            if not enum:
                continue
            counts = {}
            for record in raw_records:
                if name not in record:
                    continue
                values = record[name] if isinstance(record[name], list) else [record[name]]
                for value in values:
                    if str(value) not in enum:
                        counts[value] = counts.get(value, 0) + 1
            for value in sorted(counts, key=str):
                warnings.append(f"{prefix}.{name} has undeclared value {value} in {counts[value]} records.")

    inspect_fields("fields", definition["fields"], [decoded["raw_fields"]])
    for name, table in definition["tables"].items():
        inspect_fields(name, table["fields"], [row["raw_values"] for row in decoded["tables"][name]["records"]])
    return warnings


def save_definition_package(directory, name, draft, default):
    """Write one new package exclusively, after validating the entire payload."""
    filename = _definition_filename(name)
    path = Path(directory) / filename
    definition = deepcopy(draft)
    creation = deepcopy(definition.get("creation", {}))
    representation = next((r for r in definition["representations"]
                           if r["name"] == creation.get("representation")), definition["representations"][0])
    extension = representation.get("extension", ".bin")
    if not isinstance(extension, str) or not re.fullmatch(r"\.[A-Za-z0-9]{1,10}", extension):
        extension = ".bin"
    template = "templates/default" + extension
    definition.update(id=filename[:-4], definition_version=2)
    creation.update(template=template, representation=representation["name"],
                    template_bytes=len(default), template_sha256=hashlib.sha256(default).hexdigest())
    creation.setdefault("construction", {"default_generation": "Apply the top-level reset profile to a recognized source. Unmapped settings remain preserved."})
    definition["creation"] = creation
    validate_definition(definition)
    selected = definition["representations"].index(representation)
    if inspect_definition(default, definition)[selected]["status"] != "recognized":
        raise DefinitionGenerationError("Generated template does not match the new definition")
    payload = io.BytesIO()
    with zipfile.ZipFile(payload, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for member, data in (("definition.json", (json.dumps(definition, indent=2) + "\n").encode()), (template, default)):
            info = zipfile.ZipInfo(member, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compresslevel=9)
    created = False
    try:
        with path.open("xb") as stream:
            created = True
            blob = payload.getvalue()
            if stream.write(blob) != len(blob):
                raise OSError("Short write while creating definition package")
    except FileExistsError as error:
        raise DefinitionGenerationError(f"Definition already exists: {path}") from error
    except BaseException:
        if created:
            # The stream is closed; remove only this call's incomplete output.
            path.unlink(missing_ok=True)
        raise
    return path



def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("codeplug", type=Path, help="Codeplug or decoded image to inspect")
    parser.add_argument("--definitions", type=Path, default=Path(__file__).resolve().parents[1] / "definitions",
                        help="Directory searched recursively for .wcp files (default: repository definitions)")
    parser.add_argument("--json", action="store_true", help="Print the full report as JSON to stdout")
    parser.add_argument("--max-records", type=int, default=100000,
                        help="Maximum total records, scanned slots and array elements per candidate (default: 100000)")
    parser.add_argument("--generate-definition", action="store_true",
                        help="Preserve matching executable import/export rules and save a new .wcp package")
    parser.add_argument("--default-only", action="store_true",
                        help="Explicitly generate inspection/reset-only data without editor import/export rules")
    args = parser.parse_args(argv)
    if args.max_records <= 0:
        parser.error("--max-records must be positive")
    if args.default_only and not args.generate_definition:
        parser.error("--default-only requires --generate-definition")
    try:
        data = args.codeplug.read_bytes()
        if not args.definitions.is_dir():
            raise DefinitionError(f"Definition directory not found: {args.definitions}")
        paths = sorted(args.definitions.rglob("*.wcp"))
        if not paths:
            raise DefinitionError(f"No .wcp files found in {args.definitions}")
    except (OSError, DefinitionError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 2
    report = {"input": str(args.codeplug), "bytes": len(data),
              "sha256": hashlib.sha256(data).hexdigest(), "definitions": []}
    valid_definitions = 0
    loaded_definitions = []
    for path in paths:
        entry = {"file": str(path)}
        report["definitions"].append(entry)
        try:
            definition = load_definition(path)
            entry.update(id=definition.get("id", path.stem), recognition=definition.get("recognition", {}),
                         results=inspect_definition(data, definition, args.max_records))
            valid_definitions += 1
            loaded_definitions.append(definition)
        except (OSError, DefinitionError, ValueError, TypeError, KeyError, UnicodeError, RecursionError) as error:
            entry["error"] = str(error)
    if args.generate_definition:
        try:
            draft = generation_definition(data, loaded_definitions, args.max_records, args.default_only)
            extension = args.codeplug.suffix
            if not re.fullmatch(r"\.[A-Za-z0-9]{1,10}", extension):
                raise DefinitionGenerationError(
                    "Input filename must have a valid codeplug extension"
                )
            representation_name = draft.get("creation", {}).get("representation", draft["representations"][0]["name"])
            representation = next(r for r in draft["representations"] if r["name"] == representation_name)
            representation["extension"] = extension
            default = create_default_codeplug(data, draft)
            image, _ = decode_representation(data, representation)
            warnings = undeclared_enum_values(image, draft)
            if warnings:
                report["generation_warnings"] = warnings
                print("Definition is incomplete:", file=sys.stderr)
                for warning in warnings:
                    print(warning, file=sys.stderr)
                print("The definition will still be generated.", file=sys.stderr)
            identity = DefinitionReader(image, draft).globals
            print(f"Compatible mappings found for {draft['recognition']['model_value']}.", file=sys.stderr)
            print(f"Radio name: {identity.get('radio_name')['value']}; radio ID: {identity.get('radio_id')['value']}", file=sys.stderr)
            name = prompt_definition_name(draft, args.definitions)
            if args.default_only:
                print("Generating inspection/reset-only package: editor import/export is unavailable.", file=sys.stderr)
            output = save_definition_package(args.definitions, name, draft, default)
            report["generated_definition"] = str(output)
            print(json.dumps(report, indent=2, ensure_ascii=True) if args.json else
                  text_report(report) + f"\nGenerated definition: {output}")
            return 0
        except (DefinitionGenerationError, GenerationError, DefinitionError, OSError, KeyboardInterrupt) as error:
            print(f"Cannot generate definition: {error or 'cancelled'}. No definition was generated.", file=sys.stderr)
            return 2
    print(json.dumps(report, indent=2, ensure_ascii=True) if args.json else text_report(report))
    if any(r["status"] == "recognized" for d in report["definitions"] for r in d.get("results", [])):
        return 0
    return 1 if valid_definitions else 2


if __name__ == "__main__":
    sys.exit(main())
