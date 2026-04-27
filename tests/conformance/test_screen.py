"""Screen / cursor: SCREEN, MPOS, SHOT, SHOTRECT, SHOTWIN."""
import struct

from fixtures import ConformanceTestCase


class ScreenTests(ConformanceTestCase):
    def test_screen_dimensions(self):
        self.require_caps("SCREEN")
        with self.connect() as c:
            line = c.cmd_ok("SCREEN")
        parts = line.split()
        self.assertEqual(parts[0], "OK")
        w, h = int(parts[1]), int(parts[2])
        self.assertGreater(w, 0)
        self.assertGreater(h, 0)
        # Sanity: no real display is wider than 32K
        self.assertLess(w, 32768)
        self.assertLess(h, 32768)

    def test_mpos_returns_two_ints(self):
        self.require_caps("MPOS")
        self.require_desktop_session()
        with self.connect() as c:
            line = c.cmd_ok("MPOS")
        parts = line.split()
        self.assertEqual(parts[0], "OK")
        int(parts[1])
        int(parts[2])

    def test_shot_returns_valid_bmp(self):
        self.require_caps("SHOT", "SCREEN")
        with self.connect() as c:
            scr = c.cmd_ok("SCREEN").split()
            screen_w, screen_h = int(scr[1]), int(scr[2])
            extras, data = c.cmd_data("SHOT")
        # BITMAPFILEHEADER is 14 bytes, starts with 'BM'
        self.assertGreaterEqual(len(data), 14)
        self.assertEqual(data[:2], b"BM", "SHOT must return a BMP file ('BM' magic)")
        # bfSize at offset 2 should equal len(data)
        bf_size = struct.unpack_from("<I", data, 2)[0]
        self.assertEqual(bf_size, len(data),
                         "BITMAPFILEHEADER.bfSize must match payload length")
        # bfOffBits at offset 10
        bf_off = struct.unpack_from("<I", data, 10)[0]
        self.assertGreaterEqual(bf_off, 14 + 40)
        # BITMAPINFOHEADER follows: width at +18, height at +22, bitcount at +28
        width = struct.unpack_from("<i", data, 18)[0]
        height = struct.unpack_from("<i", data, 22)[0]
        bitcount = struct.unpack_from("<H", data, 28)[0]
        # Height may be negative (top-down) — compare absolute values.
        self.assertEqual(width, screen_w,
                         "BMP width must match SCREEN width")
        self.assertEqual(abs(height), screen_h,
                         "BMP height must match SCREEN height")
        self.assertIn(bitcount, (24, 32),
                      f"BMP bit depth {bitcount} should be 24 or 32")

    def test_shotrect_subset(self):
        self.require_caps("SHOTRECT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOTRECT 0 0 100 80")
        self.assertEqual(data[:2], b"BM")
        width = struct.unpack_from("<i", data, 18)[0]
        height = struct.unpack_from("<i", data, 22)[0]
        self.assertEqual(width, 100)
        self.assertEqual(abs(height), 80)

    def test_shotrect_invalid_size_returns_err(self):
        self.require_caps("SHOTRECT")
        with self.connect() as c:
            self.assertTrue(c.cmd("SHOTRECT 0 0 0 0").startswith("ERR"))
            self.assertTrue(c.cmd("SHOTRECT 0 0 -10 -10").startswith("ERR"))
            # Connection must survive
            self.assertEqual(c.cmd("PING"), "OK pong")
