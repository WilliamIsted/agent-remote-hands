"""Format flag on capture verbs: bmp (default) + png (when advertised)."""
import struct
import time

from client import AgentClient, AgentError
from fixtures import ConformanceTestCase


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


class ImageFormatTests(ConformanceTestCase):
    def _png_supported(self):
        formats = self.info().get("image_formats", "bmp").split(",")
        return "png" in formats

    def test_image_formats_advertised(self):
        info = self.info()
        self.assertIn("image_formats", info,
                      "INFO must include image_formats capability")
        formats = info["image_formats"].split(",")
        self.assertIn("bmp", formats, "bmp is the universal baseline")

    def test_shot_default_is_bmp(self):
        """SHOT with no format token returns BMP for back-compat."""
        self.require_caps("SHOT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOT")
        self.assertEqual(data[:2], b"BM", "SHOT default must be BMP")

    def test_shot_explicit_bmp(self):
        self.require_caps("SHOT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOT bmp")
        self.assertEqual(data[:2], b"BM")

    def test_shot_png(self):
        self.require_caps("SHOT")
        if not self._png_supported():
            self.skipTest("agent does not advertise png")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOT png")
        self.assertEqual(data[:8], PNG_MAGIC, "SHOT png must return PNG bytes")

    def test_shot_png_smaller_than_bmp(self):
        """Sanity check: PNG of the same screen should be smaller than BMP."""
        self.require_caps("SHOT")
        if not self._png_supported():
            self.skipTest("agent does not advertise png")
        with self.connect() as c:
            _, bmp = c.cmd_data("SHOT bmp")
            _, png = c.cmd_data("SHOT png")
        self.assertLess(len(png), len(bmp),
                        f"PNG ({len(png)}) should be smaller than BMP ({len(bmp)})")

    def test_shotrect_png_dimensions(self):
        self.require_caps("SHOTRECT")
        if not self._png_supported():
            self.skipTest("agent does not advertise png")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOTRECT png 0 0 64 48")
        self.assertEqual(data[:8], PNG_MAGIC)
        # PNG IHDR chunk lives at offset 8: 4-byte length, "IHDR", 4-byte width, 4-byte height
        self.assertEqual(data[12:16], b"IHDR")
        width = struct.unpack(">I", data[16:20])[0]
        height = struct.unpack(">I", data[20:24])[0]
        self.assertEqual(width, 64)
        self.assertEqual(height, 48)

    def test_unknown_format_treated_as_bmp(self):
        """Anything that isn't 'bmp' or 'png' is consumed as a positional arg
        for the next field — `SHOT foo` should ERR rather than return an
        invalid response, since 'foo' becomes the start of args (which SHOT
        doesn't take). For SHOTRECT the format-or-coords ambiguity is real,
        so verify it doesn't silently misinterpret."""
        self.require_caps("SHOTRECT")
        with self.connect() as c:
            line = c.cmd("SHOTRECT zzz 0 0 64 48")
            self.assertTrue(line.startswith("ERR"),
                            f"unknown format token should ERR: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_watch_png(self):
        """WATCH png returns PNG-encoded frames."""
        if not self._png_supported():
            self.skipTest("agent does not advertise png")
        self.require_caps("WATCH")
        with self.connect() as c:
            c._send(b"WATCH png 200 800\n")
            line = c._recv_line()
            self.assertTrue(line.startswith("OK "), f"unexpected: {line!r}")
            parts = line.split()
            length = int(parts[-1])
            data = c._recv_n(length)
            self.assertEqual(data[:8], PNG_MAGIC,
                             "WATCH png frames must be PNG")

            # Drain remainder
            deadline = time.time() + 3
            while time.time() < deadline:
                line = c._recv_line()
                if line == "END" or line.startswith("ERR"):
                    break
                if line.startswith("OK "):
                    n = int(line.split()[-1])
                    c._recv_n(n)

    def test_png_unsupported_returns_err(self):
        """If the agent doesn't list png in image_formats, SHOT png must
        ERR rather than silently returning BMP."""
        if self._png_supported():
            self.skipTest("png is supported on this agent")
        self.require_caps("SHOT")
        with self.connect() as c:
            line = c.cmd("SHOT png")
            self.assertTrue(line.startswith("ERR"),
                            f"png should return ERR when unsupported: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")


WEBP_MAGIC_PREFIX = b"RIFF"
WEBP_MAGIC_FORMAT = b"WEBP"


class WebPTests(ConformanceTestCase):
    def _webp_supported(self):
        formats = self.info().get("image_formats", "bmp").split(",")
        return "webp" in formats

    def setUp(self):
        if not self._webp_supported():
            self.skipTest("agent does not advertise webp")

    def test_shot_webp_lossless(self):
        self.require_caps("SHOT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOT webp")
        self.assertGreaterEqual(len(data), 12)
        self.assertEqual(data[:4], WEBP_MAGIC_PREFIX,
                         "WebP must start with 'RIFF'")
        self.assertEqual(data[8:12], WEBP_MAGIC_FORMAT,
                         "WebP must have 'WEBP' at offset 8")

    def test_shot_webp_lossy_quality(self):
        self.require_caps("SHOT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOT webp:85")
        self.assertEqual(data[:4], WEBP_MAGIC_PREFIX)
        self.assertEqual(data[8:12], WEBP_MAGIC_FORMAT)

    def test_webp_lossy_smaller_than_lossless(self):
        """Sanity: lossy WebP at q=50 should be smaller than lossless WebP
        on the same screen content. Degenerate sources (an empty desktop,
        a uniform colour fill) compress to a few hundred bytes lossless,
        and lossy's quant-table overhead exceeds the savings — skip the
        comparison in that case rather than flagging a fake bug."""
        self.require_caps("SHOT")
        with self.connect() as c:
            _, lossless = c.cmd_data("SHOT webp")
            _, lossy = c.cmd_data("SHOT webp:50")
        if len(lossless) < 1024:
            self.skipTest(
                f"degenerate source: lossless WebP is {len(lossless)} bytes; "
                "lossy comparison is meaningless. Run against a real desktop."
            )
        self.assertLess(len(lossy), len(lossless),
                        f"lossy ({len(lossy)}) should be smaller than "
                        f"lossless ({len(lossless)})")

    def test_webp_smaller_than_png(self):
        """For typical screen content, lossy WebP should beat lossless PNG."""
        self.require_caps("SHOT")
        if "png" not in self.info().get("image_formats", "").split(","):
            self.skipTest("png not supported, can't compare")
        with self.connect() as c:
            _, png = c.cmd_data("SHOT png")
            _, webp = c.cmd_data("SHOT webp:85")
        self.assertLess(len(webp), len(png),
                        f"WebP q=85 ({len(webp)}) should beat PNG ({len(png)})")

    def test_shotrect_webp(self):
        self.require_caps("SHOTRECT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOTRECT webp 0 0 64 48")
        self.assertEqual(data[:4], WEBP_MAGIC_PREFIX)
        self.assertEqual(data[8:12], WEBP_MAGIC_FORMAT)

    def test_webp_invalid_quality(self):
        """webp:101 is out of range; agent either rejects or silently falls
        back to BMP. Either is acceptable; what's critical is that the
        agent stays alive and subsequent commands work cleanly."""
        self.require_caps("SHOT")
        with self.connect() as c:
            line = c.cmd("SHOT webp:101")
            # If the agent fell back to BMP, drain the payload so the next
            # command's read isn't reading stale BMP bytes.
            if line.startswith("OK "):
                parts = line.split()
                length = int(parts[-1])
                if length > 0:
                    c._recv_n(length)
            else:
                self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_watch_webp_lossy(self):
        """WATCH webp:75 streams lossy WebP frames."""
        self.require_caps("WATCH")
        with self.connect() as c:
            c._send(b"WATCH webp:75 200 800\n")
            line = c._recv_line()
            self.assertTrue(line.startswith("OK "), f"unexpected: {line!r}")
            length = int(line.split()[-1])
            data = c._recv_n(length)
            self.assertEqual(data[:4], WEBP_MAGIC_PREFIX)
            self.assertEqual(data[8:12], WEBP_MAGIC_FORMAT)
            # Drain
            import time as _time
            deadline = _time.time() + 3
            while _time.time() < deadline:
                line = c._recv_line()
                if line == "END" or line.startswith("ERR"):
                    break
                if line.startswith("OK "):
                    n = int(line.split()[-1])
                    c._recv_n(n)


class CaptureEngineTests(ConformanceTestCase):
    """The `capture` capability flag advertises which capture engine the
    agent uses internally — wgc on modern Windows when WGC initialised
    successfully, gdi (BitBlt) elsewhere. The protocol contract doesn't
    care which engine is used; this test exists to surface the choice in
    CI logs so we notice if WGC stops loading on a target build."""

    def test_capture_advertised(self):
        info = self.info()
        self.assertIn("capture", info, "INFO must include capture engine")
        self.assertIn(info["capture"], ("gdi", "wgc"),
                      f"unknown capture engine: {info['capture']!r}")

    def test_shot_succeeds_under_either_engine(self):
        """Whatever engine, SHOT must produce a valid image."""
        self.require_caps("SHOT")
        with self.connect() as c:
            extras, data = c.cmd_data("SHOT")
        self.assertGreater(len(data), 0)
        self.assertEqual(data[:2], b"BM", "SHOT default is BMP regardless of engine")
