import json
from pathlib import Path
import subprocess
import threading
import tempfile
import unittest
from unittest.mock import Mock, patch

from picoforge import linux_msc
from picoforge.monitor import DeviceMonitor


class LinuxMscTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.blocks = self.root / "class" / "block"
        self.blocks.mkdir(parents=True)
        patcher = patch.object(linux_msc, "_BLOCKS", self.blocks)
        patcher.start()
        self.addCleanup(patcher.stop)

    def board(self, name="sda", serial="SERIAL", vendor="cafe"):
        usb = self.root / name
        usb.mkdir()
        for key, value in {"idVendor": vendor, "idProduct": "4012", "serial": serial}.items():
            (usb / key).write_text(value)
        disk = usb / "host" / "block" / name
        partition = disk / f"{name}1"
        partition.mkdir(parents=True)
        (partition / "partition").write_text("1")
        (self.blocks / name).symlink_to(disk)
        (self.blocks / f"{name}1").symlink_to(partition)

    def eject(self, devices):
        return [
            json.dumps({"blockdevices": devices}),
            "    Drive: '/org/freedesktop/UDisks2/drives/PicoForge_test'\n",
            "()",
        ]

    @patch.object(linux_msc.sys, "platform", "linux")
    @patch.object(linux_msc, "_board_disk", return_value=Path("/dev/sda"))
    @patch.object(linux_msc, "_run")
    def test_unmounted_disk_ejects_without_forced_unmount_or_poweroff(self, run, disk):
        run.side_effect = self.eject([{"name": "/dev/sda", "mountpoints": [None]}])
        linux_msc.eject_msc("SERIAL")
        disk.assert_called_once_with("SERIAL")
        calls = [call.args[0] for call in run.call_args_list]
        self.assertEqual(calls[0][-1], "/dev/sda")
        self.assertEqual(calls[-1][-2:], [
            "org.freedesktop.UDisks2.Drive.Eject", "{'auth.no_user_interaction': <true>}",
        ])
        self.assertNotIn("power-off", str(calls))

    @patch.object(linux_msc.sys, "platform", "linux")
    @patch.object(linux_msc, "_board_disk", return_value=Path("/dev/sda"))
    @patch.object(linux_msc, "_run")
    def test_mounted_whole_disk_unmounts_before_eject(self, run, disk):
        output = self.eject([{"name": "/dev/sda", "mountpoints": ["/media/home/card"]}])
        run.side_effect = [output[0], "Unmounted", *output[1:]]
        linux_msc.eject_msc("SERIAL")
        self.assertEqual(run.call_args_list[1].args[0], [
            "udisksctl", "unmount", "--block-device", "/dev/sda", "--no-user-interaction",
        ])

    @patch.object(linux_msc.sys, "platform", "linux")
    @patch.object(linux_msc, "_board_disk", return_value=Path("/dev/sda"))
    @patch.object(linux_msc, "_run")
    def test_busy_unmount_aborts_without_eject(self, run, disk):
        run.side_effect = [
            self.eject([{"name": "/dev/sda", "mountpoints": ["/media/card"]}])[0],
            RuntimeError("filesystem busy"),
        ]
        with self.assertRaisesRegex(RuntimeError, "filesystem busy"):
            linux_msc.eject_msc("SERIAL")
        self.assertEqual(run.call_count, 2)

    @patch.object(linux_msc.sys, "platform", "linux")
    @patch.object(linux_msc, "_board_disk", return_value=Path("/dev/sda"))
    @patch.object(linux_msc, "_run")
    def test_unrelated_device_is_rejected_before_unmount(self, run, disk):
        run.return_value = self.eject([{"name": "/dev/nvme0n1", "mountpoints": ["/"]}])[0]
        with self.assertRaisesRegex(RuntimeError, "Unexpected USB block-device topology"):
            linux_msc.eject_msc("SERIAL")
        self.assertEqual(run.call_count, 1)

    @patch.object(linux_msc.subprocess, "run")
    def test_command_failures_are_explicit_and_noninteractive(self, run):
        run.return_value = subprocess.CompletedProcess(["udisksctl"], 1, "", "Not authorized")
        with self.assertRaisesRegex(RuntimeError, "Not authorized"):
            linux_msc._run(["udisksctl", "info"])
        self.assertNotIn("shell", run.call_args.kwargs)
        self.assertEqual(run.call_args.kwargs["timeout"], 30)
        run.side_effect = subprocess.TimeoutExpired(["udisksctl"], 30)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            linux_msc._run(["udisksctl"])
        run.side_effect = FileNotFoundError()
        with self.assertRaisesRegex(RuntimeError, "requires udisksctl"):
            linux_msc._run(["udisksctl"])

    @patch.object(linux_msc.sys, "platform", "linux")
    @patch.object(linux_msc, "_board_disk")
    def test_missing_identity_cannot_select_a_disk(self, disk):
        with self.assertRaisesRegex(RuntimeError, "serial number"):
            linux_msc.eject_msc("")
        disk.assert_not_called()

    def test_identifies_physical_usb_identity_and_exact_serial(self):
        self.board()
        self.board("sdb", serial="OTHER")
        self.board("sdc", vendor="1234")
        self.assertEqual(linux_msc._board_disk("SERIAL"), Path("/dev/sda"))
        with self.assertRaisesRegex(RuntimeError, "found 0"):
            linux_msc._board_disk("MISSING")
        self.board("sdd")
        with self.assertRaisesRegex(RuntimeError, "found 2"):
            linux_msc._board_disk("SERIAL")

    @patch.object(linux_msc.sys, "platform", "linux")
    @patch.object(linux_msc, "_run")
    def test_mounted_partitions_unmount_before_disk_eject(self, run):
        self.board()
        devices = [{"name": "/dev/sda", "mountpoints": [None], "children": [
            {"name": "/dev/sda1", "mountpoints": ["/media/card"]},
        ]}]
        output = self.eject(devices)
        run.side_effect = [output[0], "Unmounted", *output[1:]]
        linux_msc.eject_msc("SERIAL")
        self.assertEqual(run.call_args_list[1].args[0][3], "/dev/sda1")
        self.assertEqual(run.call_args_list[-1].args[0][-2], "org.freedesktop.UDisks2.Drive.Eject")


