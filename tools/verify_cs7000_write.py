#!/usr/bin/env python3
"""Verify a CS7000-M17 Plus USB write capture against its source .rdbx.

Uses Python's standard library and files only. No USB access or live write mode.
Generates an independent block plan for the observed layout. Acceptance proves
capture/source consistency, not compatibility of untested radio firmware.

Run:
    python3 tools/verify_cs7000_write.py --capture write.pcapng --codeplug input.rdbx
Exit status: 0 = verified, 1 = invalid/mismatching input, 2 = command-line error.
"""

import argparse
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
import struct
import sys

BLOCK_SIZE = 2048
MAX_INPUT = 256 * 1024 * 1024
MODEL = b"CS7000-M17 PLUS\0"
VERSIONS = (b"V1.02.20.00\0", b"V1.02.21.00\0")
DATE_AT, DATE_END = 0x1218, 0x121E
VERSION_AT, VERSION_END = 0x1298, 0x12A4
CONTACT_POINTER = 0x1264
CONTACT_FIRST = 0x3F7A03


class VerificationError(ValueError):
    """The files do not establish the expected write sequence."""


def require(condition, message):
    if not condition:
        raise VerificationError(message)


def span(data, at, size):
    require(at >= 0 and size >= 0 and at + size <= len(data),
            f"Byte range [{at}, {at + size}) exceeds {len(data)} input bytes")
    return data[at:at + size]


def little(data, at, size=4):
    return int.from_bytes(span(data, at, size), "little")


def encode_frame(device_class, command, payload=b""):
    require(len(payload) <= 65535, "Frame payload too large")
    return (bytes([0xA1, device_class, command]) + struct.pack(">H", len(payload))
            + payload + struct.pack(">H", sum(payload) & 65535) + b"\xA4")


def decode_frame(data):
    require(len(data) >= 8 and data[0] == 0xA1 and data[-1] == 0xA4,
            "Invalid frame delimiters")
    size = int.from_bytes(data[3:5], "big")
    require(size + 8 == len(data), "Frame payload length mismatch")
    payload = data[5:-3]
    require(sum(payload) & 65535 == int.from_bytes(data[-3:-1], "big"),
            "Frame checksum mismatch")
    return data[1], data[2], payload


def decode_codeplug(encoded):
    require(VERSION_END <= len(encoded) <= MAX_INPUT, "Invalid codeplug file size")
    mask = encoded[0x300:0x400]
    require(mask == encoded[0x400:0x500], "Unsupported codeplug XOR mask layout")
    image = bytes(value ^ mask[i % 256] for i, value in enumerate(encoded))
    require(image[0x1200:0x1200 + len(MODEL)] == MODEL, "Unsupported radio model")
    require(image[VERSION_AT:VERSION_END] in VERSIONS, "Unsupported source CPS version")
    return image


@dataclass(frozen=True)
class WriteBlock:
    command: int
    selector: int
    extent: int
    source_offset: int
    data: bytes

    @property
    def index(self):
        return self.selector & 0x3FFF

    def frame(self, image=None):
        data = self.data if image is None else span(image, self.source_offset, BLOCK_SIZE)
        payload = struct.pack(">HIH", self.selector, self.extent, BLOCK_SIZE) + data
        return encode_frame(0x05, self.command, payload)


@dataclass(frozen=True)
class WritePlan:
    main_size: int
    contact_size: int
    contact_extent: int
    contact_count: int
    blocks: tuple


