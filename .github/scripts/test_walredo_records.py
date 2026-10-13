"""Exercise the CLI helper's record reader without starting PostgreSQL."""
import ast
from pathlib import Path
import struct
import unittest

source = Path(__file__).with_name("walredo_redo_test.py")
function = next(n for n in ast.parse(source.read_text()).body
                if isinstance(n, ast.FunctionDef) and n.name == "read_record")
namespace = {"struct": struct}
exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), "exec"), namespace)
read_record = namespace["read_record"]


class RecordReader(unittest.TestCase):
    def record(self, start, length, segment_size=16384):
        payload = struct.pack("<I", length) + bytes(i % 251 for i in range(length - 4))
        wal = bytearray(start + length + 4096)
        cursor, done = start, 0
        while done < length:
            if cursor != start and cursor % 8192 == 0:
                long = cursor % segment_size == 0
                struct.pack_into("<HHIQI4x", wal, cursor, 0xD119, 1 | (2 if long else 0),
                                 1, cursor, length - done)
                cursor += 40 if long else 24
            size = min(length - done, 8192 - cursor % 8192)
            wal[cursor:cursor + size] = payload[done:done + size]
            cursor += size
            done += size
        return wal, cursor, payload

    def test_alignment_and_continuations(self):
        for start, length in ((48, 27), (8184, 100), (16376, 100), (8184, 17000)):
            with self.subTest(start=start, length=length):
                wal, end, expected = self.record(start, length)
                wal[end:end + 8] = b"padding!"
                self.assertEqual(read_record(lambda p, n: bytes(wal[p:p+n]), start, end), expected)

    def test_invalid_continuation_and_truncation(self):
        wal, end, _ = self.record(8184, 100)
        struct.pack_into("<I", wal, 8192 + 16, 1)
        with self.assertRaises(AssertionError):
            read_record(lambda p, n: bytes(wal[p:p+n]), 8184, end)
        wal, end, _ = self.record(48, 27)
        with self.assertRaises(AssertionError):
            read_record(lambda p, n: bytes(wal[p:p+n]), 48, end - 1)


if __name__ == "__main__":
    unittest.main()
