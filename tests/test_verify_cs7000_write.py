"""Run: python3 -B -m unittest discover -s tests -p test_verify_cs7000_write.py"""

from dataclasses import replace
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "verify_cs7000_write.py"
SPEC = importlib.util.spec_from_file_location("verify_cs7000_write", SCRIPT)
validator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validator
SPEC.loader.exec_module(validator)

MAIN_SIZE = 0x1401  # Deliberately not a multiple of the transfer block size.
MASK = bytes((i * 13 + 7) & 255 for i in range(256))


def encode_image(image):
    return bytes(value ^ MASK[i % 256] for i, value in enumerate(image))


def fixture(count=3):
    """Synthetic data with nonzero unused bytes; contains no private codeplug."""
    used_end = validator.CONTACT_FIRST + count * 16 + 5 if count else 8
    contact_size = max(validator.CONTACT_FIRST + 4096,
                       ((used_end + 2047) // 2048 + 1) * 2048)
    image = bytearray(b"\xA6" * (MAIN_SIZE + contact_size))
    image[0x300:0x500] = bytes(512)
    image[0x1200:0x1200 + len(validator.MODEL)] = validator.MODEL
    image[validator.DATE_AT:validator.DATE_END] = bytes.fromhex("202602231228")
    image[validator.VERSION_AT:validator.VERSION_END] = validator.VERSIONS[0]
    struct.pack_into("<I", image, 0x240, MAIN_SIZE)
    struct.pack_into("<I", image, validator.CONTACT_POINTER, MAIN_SIZE)
    struct.pack_into("<II", image, MAIN_SIZE, count, contact_size)
    for i in range(count):
        struct.pack_into("<I", image, MAIN_SIZE + 8 + i * 4, i + 1)
    return bytes(image), encode_image(image)


def prefix():
    transactions = []
    for device_class in (2, 2, 3, 3, 5):
        transactions.append((0, validator.encode_frame(device_class, 6)))
        response = (validator.encode_frame(0x85, 6, b"\x01" + validator.MODEL
                                          + bytes(256 - len(validator.MODEL)))
                    if device_class == 5 else bytes(265))
        transactions.append((0x80, response))
    transactions.extend([(0, validator.encode_frame(5, 0x14)),
                         (0x80, validator.encode_frame(0x85, 0x14, b"\x01\x00"))])
    return transactions


def block_transactions(plan, image=None, duplicate=False):
    transactions = prefix()
    for block in plan.blocks:
        response = validator.encode_frame(0x85, block.command, b"\x01" + struct.pack(">H", block.index))
        transactions.extend([(0, block.frame(image)), (0x80, response)])
        if duplicate and block.command == 0x2D and block.index == 0:
            transactions.append((0x80, response))
    return transactions


def pcap_block(kind, body):
    body += bytes((-len(body)) % 4)
    length = 12 + len(body)
    return struct.pack("<II", kind, length) + body + struct.pack("<I", length)


def usb_packet(payload, endpoint, stage, status=0):
    return struct.pack("<HQIHBHHBBI", 27, 1, status, 9, stage, 1, 7, endpoint, 3, len(payload)) + payload


def capture_bytes(transactions, tail=None, failed_status=None, bad_tag=None, bad_residue=None,
                  bad_usb=None, omit_csw=None):
    """Wrap independent USB transfers in USBPcap, BOT and pcapng framing."""
    packets = []
    for i, (flags, data) in enumerate(transactions):
        tag = i + 1
        cdb = validator.vendor_cdb(0x28 if flags else 0x2A)
        cbw = b"USBC" + struct.pack("<II", tag, len(data)) + bytes([flags, 0, 16]) + cdb
        packets.append(usb_packet(cbw, 1, 0))
        packets.append(usb_packet(data, 0x81 if flags else 1, 1 if flags else 0,
                                  0xC0000001 if bad_usb == i else 0))
        if omit_csw == i:
            continue
        csw = (b"USBS" + struct.pack("<II", tag + (bad_tag == i), 1 if bad_residue == i else 0)
               + bytes([1 if failed_status == i else 0]))
        packets.append(usb_packet(csw, 0x81, 1))
    if tail:
        if tail == "standard":
            cbw = b"USBC" + struct.pack("<II", 999, 0) + bytes([0, 0, 6]) + bytes(16)
            packets.append(usb_packet(cbw, 1, 0))
            packets.append(usb_packet(b"", 1, 1, 0xC0000011))
        elif tail == "vendor":
            cbw = b"USBC" + struct.pack("<II", 999, 11) + bytes([0x80, 0, 16]) + validator.vendor_cdb(0x28)
            packets.append(usb_packet(cbw, 1, 0))
        else:
            cbw = b"USBC" + struct.pack("<II", 999, 18) + bytes([0x80, 0, 6]) + b"\x03" + bytes(15)
            packets.append(usb_packet(cbw, 1, 0))
    result = [pcap_block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1)),
              pcap_block(1, struct.pack("<HHI", 249, 0, 65535))]
    for i, packet in enumerate(packets):
        result.append(pcap_block(6, struct.pack("<IIIII", 0, 0, i, len(packet), len(packet)) + packet))
    return b"".join(result)


class WriteVerificationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image, cls.encoded = fixture()
        cls.plan = validator.build_write_plan(cls.image)
        cls.transactions = block_transactions(cls.plan)
        cls.capture = capture_bytes(cls.transactions)

    def reject(self, capture=None, codeplug=None, message=None):
        with self.assertRaisesRegex(validator.VerificationError, message or "."):
            validator.verify_capture(capture if capture is not None else self.capture,
                                     codeplug if codeplug is not None else self.encoded)

    def test_valid_capture(self):
        result = validator.verify_capture(self.capture, self.encoded)
        self.assertEqual((result.main_blocks, result.contact_blocks, result.contacts), (3, 2, 3))
        self.assertEqual(result.changed_metadata_bytes, 0)
        self.assertFalse(result.unfinished_scsi_tail)

    def test_plan_indices_extents_and_flags(self):
        main = [b for b in self.plan.blocks if b.command == 0x11]
        contacts = [b for b in self.plan.blocks if b.command == 0x2D]
        self.assertEqual([b.selector for b in main], [0, 0xC001, 0xC002])
        self.assertEqual([b.selector for b in contacts], [0, 2031 | 0x4000])
        self.assertEqual(self.plan.main_size, MAIN_SIZE)
        self.assertEqual(self.plan.contact_extent, validator.CONTACT_FIRST + 3 * 16 + 5)
        self.assertGreater(self.plan.contact_size, self.plan.contact_extent)

    def test_directory_spanning_multiple_blocks(self):
        image, _ = fixture(600)
        plan = validator.build_write_plan(image)
        self.assertEqual([b.index for b in plan.blocks if b.command == 0x2D],
                         [0, 1, 2031, 2032, 2033, 2034, 2035])

    def test_empty_contact_plan_has_one_final_header_block(self):
        image, _ = fixture(0)
        contacts = [b for b in validator.build_write_plan(image).blocks if b.command == 0x2D]
        self.assertEqual([(b.index, b.selector, b.extent) for b in contacts], [(0, 0x4000, 8)])

    def test_duplicate_acknowledgement_and_standard_tail(self):
        capture = capture_bytes(block_transactions(self.plan, duplicate=True), tail="standard")
        result = validator.verify_capture(capture, self.encoded)
        self.assertEqual(result.duplicate_acknowledgements, 1)
        self.assertTrue(result.unfinished_scsi_tail)

    def test_metadata_updates_are_validated_and_reported(self):
        changed = bytearray(self.image)
        changed[validator.DATE_AT:validator.DATE_END] = bytes.fromhex("202610251733")
        changed[validator.VERSION_AT:validator.VERSION_END] = validator.VERSIONS[1]
        capture = capture_bytes(block_transactions(self.plan, changed))
        result = validator.verify_capture(capture, self.encoded)
        self.assertEqual(result.changed_metadata_bytes, 5)
        self.assertEqual(len(result.metadata_updates), 2)
        self.assertIn("2026-10-25 17:33", result.metadata_updates[0])

    def test_main_boundary_bytes_are_not_zero_padded(self):
        main = [b for b in self.plan.blocks if b.command == 0x11][-1]
        boundary = MAIN_SIZE - main.source_offset
        self.assertEqual(main.data[boundary:], self.image[MAIN_SIZE:main.source_offset + 2048])
        self.assertTrue(any(main.data[boundary:]))

    def test_changed_boundary_byte_fails_even_outside_logical_extent(self):
        changed = bytearray(self.image)
        changed[MAIN_SIZE + 100] ^= 1
        self.reject(capture_bytes(block_transactions(self.plan, changed)), message="Payload differs")

    def test_changed_contact_padding_fails(self):
        changed = bytearray(self.image)
        changed[MAIN_SIZE + self.plan.contact_extent + 5] ^= 1
        self.reject(capture_bytes(block_transactions(self.plan, changed)), message="Payload differs")

    def test_unexpected_payload_change_with_correct_checksum_fails(self):
        changed = bytearray(self.image)
        changed[0x100] ^= 1
        self.reject(capture_bytes(block_transactions(self.plan, changed)), message="Payload differs")

    def test_invalid_transmitted_date_fails(self):
        for stamp in ("202613251733", "202602301733", "2026FA251733", "202610252533"):
            with self.subTest(stamp=stamp):
                changed = bytearray(self.image)
                changed[validator.DATE_AT:validator.DATE_END] = bytes.fromhex(stamp)
                self.reject(capture_bytes(block_transactions(self.plan, changed)), message="date/time")

    def test_unsupported_transmitted_version_fails(self):
        changed = bytearray(self.image)
        changed[validator.VERSION_AT:validator.VERSION_END] = b"V9.99.99.99\0"
        self.reject(capture_bytes(block_transactions(self.plan, changed)), message="CPS version")

    def test_bad_frame_checksum_fails(self):
        transactions = list(self.transactions)
        flags, data = transactions[12]
        transactions[12] = flags, data[:-2] + bytes([data[-2] ^ 1]) + data[-1:]
        self.reject(capture_bytes(transactions), message="checksum")

    def test_missing_block_fails(self):
        transactions = self.transactions[:12] + self.transactions[14:]
        self.reject(capture_bytes(transactions), message="Missing or extra")

    def test_extra_block_fails(self):
        self.reject(capture_bytes(self.transactions + self.transactions[-2:]), message="Missing or extra")

    def test_wrong_selector_or_extent_fails(self):
        block = self.plan.blocks[0]
        for altered in (replace(block, selector=0xC001), replace(block, extent=block.extent + 1)):
            with self.subTest(altered=altered.selector):
                transactions = list(self.transactions)
                transactions[12] = 0, altered.frame()
                self.reject(capture_bytes(transactions), message="selection/flags/extent")

    def test_missing_final_marker_fails(self):
        block = self.plan.blocks[-1]
        transactions = list(self.transactions)
        transactions[-2] = 0, replace(block, selector=block.index | 0xC000).frame()
        self.reject(capture_bytes(transactions), message="selection/flags/extent")

    def test_bad_acknowledgement_fails(self):
        transactions = list(self.transactions)
        transactions[-1] = 0x80, validator.encode_frame(0x85, 0x2D, b"\x01\x00\x00")
        self.reject(capture_bytes(transactions), message="acknowledgement")

    def test_failed_acknowledgement_fails(self):
        transactions = list(self.transactions)
        transactions[-1] = 0x80, validator.encode_frame(0x85, 0x2D, b"\x00" + struct.pack(">H", self.plan.blocks[-1].index))
        self.reject(capture_bytes(transactions), message="acknowledgement")

    def test_zero_poll_is_not_a_successful_acknowledgement(self):
        transactions = list(self.transactions)
        transactions[-1] = 0x80, bytes(11)
        self.reject(capture_bytes(transactions), message="acknowledgement")

    def test_read_setup_is_rejected(self):
        transactions = list(self.transactions)
        transactions[10] = 0, validator.encode_frame(5, 0x15)
        self.reject(capture_bytes(transactions), message="identification/setup")

    def test_failed_vendor_transport_fails(self):
        for keyword, message in (("failed_status", "CSW reports failure"),
                                 ("bad_tag", "tag mismatch"), ("bad_residue", "residue"),
                                 ("bad_usb", "USB failure")):
            with self.subTest(keyword=keyword):
                self.reject(capture_bytes(self.transactions, **{keyword: 12}), message=message)

    def test_incomplete_vendor_tail_fails(self):
        self.reject(capture_bytes(self.transactions, tail="vendor"), message="Incomplete vendor")

    def test_missing_final_csw_fails(self):
        self.reject(capture_bytes(self.transactions, omit_csw=len(self.transactions) - 1), message="Incomplete vendor")

    def test_unrecognized_scsi_tail_fails(self):
        self.reject(capture_bytes(self.transactions, tail="sense"), message="SCSI tail")

    def test_truncated_or_malformed_capture_fails(self):
        for data in (b"bad", self.capture[:-1], self.capture[:-8], self.capture[:-4] + bytes(4)):
            with self.subTest(length=len(data)):
                self.reject(data)

    def test_wrong_source_model_or_version_fails(self):
        for at, value, message in ((0x1200, b"X", "radio model"),
                                   (validator.VERSION_AT, b"X", "CPS version")):
            with self.subTest(at=at):
                changed = bytearray(self.image)
                changed[at:at + 1] = value
                self.reject(codeplug=encode_image(changed), message=message)

    def test_invalid_source_pointers_and_counts_fail(self):
        for offset, value in ((validator.CONTACT_POINTER, 0xFFFFFFFF),
                               (0x240, MAIN_SIZE + 1), (MAIN_SIZE + 4, 0),
                               (MAIN_SIZE, 10001), (MAIN_SIZE + 8, 100)):
            with self.subTest(offset=offset):
                changed = bytearray(self.image)
                struct.pack_into("<I", changed, offset, value)
                self.reject(codeplug=encode_image(changed))

    def test_cli_is_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.rdbx"
            capture = Path(directory) / "write.pcapng"
            source.write_bytes(self.encoded)
            capture.write_bytes(capture_bytes(self.transactions, tail="standard"))
            before = [hashlib.sha256(p.read_bytes()).digest() for p in (source, capture)]
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--capture", str(capture),
                                     "--codeplug", str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("3 contacts", result.stdout)
            self.assertIn("USB was not accessed", result.stdout)
            self.assertEqual(before, [hashlib.sha256(p.read_bytes()).digest() for p in (source, capture)])

    def test_cli_failure_is_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "invalid.rdbx"
            source.write_bytes(b"invalid")
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--capture", str(source),
                                     "--codeplug", str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("ERROR:", result.stderr)


if __name__ == "__main__":
    unittest.main()
