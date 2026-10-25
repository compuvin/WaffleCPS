#!/usr/bin/env python3
"""Compare a codeplug with .wcp definitions and decode their known fields.

Examples:
    python3 tools/inspect_codeplug.py Codeplugs/example.rdbx
    python3 tools/inspect_codeplug.py image.bin --definitions another/directory --json

Only reads files. Does not use .wrd definitions, access USB, or change Waffle.
Blank/erased comparisons and empty tables do not establish a partial match.
JSON check results retain those comparisons with informative set to false.
Exit codes: 0 = recognized, 1 = partial/no match, 2 = input/definition error.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys


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
    def __init__(self, reader, fields, path, base=None):
        self.reader, self.fields, self.path, self.base = reader, fields, path, base
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
        at = (self.expression(field["at"], scope) if scope.base is None
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
        if "not_hex" in validity:
            at = cursor + integer(validity.get("offset", 0))
            size = integer(validity["bytes"])
            patterns = [bytes.fromhex(value) for value in validity["not_hex"]]
            if size <= 0 or not patterns or any(len(pattern) != size for pattern in patterns):
                raise DefinitionError("Invalid selection probe")
            return byte_range(self.image, at, size) not in patterns
        raise DefinitionError("Unsupported record selection rule")

    def decode_available_fields_and_tables(self):
        fields, raw_fields = self.globals.decoded()
        for name, table in self.definition.get("tables", {}).items():
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
                        scope = Scope(self, table["fields"], f"{path}[{index}]", cursor)
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


def validate_definition(definition):
    if definition.get("definition_type") != "codeplug" or definition.get("definition_version") != 1:
        raise DefinitionError("Expected a version 1 codeplug definition")
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
            for error in ([] if compact else result.get("errors", [])[:8]):
                lines.append(f"    Decode issue: {error['path']}: {error['message']}")
            if not compact and len(result.get("errors", [])) > 8:
                lines.append("    Further decode issues are included in --json output.")
            if "error" in result:
                lines.append(f"    {result['error']}")
            if result["status"] == "partial":
                lines.append("    Similarities found; this is not confirmed format compatibility.")
    lines.append("\nRead-only inspection. Use --json for decoded fields, records, and all check results.")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("codeplug", type=Path, help="Codeplug or decoded image to inspect")
    parser.add_argument("--definitions", type=Path, default=Path(__file__).resolve().parents[1] / "definitions",
                        help="Directory searched recursively for .wcp files (default: repository definitions)")
    parser.add_argument("--json", action="store_true", help="Print the full report as JSON to stdout")
    parser.add_argument("--max-records", type=int, default=100000,
                        help="Maximum total records, scanned slots and array elements per candidate (default: 100000)")
    args = parser.parse_args(argv)
    if args.max_records <= 0:
        parser.error("--max-records must be positive")
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
    for path in paths:
        entry = {"file": str(path)}
        report["definitions"].append(entry)
        try:
            definition = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)
            if not isinstance(definition, dict):
                raise DefinitionError("Definition must be a JSON object")
            entry.update(id=definition.get("id", path.stem), recognition=definition.get("recognition", {}),
                         results=inspect_definition(data, definition, args.max_records))
            valid_definitions += 1
        except (OSError, DefinitionError, ValueError, TypeError, KeyError, UnicodeError, RecursionError) as error:
            entry["error"] = str(error)
    print(json.dumps(report, indent=2, ensure_ascii=True) if args.json else text_report(report))
    if any(r["status"] == "recognized" for d in report["definitions"] for r in d.get("results", [])):
        return 0
    return 1 if valid_definitions else 2


if __name__ == "__main__":
    sys.exit(main())