class MonitorMscTests(unittest.TestCase):
    def setUp(self):
        self.monitor = DeviceMonitor.__new__(DeviceMonitor)
        self.monitor._device_lock = threading.RLock()
        self.monitor._lock = threading.RLock()
        self.monitor._active_client = Mock()
        self.monitor._snapshot = {
            "hardware": {"serial_number": "SERIAL"},
            "msc": {"enabled": True},
        }

    @patch("picoforge.monitor.sys.platform", "linux")
    @patch("picoforge.monitor.eject_msc")
    def test_hid_disable_only_follows_successful_eject(self, eject):
        order = Mock()
        order.attach_mock(eject, "eject")
        order.attach_mock(self.monitor._active_client, "hid")
        self.monitor.set_msc(False)
        self.assertEqual(order.mock_calls[0].args, ("SERIAL",))
        self.assertEqual(order.mock_calls[1].args, (False,))

    @patch("picoforge.monitor.sys.platform", "linux")
    @patch("picoforge.monitor.eject_msc", side_effect=RuntimeError("Not authorized"))
    def test_permission_failure_preserves_host_ownership(self, eject):
        with self.assertRaisesRegex(RuntimeError, "Not authorized"):
            self.monitor.set_msc(False)
        self.monitor._active_client.set_msc.assert_not_called()
        self.assertTrue(self.monitor.snapshot()["msc"]["enabled"])

    @patch("picoforge.monitor.eject_msc")
    def test_enable_never_ejects(self, eject):
        self.monitor.set_msc(True)
        eject.assert_not_called()
        self.monitor._active_client.set_msc.assert_called_once_with(True)

    @patch("picoforge.monitor.eject_msc")
    def test_force_return_bypasses_linux_eject_only_on_capable_enabled_firmware(self, eject):
        with self.assertRaisesRegex(RuntimeError, "force-return support"):
            self.monitor.set_msc(False, force=True)
        self.monitor._active_client.set_msc.assert_not_called()
        self.monitor._snapshot["msc"]["force_return_supported"] = True
        status = self.monitor.set_msc(False, force=True)
        eject.assert_not_called()
        self.monitor._active_client.set_msc.assert_called_once_with(False, force=True)
        self.assertTrue(status["transition_pending"])
        self.assertTrue(status["force_return_supported"])

    @patch("picoforge.monitor.sys.platform", "win32")
    @patch("picoforge.monitor.eject_msc")
    def test_other_platforms_retain_manual_host_eject(self, eject):
        self.monitor.set_msc(False)
        eject.assert_not_called()
        self.monitor._active_client.set_msc.assert_called_once_with(False)


if __name__ == "__main__":
    unittest.main()
