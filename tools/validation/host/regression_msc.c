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
static pf_sd_card_result_t read_result=PF_SD_CARD_OK, write_result=PF_SD_CARD_OK;
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
pf_sd_card_result_t pf_sd_card_msc_acquire(void)
{ if(owner_result==PF_SD_CARD_OK) owner=PF_SD_CARD_OWNER_USB_MSC; return owner_result; }
pf_sd_card_result_t pf_sd_card_msc_release(void)
{ if(owner_result==PF_SD_CARD_OK) owner=PF_SD_CARD_OWNER_FIRMWARE; return owner_result; }
pf_sd_card_result_t pf_sd_card_msc_sync(void) { ++syncs; return sync_result; }
pf_sd_card_result_t pf_sd_card_msc_read_blocks(uint32_t l, void *b, uint32_t n) { (void)l;(void)b;(void)n; return read_result; }
pf_sd_card_result_t pf_sd_card_msc_write_blocks(uint32_t l, const void *b, uint32_t n) { (void)l;(void)b;(void)n; return write_result; }
bool tud_msc_set_sense(uint8_t l, uint8_t s, uint8_t a, uint8_t q) { (void)l;(void)q; sense=s; asc=a; return true; }
/* Callback-boundary model of the pinned source ordering, not a BOT test. */
static void request_sense(unsigned expected_key, unsigned expected_asc)
{
 uint8_t response[18]={0xf0,0,0,0,0,0,0,10};
 response[2]=sense; response[12]=asc;
 assert(tud_msc_request_sense_cb(0,response,sizeof(response))==18);
 assert(response[0]==0xf0 && response[7]==10 && response[2]==expected_key);
 assert(response[12]==expected_asc && response[13]==0);
 tud_msc_set_sense(0,0,0,0); /* TinyUSB clears after application response. */
}
static void transfer_failures(void)
{
 uint8_t block[512]={0};
 for (unsigned partial=0; partial<2; ++partial) {
  unsigned size=partial ? 16 : 512, offset=partial ? 4 : 0;
  read_result=PF_SD_CARD_IO_ERROR;
  assert(tud_msc_read10_cb(0,0,offset,block,size)==-1);
  assert(sense==3 && asc==0x11);
  tud_msc_set_sense(0,2,0x3a,0); /* Unconditional TinyUSB failure overwrite. */
  uint8_t short_response[17]={0};
  assert(tud_msc_request_sense_cb(0,short_response,17)==-1);
  assert(tud_msc_request_sense_cb(0,NULL,18)==-1);
  request_sense(3,0x11); request_sense(0,0);
  /* Partial WRITE10 first reads the sector; report its read failure. */
  if (partial) {
   assert(tud_msc_write10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(3,0x11);
  }
  read_result=PF_SD_CARD_OK; write_result=PF_SD_CARD_IO_ERROR;
  assert(tud_msc_write10_cb(0,0,offset,block,size)==-1);
  assert(sense==3 && asc==0x0c);
  tud_msc_set_sense(0,2,0x3a,0); request_sense(3,0x0c); request_sense(0,0);
  for (unsigned not_configured=0; not_configured<2; ++not_configured) {
   read_result=write_result=not_configured ? PF_SD_CARD_NOT_CONFIGURED : PF_SD_CARD_NOT_READY;
   assert(tud_msc_read10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
   read_result=PF_SD_CARD_OK;
   assert(tud_msc_write10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
  }
  read_result=write_result=PF_SD_CARD_OK;
 }
 /* A later application error supersedes pending transfer sense. */
 read_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 initialized=false;
 assert(!tud_msc_test_unit_ready_cb(0)); request_sense(2,0x3a);
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
 assert(tud_msc_write10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
 initialized=true; read_result=PF_SD_CARD_OK;
 assert(tud_msc_read10_cb(0,0,0,block,512)==512);
 assert(tud_msc_write10_cb(0,0,0,block,512)==512);
 request_sense(0,0);
}
static void sense_lifecycle(void)
{
 uint8_t block[512]={0}, cmd[16]={0x35};
 /* TinyUSB overwrites failed transfer sense; stale app sense must not leak. */
 read_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 read_result=PF_SD_CARD_OK;
 assert(tud_msc_read10_cb(0,0,0,block,512)==512);
 tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_write10_cb(0,0,4,block,16)==-1);
 read_result=PF_SD_CARD_OK;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==0);
 tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 assert(tud_msc_read10_cb(0,128,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x21);
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 assert(tud_msc_write10_cb(0,0,0,NULL,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x24);
 assert(tud_msc_read10_cb(0,0,513,block,0)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x24);
 assert(tud_msc_write10_cb(0,128,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x21);
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_umount_cb(); tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_mount_cb(); tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_SD_CARD_OK;
 /* Built-in TinyUSB commands call these callbacks, not our SCSI hook. */
 for (unsigned c=0; c<3; ++c) {
  uint32_t blocks; uint16_t size;
  read_result=PF_SD_CARD_IO_ERROR;
  assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
  tud_msc_set_sense(0,2,0x3a,0);
  if (c==0) assert(tud_msc_test_unit_ready_cb(0));
  else if (c==1) { tud_msc_capacity_cb(0,&blocks,&size); assert(blocks==128 && size==512); }
  else assert(tud_msc_is_writable_cb(0));
  tud_msc_set_sense(0,0,0,0); request_sense(0,0);
  read_result=PF_SD_CARD_OK;
 }
 /* Failure paths still report fresh NOT READY. */
 initialized=false;
 {
  uint32_t blocks=9; uint16_t size;
  assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
  tud_msc_capacity_cb(0,&blocks,&size); assert(blocks==0);
  assert(sense==2 && asc==0x3a);
 }
 initialized=true;
 /* Queued model dispatch calls the linked driver boundary, not a reset hook. */
 extern void model_enqueue(unsigned);
 extern void model_dispatch(void);
 extern void model_control(uint8_t, const tusb_control_request_t *);
 tusb_control_request_t reset={.bmRequestType=0x21,.bRequest=MSC_REQ_RESET};
 read_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0);
 model_enqueue(1); /* BOT setup */
 model_dispatch(); request_sense(0,0);
 /* Failure already in queue before reset must not erase that reset. */
 model_enqueue(0); model_enqueue(2); /* transfer failure, bus reset */
 model_dispatch(); model_dispatch(); request_sense(0,0);
 /* Reset enqueued while a previous transfer callback is still running. */
 model_enqueue(2);
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0);
 request_sense(3,0x11); /* enqueue is not a processing barrier */
 model_dispatch(); request_sense(0,0);
 /* A fresh failure after handled reset must survive. */
 model_enqueue(2); model_enqueue(0);
 model_dispatch(); model_dispatch(); request_sense(3,0x11);
 /* Rejected requests and non-setup stages do not invalidate cached sense. */
 for (unsigned invalid=0; invalid<4; ++invalid) {
  model_enqueue(0); model_dispatch();
  tusb_control_request_t req=reset;
  if (invalid==0) req.wValue=1;
  if (invalid==1) req.wLength=1;
  if (invalid==2) req.bmRequestType=0x01;
  model_control(invalid==3 ? CONTROL_STAGE_ACK : CONTROL_STAGE_SETUP,&req);
  request_sense(3,0x11);
 }
 read_result=PF_SD_CARD_OK;
}
int main(void)
{
 uint8_t cmd[16]={0x35}; msc_enabled=true;
 transfer_failures();
 sense_lifecycle();
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==0 && syncs==2);
 unsigned range_before=syncs;
 cmd[5]=128; assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && asc==0x21 && syncs==range_before);
 cmd[5]=127; cmd[8]=2;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && syncs==range_before);
 cmd[8]=1; assert(tud_msc_scsi_cb(0,cmd,NULL,0)==0);
 cmd[5]=0; cmd[8]=0;
 sync_result=PF_SD_CARD_IO_ERROR;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==3);
 sync_result=PF_SD_CARD_NOT_READY;
 assert(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==2);
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
 assert(!tud_msc_prevent_allow_medium_removal_cb(0,2,0) && removal_prevented);
 assert(!tud_msc_prevent_allow_medium_removal_cb(0,0,1) && removal_prevented);
 assert(tud_msc_prevent_allow_medium_removal_cb(0,0,0));
 sync_result=PF_SD_CARD_IO_ERROR;
 assert(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected);
 sync_result=PF_SD_CARD_NOT_READY;
 assert(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected && sense==2);
 sync_result=PF_SD_CARD_OK;
 assert(tud_msc_start_stop_cb(0,0,false,true) && msc_ejected);
 before=syncs;
 assert(tud_msc_start_stop_cb(0,0,false,true) && syncs==before);
 initialized=false;
 assert(!tud_msc_start_stop_cb(0,0,true,true) && msc_ejected);
 initialized=true;
 worker_busy=true; pf_usb_msc_request(false); pf_usb_msc_task(); assert(msc_enabled);
 worker_busy=false; owner_result=PF_SD_CARD_IO_ERROR;
 pf_usb_msc_request(false); pf_usb_msc_task(); assert(msc_enabled && owner==PF_SD_CARD_OWNER_USB_MSC);
 owner_result=PF_SD_CARD_OK; transfer_sense_pending=true;
 pf_usb_msc_request(false); pf_usb_msc_task(); assert(!msc_enabled && owner==PF_SD_CARD_OWNER_FIRMWARE && !transfer_sense_pending);
 before=syncs; tud_msc_write10_complete_cb(0); assert(syncs==before);
 assert(locks==0 && !mounted);
 puts("Complete production MSC regression passed");
}
