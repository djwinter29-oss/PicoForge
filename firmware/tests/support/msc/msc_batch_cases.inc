/* Executes production MSC -> owner/lock facade -> data-bearing backend seam.
 * This is deliberately not a TinyUSB BOT or physical transport test. */
#include "unity.h"
#include <stdbool.h>
#include <pthread.h>
#define PF_STORAGE_WORKER_H
bool pf_fatfs_volume_is_mounted(void);
static void pf_fatfs_volume_forget_filesystem_format(void);
static bool pf_storage_worker_busy(void);
static bool pf_storage_worker_media_absent(void);
static bool pf_storage_worker_recover_media(void);
static bool pf_storage_worker_return_from_usb(void);
static bool pf_storage_worker_unmount_for_usb(void);
#include "app/usb/usb_msc.c"
uint64_t test_time_us;
#include "drivers/sdcard/sd.h"
#if PF_CONFIG_SDCARD_TRANSPORT == 1
#include "drivers/sdcard/sd_spi.h"
#define pf_sd_native_init pf_sd_spi_init
#define pf_sd_native_get_init_stage pf_sd_spi_get_init_stage
#define pf_sd_native_init_stage_t pf_sd_spi_init_stage_t
#define pf_sd_native_is_initialized pf_sd_spi_is_initialized
#define pf_sd_native_block_count pf_sd_spi_block_count
#define pf_sd_native_get_info pf_sd_spi_get_info
#define pf_sd_native_confirm_card_removed pf_sd_spi_confirm_card_removed
#define pf_sd_native_read_blocks pf_sd_spi_read_blocks
#define pf_sd_native_write_blocks pf_sd_spi_write_blocks
#define pf_sd_native_sync pf_sd_spi_sync
#define TEST_TRANSPORT PF_SD_LINK_SPI
#else
#include "drivers/sdcard/sd_native.h"
#define TEST_TRANSPORT PF_SD_LINK_NATIVE_SD
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits" /* Existing diskio uint32 width guards. */
#include "middleware/storage/fatfs_disk.c"
#include "middleware/storage/block_device.c"
#pragma GCC diagnostic pop
pthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t media[8][512];
static unsigned reads, writes, syncs, last_count;
static uint32_t capacity = 8;
static bool initialized, uncertain;
static pf_sd_result_t read_error, write_error, sync_error;
bool pf_fatfs_volume_is_mounted(void) { return false; }
static void pf_fatfs_volume_forget_filesystem_format(void) {}
static bool pf_storage_worker_busy(void) { return false; }
static bool pf_storage_worker_media_absent(void) { return false; }
static bool pf_storage_worker_recover_media(void) { return false; }
static bool pf_storage_worker_return_from_usb(void) { return false; }
static bool pf_storage_worker_unmount_for_usb(void) { return false; }
void __real_mscd_reset(uint8_t p) { (void)p; }
bool __real_mscd_control_xfer_cb(uint8_t p,uint8_t s,const tusb_control_request_t *r)
{ (void)p; (void)s; (void)r; return false; }
bool tud_msc_set_sense(uint8_t l,uint8_t k,uint8_t a,uint8_t q)
{ (void)l; (void)k; (void)a; (void)q; return true; }
pf_sd_result_t pf_sd_native_init(const pf_sd_config_t *c)
{ (void)c; initialized=true; return PF_SD_OK; }
pf_sd_native_init_stage_t pf_sd_native_get_init_stage(void)
{ return (pf_sd_native_init_stage_t)99; }
bool pf_sd_native_is_initialized(void) { return initialized; }
uint32_t pf_sd_native_block_count(void) { return capacity; }
void pf_sd_native_get_info(pf_sd_info_t *i) { memset(i,0,sizeof(*i)); }
void pf_sd_native_confirm_card_removed(void) { initialized=false; }
pf_sd_result_t pf_sd_native_read_blocks(uint32_t l,void *b,uint32_t n)
{
 TEST_ASSERT_TRUE(n && l<8 && n<=8-l && b); ++reads; last_count=n;
 if(read_error) return read_error;
 memcpy(b,media[l],n*512u); return PF_SD_OK;
}
pf_sd_result_t pf_sd_native_write_blocks(uint32_t l,const void *b,uint32_t n)
{
 TEST_ASSERT_TRUE(n && l<8 && n<=8-l && b); ++writes; last_count=n;
 if(write_error) { uncertain=true; return write_error; }
 memcpy(media[l],b,n*512u); return PF_SD_OK;
}
pf_sd_result_t pf_sd_native_sync(void)
{ ++syncs; TEST_ASSERT_FALSE(uncertain); return sync_error; }
pf_sd_link_mode_t pf_sd_board_pins(uint8_t g[6], uint8_t *d) { memset(g,0xff,6); *d=0xff; return PF_SD_LINK_NATIVE_SD; }
static void msc_batch_regression(void)
{
 pf_sd_config_t c={.transport.mode=TEST_TRANSPORT};
 pf_fatfs_disk_bind(pf_storage_firmware_device());
 TEST_ASSERT_TRUE(pf_sd_init(&c)==PF_SD_OK);
 pf_usb_msc_request(true); pf_usb_msc_task(); TEST_ASSERT_TRUE(msc_enabled);
 uint8_t raw[1538], out[1538]; uint8_t *data=raw+1, *readback=out+1;
 for(unsigned i=0;i<1536;++i) data[i]=(uint8_t)(i*73u+i/512u);
 memset(out,0xa5,sizeof(out));
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,2,0,data,1536)==1536);
 TEST_ASSERT_TRUE(writes==1 && last_count==3 && syncs==1);
 TEST_ASSERT_EQUAL_MEMORY(data,media[2],1536);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,2,0,readback,1536)==1536);
 TEST_ASSERT_TRUE(reads==1 && last_count==3 && !memcmp(readback,data,1536));
 TEST_ASSERT_TRUE(out[0]==0xa5 && out[1537]==0xa5);
 uint8_t saved[512]; memcpy(saved,media[3],512);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,3,17,data,23)==23);
 TEST_ASSERT_TRUE(!memcmp(media[3],saved,17) && !memcmp(media[3]+17,data,23));
 TEST_ASSERT_EQUAL_MEMORY(saved+40,media[3]+40,472);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,3,17,readback,23)==23 && !memcmp(readback,data,23));
 /* Exact end-of-card batch succeeds; empty capacity never reaches backend. */
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,5,0,readback,1536)==1536);
 TEST_ASSERT_TRUE(last_count==3 && !memcmp(readback,media[5],1536));
 unsigned before_empty=reads;
 capacity=0;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,readback,512)==-1 && reads==before_empty);
 capacity=8;
 unsigned r=reads,w=writes,s=syncs;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,7,0,readback,1024)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,UINT32_MAX,0,data,512)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,data,513)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,1,data,512)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,NULL,1024)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,data,0x80000000u)==-1);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(1,0,0,readback,1024)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,512,data,0)==0);
 TEST_ASSERT_TRUE(reads==r && writes==w && syncs==s);
 TEST_ASSERT_TRUE(disk_write(0,data,0,2)==RES_NOTRDY && writes==w);
 TEST_ASSERT_TRUE(disk_read(0,readback,0,2)==RES_NOTRDY && reads==r);
 LBA_t sectors=0; WORD size=0;
 TEST_ASSERT_TRUE(disk_ioctl(0,GET_SECTOR_COUNT,&sectors)==RES_OK && sectors==8);
 TEST_ASSERT_TRUE(disk_ioctl(0,GET_SECTOR_SIZE,&size)==RES_OK && size==512);
 read_error=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,1,data,10)==-1 && writes==w);
 read_error=PF_SD_OK; sync_error=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,data,1024)==-1 && writes==w+1 && syncs==s+1);
 TEST_ASSERT_TRUE(transfer_sense_pending && transfer_sense_asc==0x0c);
 sync_error=PF_SD_OK; write_error=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,data,1024)==-1 && writes==w+2 && syncs==s+1);
 TEST_ASSERT_TRUE(uncertain && transfer_sense_asc==0x0c); /* no sync/replay after uncertain write */
 TEST_ASSERT_TRUE(pf_storage_msc_release()==PF_BLOCK_OK);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,readback,1024)==-1 && reads==r+1);
}
