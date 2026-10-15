#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define PF_STORAGE_H
#define PF_STORAGE_WORKER_H
static bool pf_storage_is_mounted(void);
static void pf_storage_forget_filesystem_format(void);
static bool pf_storage_worker_busy(void);
static bool pf_storage_worker_media_absent(void);
#include "../../../firmware/src/usb/msc.c"
static bool initialized=true, worker_busy, mounted;
static unsigned locks, syncs, sense, asc;
static pf_sd_card_result_t sync_result=PF_SD_CARD_OK, owner_result=PF_SD_CARD_OK;
static pf_sd_card_owner_t owner=PF_SD_CARD_OWNER_USB_MSC;
static bool pf_storage_is_mounted(void) { return mounted; }
static void pf_storage_forget_filesystem_format(void) {}
static bool pf_storage_worker_busy(void) { return worker_busy; }
static bool pf_storage_worker_media_absent(void) { return false; }
void pf_sd_card_lock(void) { ++locks; }
void pf_sd_card_unlock(void) { assert(locks); --locks; }
bool pf_sd_card_is_initialized(void) { return initialized; }
uint32_t pf_sd_card_block_count(void) { return 128; }
void pf_sd_card_get_info(pf_sd_card_info_t *i) { *i=(pf_sd_card_info_t){.present=true,.presence_known=true}; }
pf_sd_card_result_t pf_sd_card_set_owner(pf_sd_card_owner_t o) { if(owner_result==PF_SD_CARD_OK) owner=o; return owner_result; }
pf_sd_card_result_t pf_sd_card_msc_sync(void) { ++syncs; return sync_result; }
pf_sd_card_result_t pf_sd_card_msc_read_blocks(uint32_t l, void *b, uint32_t n) { (void)l;(void)b;(void)n; return PF_SD_CARD_OK; }
pf_sd_card_result_t pf_sd_card_msc_write_blocks(uint32_t l, const void *b, uint32_t n) { (void)l;(void)b;(void)n; return PF_SD_CARD_OK; }
bool tud_msc_set_sense(uint8_t l, uint8_t s, uint8_t a, uint8_t q) { (void)l;(void)q; sense=s; asc=a; return true; }
int main(void)
{
 uint8_t cmd[16]={0x35}; msc_enabled=true;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==0 && syncs==1);
 sync_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==3);
 sync_result=PF_SD_CARD_OK;
 cmd[1]=2; assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==5); cmd[1]=0;
 assert(tud_msc_scsi_cb(1,cmd,NULL,0)==-1);
 initialized=false; unsigned failed_before=syncs;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && syncs==failed_before && sense==2);
 initialized=true;
 cmd[0]=0xff; assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && asc==0x20); cmd[0]=0x35;
 uint8_t block[512]={0}; unsigned write_before=syncs;
 assert(tud_msc_write10_cb(0,0,0,block,512)==512);
 tud_msc_write10_complete_cb(0); assert(syncs==write_before);
 assert(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
 unsigned before=syncs;
 assert(!tud_msc_start_stop_cb(0,0,false,true));
 assert(!msc_ejected && syncs==before && asc==0x53);
 pf_usb_msc_request(false); pf_usb_msc_task(); assert(msc_enabled && request_rejected);
 assert(tud_msc_prevent_allow_medium_removal_cb(0,0,0));
 sync_result=PF_SD_CARD_IO_ERROR;
 assert(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected);
 sync_result=PF_SD_CARD_OK;
 assert(tud_msc_start_stop_cb(0,0,false,true) && msc_ejected);
 worker_busy=true; pf_usb_msc_request(false); pf_usb_msc_task(); assert(msc_enabled);
 worker_busy=false; owner_result=PF_SD_CARD_IO_ERROR;
 pf_usb_msc_request(false); pf_usb_msc_task(); assert(msc_enabled && owner==PF_SD_CARD_OWNER_USB_MSC);
 owner_result=PF_SD_CARD_OK;
 pf_usb_msc_request(false); pf_usb_msc_task(); assert(!msc_enabled && owner==PF_SD_CARD_OWNER_FIRMWARE);
 before=syncs; tud_msc_write10_complete_cb(0); assert(syncs==before);
 assert(locks==0 && !mounted);
 puts("Complete production MSC regression passed");
}
