"""Exercise pinned FatFs and the production volume/disk adapters on temporary sparse files, never media."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.fixture(scope="module")
def formatter(tmp_path_factory):
    directory = tmp_path_factory.mktemp("fatfs-format")
    harness = directory / "format.c"
    harness.write_text(r'''
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app/storage/fatfs_volume.h"
#include "middleware/storage/fatfs_disk.h"

static FILE *image;
static uint32_t sectors;
static pf_block_result_t geometry(void *context, pf_block_geometry_t *out)
{
    (void)context;
    *out = (pf_block_geometry_t){.sector_count = sectors, .sector_size = 512, .erase_sector_count = 1};
    return PF_BLOCK_OK;
}
static pf_block_result_t read_blocks(void *context, uint32_t first, void *buffer, uint32_t count)
{
    (void)context;
    if (fseeko(image, (off_t)first * 512, SEEK_SET) != 0) return PF_BLOCK_IO_ERROR;
    return fread(buffer, 512, count, image) == count ? PF_BLOCK_OK : PF_BLOCK_IO_ERROR;
}
static pf_block_result_t write_blocks(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    (void)context;
    if (fseeko(image, (off_t)first * 512, SEEK_SET) != 0) return PF_BLOCK_IO_ERROR;
    return fwrite(buffer, 512, count, image) == count ? PF_BLOCK_OK : PF_BLOCK_IO_ERROR;
}
static pf_block_result_t sync_blocks(void *context)
{
    (void)context;
    return fflush(image) == 0 ? PF_BLOCK_OK : PF_BLOCK_IO_ERROR;
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    sectors = (uint32_t)strtoul(argv[1], NULL, 10);
    const int expected_format = atoi(argv[2]);
    image = tmpfile();
    assert(image != NULL);
    assert(ftruncate(fileno(image), (off_t)sectors * 512) == 0);
    const pf_block_device_ops_t ops = {
        .geometry = geometry, .read = read_blocks, .write = write_blocks, .sync = sync_blocks,
    };
    const pf_block_device_t device = {.ops = &ops};
    if (sectors >= 8388608u) {
        /* Reproduce the old FAT12/16-only failure with the actual pinned implementation. */
        BYTE work[FF_MAX_SS];
        const MKFS_PARM legacy = {.fmt = FM_FAT | FM_SFD, .n_fat = 1, .n_root = 64};
        pf_fatfs_disk_bind(&device);
        assert(f_mkfs("0:", &legacy, work, sizeof(work)) == FR_MKFS_ABORTED);
        pf_fatfs_disk_bind(NULL);
    }
    FRESULT result = pf_fatfs_volume_format(&device);
    printf("sectors=%u format_result=%d\n", sectors, result);
    fflush(stdout);
    assert(result == FR_OK);
    assert(!pf_fatfs_volume_is_mounted());
    BYTE boot[512];
    assert(read_blocks(NULL, 0, boot, 1) == PF_BLOCK_OK);
    assert(boot[510] == 0x55 && boot[511] == 0xaa);
    for (unsigned i = 446; i < 510; ++i) assert(boot[i] == 0); /* No MBR partition entries. */
    assert(pf_fatfs_volume_mount(&device, &result) == PF_FATFS_VOLUME_OK);
    assert(result == FR_OK);
    assert(pf_fatfs_volume_filesystem_format() == expected_format);
    FIL file;
    const char payload[] = "Pinned FatFs round trip";
    UINT transferred;
    assert(f_open(&file, "0:/long format regression.txt", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    assert(f_write(&file, payload, sizeof(payload), &transferred) == FR_OK);
    assert(transferred == sizeof(payload));
    assert(f_close(&file) == FR_OK);
    assert(pf_fatfs_volume_unmount(&result) == PF_FATFS_VOLUME_OK);
    assert(pf_fatfs_volume_mount(&device, &result) == PF_FATFS_VOLUME_OK);
    assert(pf_fatfs_volume_filesystem_format() == expected_format);
    char readback[sizeof(payload)];
    assert(f_open(&file, "0:/long format regression.txt", FA_READ) == FR_OK);
    assert(f_read(&file, readback, sizeof(readback), &transferred) == FR_OK);
    assert(transferred == sizeof(payload) && memcmp(payload, readback, sizeof(payload)) == 0);
    assert(f_close(&file) == FR_OK);
    assert(pf_fatfs_volume_unmount(&result) == PF_FATFS_VOLUME_OK);
    assert(fclose(image) == 0);
}
''')
    source = ROOT / "firmware/src"
    vendor = ROOT / "firmware/third_party/fatfs/source"
    binary = directory / "format"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra",
        "-I", str(source), "-I", str(source / "integration/fatfs"), "-I", str(vendor),
        str(harness), str(source / "app/storage/fatfs_volume.c"),
        str(source / "middleware/storage/fatfs_disk.c"), str(source / "middleware/storage/block_device.c"),
        str(vendor / "ff.c"), str(vendor / "ffunicode.c"), "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("mib,format_id", [
    (1, 1), (16, 2), (1024, 2), (4096, 3), (32768, 3), (131072, 3),
])
def test_format_mount_and_file_round_trip_without_partition_table(formatter, mib, format_id):
    subprocess.run([str(formatter), str(mib * 2048), str(format_id)], check=True)
