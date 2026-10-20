"""Exercise the post-link settings reservation guard without hardware."""
from pathlib import Path
import subprocess

import pytest


GUARD = Path(__file__).resolve().parents[2] / "firmware/check_settings_sector.cmake"


@pytest.mark.parametrize("flash_size", [2 * 1024 * 1024, 4 * 1024 * 1024])
@pytest.mark.parametrize("end_delta", [-1, 0, 1])
def test_firmware_cannot_overlap_settings_sector(flash_size, end_delta):
    settings_start = 0x10000000 + flash_size - 4096
    result = subprocess.run(
        ["cmake", f"-DPF_IMAGE_END={settings_start + end_delta}",
         f"-DPF_FLASH_SIZE_BYTES={flash_size}", "-DPF_SECTOR_SIZE=4096", "-P", str(GUARD)],
        capture_output=True, text=True, check=False,
    )
    if end_delta > 0:
        assert result.returncode != 0
        assert "overlaps the reserved settings sector" in result.stderr
    else:
        assert result.returncode == 0, result.stderr