def build_write_plan(image):
    require(VERSION_END <= len(image) <= MAX_INPUT, "Invalid decoded image size")
    require(image[0x1200:0x1200 + len(MODEL)] == MODEL, "Unsupported radio model")
    require(image[VERSION_AT:VERSION_END] in VERSIONS, "Unsupported source CPS version")
    main = little(image, CONTACT_POINTER)
    require(VERSION_END <= main <= 0x4000 * BLOCK_SIZE, "Invalid main-region extent")
    require(little(image, 0x240) == main, "Contact directory pointer mismatch")
    count, size = little(image, main), little(image, main + 4)
    require(8 <= size <= 0x4000 * BLOCK_SIZE and main + size == len(image),
            "Contact region does not match the image size")
    require(count <= 10000, "Contact count exceeds the CS7000 limit")
    directory_end = 8 + count * 4
    require(directory_end <= CONTACT_FIRST and directory_end <= size,
            "Contact directory exceeds the region")
    for at in range(main + 8, main + directory_end, 4):
        require(1 <= little(image, at) <= count, "Invalid contact directory reference")
    # Each logical contact spans 21 bytes, with a 16-byte physical stride.
    records_end = CONTACT_FIRST + (count - 1) * 16 + 21 if count else 0
    extent = max(directory_end, records_end)
    require(extent <= size, "Contact records exceed the stored region")
    contact_indices = set(range((directory_end + BLOCK_SIZE - 1) // BLOCK_SIZE))
    if count:
        contact_indices.update(range(CONTACT_FIRST // BLOCK_SIZE,
                                     (records_end + BLOCK_SIZE - 1) // BLOCK_SIZE))
    blocks = []
    for command, base, length, indices in (
        (0x11, 0, main, range((main + BLOCK_SIZE - 1) // BLOCK_SIZE)),
        (0x2D, main, extent, sorted(contact_indices)),
    ):
        indices = tuple(indices)
        for position, index in enumerate(indices):
            selector = 0 if position == 0 else index | 0xC000
            if command == 0x2D and position == len(indices) - 1:
                selector = index | 0x4000
            offset = base + index * BLOCK_SIZE
            # Main's final block overlaps the following source region. Do not
            # synthesize zero padding or truncate bytes at a logical boundary.
            block = span(image, offset, BLOCK_SIZE)
            blocks.append(WriteBlock(command, selector, length, offset, block))
    return WritePlan(main, size, extent, count, tuple(blocks))


@dataclass
class Transaction:
    packet: int
    tag: int
    size: int
    flags: int
    cdb: bytes
    data: bytearray = field(default_factory=bytearray)
    end_packet: int = 0

    @property
    def vendor(self):
        return self.cdb[0] == 0xFF


def vendor_cdb(opcode):
    return bytes([0xFF, opcode]) + bytes(12) + b"GP"


@dataclass
class Exchange:
    packet: int
    request: bytes
    polls: list = field(default_factory=list)
    responses: list = field(default_factory=list)


@dataclass
class Capture:
    packets: int
    vendor_transactions: int
    exchanges: list
    unfinished_tail: bool


def parse_capture(data):
    require(28 <= len(data) <= MAX_INPUT, "Invalid capture size")
    require(data[:4] == b"\x0A\x0D\x0D\x0A" and data[8:12] == b"\x4D\x3C\x2B\x1A",
            "Expected a little-endian pcapng capture")
    links, transactions = [], []
    active = radio = None
    offset = packet = 0
    while offset < len(data):
        block_type = little(data, offset)
        length = little(data, offset + 4)
        require(length >= 12 and length % 4 == 0, "Malformed pcapng block length")
        block = span(data, offset, length)
        require(little(block, length - 4) == length, "pcapng block trailer mismatch")
        if block_type == 0x0A0D0D0A:
            require(offset == 0, "Multiple pcapng sections are unsupported")
        elif block_type == 1:
            require(length >= 20, "Truncated interface descriptor")
            links.append(little(block, 8, 2))
        elif block_type == 6:
            packet += 1
            require(length >= 32, "Truncated enhanced packet block")
            interface, captured, original = (little(block, at) for at in (8, 20, 24))
            require(interface < len(links), "Unknown capture interface")
            require(captured <= length - 32 and captured == original,
                    "Truncated captured packet")
            if links[interface] == 249:
                p = span(block, 28, captured)
                require(len(p) >= 27, "Truncated USBPcap header")
                header = little(p, 0, 2)
                require(27 <= header <= len(p) and little(p, 23) == len(p) - header,
                        "USBPcap payload length mismatch")
                bus, device = little(p, 17, 2), little(p, 19, 2)
                ep, stage, transfer = p[21], p[16] & 1, p[22]
                payload = p[header:]
                cbw = len(payload) == 31 and payload[:4] == b"USBC"
                if cbw and payload[15] == 0xFF and radio is None:
                    radio = bus, device
                if (bus, device) == radio and transfer == 3 and ep in (1, 0x81):
                    status = little(p, 10)
                    if status and (active and active.vendor or cbw and payload[15] == 0xFF):
                        raise VerificationError(f"USB failure at packet {packet}: 0x{status:08X}")
                    relevant = (ep == 1 and stage == 0) or (ep == 0x81 and stage == 1)
                    if relevant and payload and not status:
                        if cbw:
                            require(active is None and ep == 1, "Overlapping BOT transactions")
                            require(1 <= payload[14] <= 16 and payload[13] == 0
                                    and payload[12] in (0, 0x80), "Unsupported CBW fields")
                            active = Transaction(packet, little(payload, 4), little(payload, 8),
                                                 payload[12], payload[15:15 + payload[14]])
                            require(active.size <= MAX_INPUT, "Excessive BOT transfer length")
                        elif len(payload) == 13 and payload[:4] == b"USBS":
                            require(active is not None and ep == 0x81, "CSW without CBW")
                            require(little(payload, 4) == active.tag, "CSW tag mismatch")
                            residue, result = little(payload, 8), payload[12]
                            require(result <= 2 and len(active.data) + residue == active.size,
                                    "CSW residue or transfer length mismatch")
                            if active.vendor:
                                require(result == 0 and residue == 0, "Vendor CSW reports failure")
                                require(active.cdb == vendor_cdb(0x28 if active.flags else 0x2A),
                                        "Unrecognized vendor CDB")
                                active.end_packet = packet
                                transactions.append(active)
                            active = None
                        else:
                            require(active is not None and ep == (0x81 if active.flags else 1),
                                    "Unexpected BOT data phase")
                            active.data.extend(payload)
                            require(len(active.data) <= active.size, "Excess BOT data")
        offset += length
    require(transactions, "No completed vendor transactions")
    unfinished = active is not None
    if unfinished:
        require(not active.vendor, "Incomplete vendor transaction")
        require(active.packet > transactions[-1].end_packet and active.cdb == bytes(6)
                and active.size == 0 and active.flags == 0 and not active.data,
                "Unrecognized unfinished SCSI tail")
    exchanges = []
    for transaction in transactions:
        payload = bytes(transaction.data)
        if not transaction.flags:
            decode_frame(payload)
            exchanges.append(Exchange(transaction.packet, payload))
        else:
            require(exchanges, "Response before request")
            exchanges[-1].polls.append(transaction.size)
            if any(payload):
                decode_frame(payload)
                exchanges[-1].responses.append(payload)
    return Capture(packet, len(transactions), exchanges, unfinished)


def date_text(raw, allow_unset=False):
    if allow_unset and raw == bytes(6):
        return "unset"
    require(len(raw) == 6 and all((b >> 4) <= 9 and (b & 15) <= 9 for b in raw),
            "Invalid BCD date/time update")
    values = [(b >> 4) * 10 + (b & 15) for b in raw]
    try:
        return datetime(values[0] * 100 + values[1], *values[2:]).strftime("%Y-%m-%d %H:%M")
    except ValueError as error:
        raise VerificationError("Invalid BCD date/time update") from error


@dataclass(frozen=True)
class Verification:
    packets: int
    vendor_transactions: int
    main_blocks: int
    contact_blocks: int
    contacts: int
    duplicate_acknowledgements: int
    metadata_updates: tuple
    changed_metadata_bytes: int
    unfinished_scsi_tail: bool


def verify_capture(capture_data, codeplug_data):
    image = decode_codeplug(codeplug_data)
    plan = build_write_plan(image)
    capture = parse_capture(capture_data)
    exchanges = capture.exchanges
    require(len(exchanges) == 6 + len(plan.blocks), "Missing or extra write requests")
    for i, device_class in enumerate((2, 2, 3, 3, 5, 5)):
        e = exchanges[i]
        command = 0x14 if i == 5 else 0x06
        require(e.request == encode_frame(device_class, command), "Unexpected identification/setup sequence")
        require(e.polls and all(n == (10 if i == 5 else 265) for n in e.polls),
                "Unexpected identification/setup poll length")
        if i < 4:
            require(not e.responses, "Unrecognized identification probe response")
        elif i == 4:
            require(len(e.responses) == 1, "Missing or duplicate model identification")
            c, cmd, payload = decode_frame(e.responses[0])
            require(c == 0x85 and cmd == 6 and len(payload) == 257 and payload[0] == 1
                    and payload[1:1 + len(MODEL)] == MODEL, "Radio model identification mismatch")
        else:
            require(e.responses == [encode_frame(0x85, 0x14, b"\x01\x00")],
                    "Write-session setup did not succeed")
    main_data = []
    duplicates = 0
    for block, exchange in zip(plan.blocks, exchanges[6:]):
        c, command, payload = decode_frame(exchange.request)
        require(c == 5 and command == block.command and len(payload) == 2056,
                "Unexpected block-write command or length")
        selector, extent, length = struct.unpack(">HIH", payload[:8])
        require((selector, extent, length) == (block.selector, block.extent, BLOCK_SIZE),
                f"Block selection/flags/extent mismatch for command 0x{block.command:02X}, block {block.index}")
        require(exchange.polls and all(n == 11 for n in exchange.polls), "Unexpected acknowledgement poll length")
        acknowledgement = encode_frame(0x85, command, b"\x01" + struct.pack(">H", block.index))
        require(exchange.responses and all(r == acknowledgement for r in exchange.responses),
                f"Missing or invalid acknowledgement for block {block.index}")
        duplicates += len(exchange.responses) - 1
        if command == 0x11:
            main_data.append(payload[8:])
    sent_main = b"".join(main_data)
    source_version, target_version = image[VERSION_AT:VERSION_END], sent_main[VERSION_AT:VERSION_END]
    require(target_version in VERSIONS, "Unsupported transmitted CPS version")
    source_date, target_date = image[DATE_AT:DATE_END], sent_main[DATE_AT:DATE_END]
    # Validate the transmitted stamp even when it did not change. Preserve an
    # unchanged unset factory stamp; a changed stamp must be a valid date/time.
    target_date_text = date_text(target_date, allow_unset=source_date == target_date)
    updates = []
    if source_date != target_date:
        updates.append(f"Date/time: {date_text(source_date, allow_unset=True)} -> {target_date_text}")
    if source_version != target_version:
        updates.append(f"CPS version: {source_version[:-1].decode()} -> {target_version[:-1].decode()}")
    normalized = bytearray(image)
    normalized[DATE_AT:DATE_END] = target_date
    normalized[VERSION_AT:VERSION_END] = target_version
    metadata_bytes = sum(a != b for a, b in zip(image[DATE_AT:DATE_END], target_date))
    metadata_bytes += sum(a != b for a, b in zip(source_version, target_version))
    for block, exchange in zip(plan.blocks, exchanges[6:]):
        expected = block.frame(normalized)
        if exchange.request != expected:
            actual_data = exchange.request[13:-3]
            expected_data = normalized[block.source_offset:block.source_offset + BLOCK_SIZE]
            different = [i for i, (a, b) in enumerate(zip(actual_data, expected_data)) if a != b]
            require(not different,
                    f"Payload differs at image offset 0x{block.source_offset + different[0]:X} "
                    f"(command 0x{block.command:02X}, block {block.index})" if different else "Generated frame mismatch")
            raise VerificationError("Generated frame mismatch")
    return Verification(capture.packets, capture.vendor_transactions, len(main_data),
                        len(plan.blocks) - len(main_data), plan.contact_count, duplicates,
                        tuple(updates), metadata_bytes, capture.unfinished_tail)


def read_input(path):
    with path.open("rb") as stream:
        data = stream.read(MAX_INPUT + 1)
    require(len(data) <= MAX_INPUT, f"Input exceeds {MAX_INPUT} bytes: {path}")
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--capture", required=True, type=Path, help="USBPcap .pcapng of a vendor write")
    parser.add_argument("--codeplug", required=True, type=Path, help="The exact source .rdbx")
    args = parser.parse_args(argv)
    try:
        result = verify_capture(read_input(args.capture), read_input(args.codeplug))
    except (OSError, VerificationError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Verified CS7000-M17 PLUS: {result.main_blocks} main blocks, "
          f"{result.contact_blocks} contact blocks, {result.contacts} contacts.")
    print(f"{result.vendor_transactions} completed vendor transactions; "
          f"{result.duplicate_acknowledgements} duplicate acknowledgement(s).")
    print(f"Unexpected payload differences: 0. Metadata bytes changed: {result.changed_metadata_bytes}.")
    for update in result.metadata_updates:
        print(f"  {update}")
    if result.unfinished_scsi_tail:
        print("The later unfinished TEST UNIT READY probe is separate from the completed writes.")
    print("Offline verification only; files were read and USB was not accessed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
