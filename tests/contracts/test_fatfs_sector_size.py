"""Compile the real disk adapter with a variable-sector FatFs configuration; no media I/O."""
from pathlib import Path
import re
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_variable_sector_sizes(tmp_path):
    config = (ROOT / 'firmware/src/integration/fatfs/ffconf.h').read_text()
    config = re.sub(r'(#define FF_MAX_SS\s+)512', r'\g<1>4096', config)
    (tmp_path / 'ffconf.h').write_text(config)
    harness = tmp_path / 'sector_sizes.c'
    harness.write_text('''
#include <assert.h>
#include "ff.h"
#include "diskio.h"
#include "fatfs_disk.h"
static unsigned size;
static pf_block_result_t geometry(void *ctx, pf_block_geometry_t *g) {
    (void)ctx;
    *g = (pf_block_geometry_t){.sector_count=16, .sector_size=size, .erase_sector_count=1};
    return PF_BLOCK_OK;
}
static pf_block_result_t read_blocks(void *c, uint32_t f, void *b, uint32_t n) { return PF_BLOCK_OK; }
static pf_block_result_t write_blocks(void *c, uint32_t f, const void *b, uint32_t n) { return PF_BLOCK_OK; }
static pf_block_result_t sync_blocks(void *c) { return PF_BLOCK_OK; }
int main(void) {
    const pf_block_device_ops_t ops={.geometry=geometry, .read=read_blocks, .write=write_blocks, .sync=sync_blocks};
    const pf_block_device_t device={.ops=&ops};
    pf_fatfs_disk_bind(&device);
    const unsigned supported[]={512,1024,2048,4096};
    for (unsigned i=0; i<4; ++i) {
        size=supported[i];
        assert(disk_initialize(0)==0);
        WORD reported=0;
        assert(disk_ioctl(0,GET_SECTOR_SIZE,&reported)==RES_OK);
        assert(reported==size);
    }
    const unsigned invalid[]={0,256,768,1536,8192};
    for (unsigned i=0; i<5; ++i) {
        size=invalid[i];
        assert(disk_initialize(0)==STA_NOINIT);
    }
}
''')
    storage = ROOT / 'firmware/src/platform/storage'
    binary = tmp_path / 'sector_sizes'
    subprocess.run(['gcc', '-std=c11', '-I', str(tmp_path),
                    '-I', str(ROOT / 'firmware/third_party/fatfs/source'),
                    '-I', str(ROOT / 'firmware/src/integration/fatfs'), '-I', str(storage),
                    str(harness), str(storage / 'fatfs_disk.c'), str(storage / 'block_device.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
