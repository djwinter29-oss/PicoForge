import unittest

from picoforge.dashboard.freshness import calculate_report_freshness


class ReportFreshnessTests(unittest.TestCase):
    def test_freshness_marks_report_age_and_support_independently(self):
        result = calculate_report_freshness(
            {3: 10.0, 10: 9.0, 11: 9.0},
            now=10.5,
            connected=True,
            hardware_supported=True,
            self_test_supported=False,
        )

        self.assertEqual(result["3"], {
            "supported": True, "available": True, "stale": False, "age_ms": 500,
        })
        self.assertEqual(result["10"], {
            "supported": True, "available": True, "stale": True, "age_ms": 1500,
        })
        self.assertEqual(result["11"], {
            "supported": False, "available": True, "stale": True, "age_ms": 1500,
        })

    def test_missing_reports_are_unavailable_not_stale(self):
        result = calculate_report_freshness({}, 20.0, False, None, None)

        self.assertTrue(all(not report["available"] and not report["stale"]
                            for report in result.values()))
        self.assertTrue(all(report["supported"] is None for report in result.values()))
