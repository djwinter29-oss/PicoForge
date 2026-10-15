/* Executes production MSC -> owner/lock facade -> data-bearing backend seam.
 * This is deliberately not a TinyUSB BOT or physical transport test. */
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#define PF_STORAGE_H
#define PF_STORAGE_WORKER_H
static bool pf_storage_is_mounted(void);
static void pf_storage_forget_filesystem_format(void);
static bool pf_storage_worker_busy(void);
static bool pf_storage_worker_media_absent(void);
static bool pf_storage_worker_recover_card(void);
#include "../../../firmware/src/usb/msc.c"
#include "drivers/sd_card/sd_card.h"
#include "drivers/sd_card/sdio_pio/sdio_pio.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits" /* Existing diskio uint32 width guards. */
#include "../../../firmware/src/integration/fatfs/diskio.c"
#pragma GCC diagnostic pop
pthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t media[8][512];
static unsigned reads, writes, syncs, last_count;
static uint32_t capacity = 8;
static bool initialized, uncertain;
static pf_sd_card_result_t read_error, write_error, sync_error;
static bool pf_storage_is_mounted(void) { return false; }
static void pf_storage_forget_filesystem_format(void) {}
static bool pf_storage_worker_busy(void) { return false; }
static bool pf_storage_worker_media_absent(void) { return false; }
static bool pf_storage_worker_recover_card(void) { return false; }
void __real_mscd_reset(uint8_t p) { (void)p; }
bool __real_mscd_control_xfer_cb(uint8_t p,uint8_t s,const tusb_control_request_t *r)
{ (void)p; (void)s; (void)r; return false; }
bool tud_msc_set_sense(uint8_t l,uint8_t k,uint8_t a,uint8_t q)
{ (void)l; (void)k; (void)a; (void)q; return true; }
pf_sd_card_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *c)
{ (void)c; initialized=true; return PF_SD_CARD_OK; }
pf_sdio_pio_init_stage_t pf_sdio_pio_get_init_stage(void)
{ return (pf_sdio_pio_init_stage_t)99; }
bool pf_sdio_pio_is_initialized(void) { return initialized; }
uint32_t pf_sdio_pio_block_count(void) { return capacity; }
void pf_sdio_pio_get_info(pf_sd_card_info_t *i) { memset(i,0,sizeof(*i)); }
void pf_sdio_pio_confirm_card_removed(void) { initialized=false; }
pf_sd_card_result_t pf_sdio_pio_read_blocks(uint32_t l,void *b,uint32_t n)
{
 assert(pf_sd_card_get_owner()==PF_SD_CARD_OWNER_USB_MSC);
 assert(n && l<8 && n<=8-l && b); ++reads; last_count=n;
 if(read_error) return read_error;
 memcpy(b,media[l],n*512u); return PF_SD_CARD_OK;
}
pf_sd_card_result_t pf_sdio_pio_write_blocks(uint32_t l,const void *b,uint32_t n)
{
 assert(pf_sd_card_get_owner()==PF_SD_CARD_OWNER_USB_MSC);
 assert(n && l<8 && n<=8-l && b); ++writes; last_count=n;
 if(write_error) { uncertain=true; return write_error; }
 memcpy(media[l],b,n*512u); return PF_SD_CARD_OK;
}
pf_sd_card_result_t pf_sdio_pio_sync(void)
{ ++syncs; assert(!uncertain); return sync_error; }
void pf_sd_card_get_profile(pf_sd_card_profile_t *p) { *p=(pf_sd_card_profile_t){0}; }
int main(void)
{
 pf_sd_card_config_t c={.transport=PF_SD_CARD_TRANSPORT_SDIO_PIO};
 assert(pf_sd_card_init(&c)==PF_SD_CARD_OK);
 pf_usb_msc_request(true); pf_usb_msc_task(); assert(msc_enabled);
 uint8_t raw[1538], out[1538]; uint8_t *data=raw+1, *readback=out+1;
 for(unsigned i=0;i<1536;++i) data[i]=(uint8_t)(i*73u+i/512u);
 memset(out,0xa5,sizeof(out));
 assert(tud_msc_write10_cb(0,2,0,data,1536)==1536);
 assert(writes==1 && last_count==3 && syncs==1);
 assert(!memcmp(media[2],data,1536));
 assert(tud_msc_read10_cb(0,2,0,readback,1536)==1536);
 assert(reads==1 && last_count==3 && !memcmp(readback,data,1536));
 assert(out[0]==0xa5 && out[1537]==0xa5);
 uint8_t saved[512]; memcpy(saved,media[3],512);
 assert(tud_msc_write10_cb(0,3,17,data,23)==23);
 assert(!memcmp(media[3],saved,17) && !memcmp(media[3]+17,data,23));
 assert(!memcmp(media[3]+40,saved+40,472));
 assert(tud_msc_read10_cb(0,3,17,readback,23)==23 && !memcmp(readback,data,23));
 /* Exact end-of-card batch succeeds; empty capacity never reaches backend. */
 assert(tud_msc_read10_cb(0,5,0,readback,1536)==1536);
 assert(last_count==3 && !memcmp(readback,media[5],1536));
 unsigned before_empty=reads;
 capacity=0;
 assert(tud_msc_read10_cb(0,0,0,readback,512)==-1 && reads==before_empty);
 capacity=8;
 unsigned r=reads,w=writes,s=syncs;
 assert(tud_msc_read10_cb(0,7,0,readback,1024)==-1);
 assert(tud_msc_write10_cb(0,UINT32_MAX,0,data,512)==-1);
 assert(tud_msc_write10_cb(0,0,0,data,513)==-1);
 assert(tud_msc_write10_cb(0,0,1,data,512)==-1);
 assert(tud_msc_write10_cb(0,0,0,NULL,1024)==-1);
 assert(tud_msc_write10_cb(0,0,0,data,0x80000000u)==-1);
 assert(tud_msc_read10_cb(1,0,0,readback,1024)==-1);
 assert(tud_msc_write10_cb(0,0,512,data,0)==0);
 assert(reads==r && writes==w && syncs==s);
 assert(disk_write(0,data,0,2)==RES_NOTRDY && writes==w);
 assert(disk_read(0,readback,0,2)==RES_NOTRDY && reads==r);
 LBA_t sectors=0; WORD size=0;
 assert(disk_ioctl(0,GET_SECTOR_COUNT,&sectors)==RES_OK && sectors==8);
 assert(disk_ioctl(0,GET_SECTOR_SIZE,&size)==RES_OK && size==512);
 read_error=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_write10_cb(0,0,1,data,10)==-1 && writes==w);
 read_error=PF_SD_CARD_OK; sync_error=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_write10_cb(0,0,0,data,1024)==-1 && writes==w+1 && syncs==s+1);
 assert(transfer_sense_pending && transfer_sense_asc==0x0c);
 sync_error=PF_SD_CARD_OK; write_error=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_write10_cb(0,0,0,data,1024)==-1 && writes==w+2 && syncs==s+1);
 assert(uncertain && transfer_sense_asc==0x0c); /* no sync/replay after uncertain write */
 assert(pf_sd_card_msc_release()==PF_SD_CARD_OK);
 assert(tud_msc_read10_cb(0,0,0,readback,1024)==-1 && reads==r+1);
 puts("MSC batch: production facade, bytes, bounds, RMW, ownership and checked completion passed");
}
