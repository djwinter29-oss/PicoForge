import unittest

from picoforge.device import _matches_vendor_collection


class HidEnumerationTests(unittest.TestCase):
    def test_matches_expected_vendor_usage(self):
        self.assertTrue(_matches_vendor_collection({"usage_page": 0xFF00, "usage": 1}))

    def test_rejects_other_vendor_usage(self):
        self.assertFalse(_matches_vendor_collection({"usage_page": 0x0001, "usage": 1}))

    def test_falls_back_when_hidapi_metadata_is_unavailable(self):
        self.assertTrue(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": ""}
            )
        )

    def test_uses_product_string_when_usage_is_unavailable(self):
        self.assertTrue(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": "PicoForge CDC + HID"}
            )
        )
        self.assertFalse(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": "Other HID device"}
            )
        )
        self.assertTrue(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": "PicoForge CDC + HID + MSC"}
            )
        )


if __name__ == "__main__":
    unittest.main()