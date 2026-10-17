#include <stdbool.h>
#include "unity.h"

TEST_INCLUDE_PATH("firmware/tests/support/msc")
TEST_SOURCE_FILE("firmware/tests/support/msc/msc_reset_model.c")
TEST_SOURCE_FILE("firmware/tests/support/msc/msc_reset_queue.c")
#define PF_STORAGE_H
#define PF_STORAGE_WORKER_H
static bool pf_storage_is_mounted(void);
static void pf_storage_forget_filesystem_format(void);
static bool pf_storage_worker_busy(void);
static bool pf_storage_worker_media_absent(void);
static bool pf_storage_worker_recover_card(void);
#include "../../src/usb/msc.c"
static bool initialized=true, worker_busy, mounted;
static unsigned locks, syncs, sense, asc;
static pf_sd_result_t read_result=PF_SD_OK, write_result=PF_SD_OK;
static pf_sd_result_t sync_result=PF_SD_OK, owner_result=PF_SD_OK;
static pf_sd_owner_t owner=PF_SD_OWNER_USB_MSC;
static bool pf_storage_is_mounted(void) { return mounted; }
static void pf_storage_forget_filesystem_format(void) {}
static bool pf_storage_worker_busy(void) { return worker_busy; }
static bool pf_storage_worker_media_absent(void) { return false; }
static bool pf_storage_worker_recover_card(void) { return false; }
bool pf_sd_try_lock(void) { ++locks; return true; }
void pf_sd_lock(void) { ++locks; }
void pf_sd_unlock(void) { TEST_ASSERT_TRUE(locks); --locks; }
bool pf_sd_is_initialized(void) { return initialized; }
uint32_t pf_sd_block_count(void) { return 128; }
void pf_sd_get_info(pf_sd_info_t *i) { *i=(pf_sd_info_t){.present=true,.presence_known=true}; }
pf_sd_result_t pf_sd_msc_acquire(void)
{ if(owner_result==PF_SD_OK) owner=PF_SD_OWNER_USB_MSC; return owner_result; }
pf_sd_result_t pf_sd_msc_release(void)
{ if(owner_result==PF_SD_OK) owner=PF_SD_OWNER_FIRMWARE; return owner_result; }
pf_sd_result_t pf_sd_msc_sync(void) { ++syncs; return sync_result; }
pf_sd_result_t pf_sd_msc_read_blocks(uint32_t l, void *b, uint32_t n) { (void)l;(void)b;(void)n; return read_result; }
pf_sd_result_t pf_sd_msc_write_blocks(uint32_t l, const void *b, uint32_t n) { (void)l;(void)b;(void)n; return write_result; }
bool tud_msc_set_sense(uint8_t l, uint8_t s, uint8_t a, uint8_t q) { (void)l;(void)q; sense=s; asc=a; return true; }
/* Callback-boundary model of the pinned source ordering, not a BOT test. */
static void request_sense(unsigned expected_key, unsigned expected_asc)
{
 uint8_t response[18]={0xf0,0,0,0,0,0,0,10};
 response[2]=sense; response[12]=asc;
 TEST_ASSERT_TRUE(tud_msc_request_sense_cb(0,response,sizeof(response))==18);
 TEST_ASSERT_TRUE(response[0]==0xf0 && response[7]==10 && response[2]==expected_key);
 TEST_ASSERT_TRUE(response[12]==expected_asc && response[13]==0);
 tud_msc_set_sense(0,0,0,0); /* TinyUSB clears after application response. */
}
static void transfer_failures(void)
{
 uint8_t block[512]={0};
 for (unsigned partial=0; partial<2; ++partial) {
  unsigned size=partial ? 16 : 512, offset=partial ? 4 : 0;
  read_result=PF_SD_IO_ERROR;
  TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,offset,block,size)==-1);
  TEST_ASSERT_TRUE(sense==3 && asc==0x11);
  tud_msc_set_sense(0,2,0x3a,0); /* Unconditional TinyUSB failure overwrite. */
  uint8_t short_response[17]={0};
  TEST_ASSERT_TRUE(tud_msc_request_sense_cb(0,short_response,17)==-1);
  TEST_ASSERT_TRUE(tud_msc_request_sense_cb(0,NULL,18)==-1);
  request_sense(3,0x11); request_sense(0,0);
  /* Partial WRITE10 first reads the sector; report its read failure. */
  if (partial) {
   TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(3,0x11);
  }
  read_result=PF_SD_OK; write_result=PF_SD_IO_ERROR;
  TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,offset,block,size)==-1);
  TEST_ASSERT_TRUE(sense==3 && asc==0x0c);
  tud_msc_set_sense(0,2,0x3a,0); request_sense(3,0x0c); request_sense(0,0);
  for (unsigned not_configured=0; not_configured<2; ++not_configured) {
   read_result=write_result=not_configured ? PF_SD_NOT_CONFIGURED : PF_SD_NOT_READY;
   TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
   read_result=PF_SD_OK;
   TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
  }
  read_result=write_result=PF_SD_OK;
 }
 /* A later application error supersedes pending transfer sense. */
 read_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 initialized=false;
 TEST_ASSERT_TRUE(!tud_msc_test_unit_ready_cb(0)); request_sense(2,0x3a);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
 initialized=true; read_result=PF_SD_OK;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==512);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,block,512)==512);
 request_sense(0,0);
}
static void sense_lifecycle(void)
{
 uint8_t block[512]={0}, cmd[16]={0x35};
 /* TinyUSB overwrites failed transfer sense; stale app sense must not leak. */
 read_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 read_result=PF_SD_OK;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==512);
 tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,4,block,16)==-1);
 read_result=PF_SD_OK;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==0);
 tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,128,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x21);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,NULL,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x24);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,513,block,0)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x24);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,128,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(5,0x21);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_umount_cb(); tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_mount_cb(); tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_SD_OK;
 /* Built-in TinyUSB commands call these callbacks, not our SCSI hook. */
 for (unsigned c=0; c<3; ++c) {
  uint32_t blocks; uint16_t size;
  read_result=PF_SD_IO_ERROR;
  TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
  tud_msc_set_sense(0,2,0x3a,0);
  if (c==0) TEST_ASSERT_TRUE(tud_msc_test_unit_ready_cb(0));
  else if (c==1) { tud_msc_capacity_cb(0,&blocks,&size); TEST_ASSERT_TRUE(blocks==128 && size==512); }
  else TEST_ASSERT_TRUE(tud_msc_is_writable_cb(0));
  tud_msc_set_sense(0,0,0,0); request_sense(0,0);
  read_result=PF_SD_OK;
 }
 /* Failure paths still report fresh NOT READY. */
 initialized=false;
 {
  uint32_t blocks=9; uint16_t size;
  TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
  tud_msc_capacity_cb(0,&blocks,&size); TEST_ASSERT_TRUE(blocks==0);
  TEST_ASSERT_TRUE(sense==2 && asc==0x3a);
 }
 initialized=true;
 /* Queued model dispatch calls the linked driver boundary, not a reset hook. */
 extern void model_enqueue(unsigned);
 extern void model_dispatch(void);
 extern void model_control(uint8_t, const tusb_control_request_t *);
 tusb_control_request_t reset={.bmRequestType=0x21,.bRequest=MSC_REQ_RESET};
 read_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0);
 model_enqueue(1); /* BOT setup */
 model_dispatch(); request_sense(0,0);
 /* Failure already in queue before reset must not erase that reset. */
 model_enqueue(0); model_enqueue(2); /* transfer failure, bus reset */
 model_dispatch(); model_dispatch(); request_sense(0,0);
 /* Reset enqueued while a previous transfer callback is still running. */
 model_enqueue(2);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
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
 read_result=PF_SD_OK;
}
void setUp(void)
{
 initialized=true; worker_busy=mounted=false;
 locks=syncs=sense=asc=0;
 read_result=write_result=sync_result=owner_result=PF_SD_OK;
 owner=PF_SD_OWNER_USB_MSC;
 msc_enabled=true;
 msc_ejected=removal_prevented=request_pending=requested_enabled=false;
 transition_pending=request_rejected=recovery_requested=false;
 transfer_sense_pending=false;
 transfer_sense_key=transfer_sense_asc=0;
 extern void model_queue_reset(void);
 model_queue_reset();
}

void tearDown(void)
{
 TEST_ASSERT_EQUAL_UINT(0, locks);
}

void test_transfer_errors_survive_tinyusb_sense_overwrite(void)
{
 transfer_failures();
}

void test_sense_lifecycle_follows_commands_and_handled_resets(void)
{
 sense_lifecycle();
}

void test_sync_eject_removal_prevention_and_owner_handoff(void)
{
 uint8_t cmd[16]={0x35}; msc_enabled=true;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==0 && syncs==1);
 unsigned range_before=syncs;
 cmd[5]=128; TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && asc==0x21 && syncs==range_before);
 cmd[5]=127; cmd[8]=2;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && syncs==range_before);
 cmd[8]=1; TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==0);
 cmd[5]=0; cmd[8]=0;
 sync_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==3);
 sync_result=PF_SD_NOT_READY;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==2);
 sync_result=PF_SD_OK;
 cmd[1]=2; TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==5); cmd[1]=0;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(1,cmd,NULL,0)==-1);
 initialized=false; unsigned failed_before=syncs;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && syncs==failed_before && sense==2);
 initialized=true;
 cmd[0]=0xff; TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && asc==0x20); cmd[0]=0x35;
 uint8_t block[512]={0}; unsigned write_before=syncs;
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,block,512)==512);
 tud_msc_write10_complete_cb(0); TEST_ASSERT_TRUE(syncs==write_before);
 TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
 unsigned before=syncs;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,false,true));
 TEST_ASSERT_TRUE(!msc_ejected && syncs==before && asc==0x53);
 pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(msc_enabled && request_rejected);
 TEST_ASSERT_TRUE(!tud_msc_prevent_allow_medium_removal_cb(0,2,0) && removal_prevented);
 TEST_ASSERT_TRUE(!tud_msc_prevent_allow_medium_removal_cb(0,0,1) && removal_prevented);
 TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0,0,0));
 sync_result=PF_SD_IO_ERROR;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected);
 sync_result=PF_SD_NOT_READY;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected && sense==2);
 sync_result=PF_SD_OK;
 TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0,0,false,true) && msc_ejected);
 before=syncs;
 TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0,0,false,true) && syncs==before);
 initialized=false;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,true,true) && msc_ejected);
 initialized=true;
 worker_busy=true; pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(msc_enabled);
 worker_busy=false; owner_result=PF_SD_IO_ERROR;
 pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(msc_enabled && owner==PF_SD_OWNER_USB_MSC);
 owner_result=PF_SD_OK; transfer_sense_pending=true;
 pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(!msc_enabled && owner==PF_SD_OWNER_FIRMWARE && !transfer_sense_pending);
 before=syncs; tud_msc_write10_complete_cb(0); TEST_ASSERT_TRUE(syncs==before);
 TEST_ASSERT_TRUE(locks==0 && !mounted);
}

void test_start_stop_invalid_lun_and_disabled_media_report_fresh_sense(void)
{
 sense=SCSI_SENSE_MEDIUM_ERROR; asc=0x11;
 TEST_ASSERT_FALSE(tud_msc_start_stop_cb(1,0,false,true));
 TEST_ASSERT_EQUAL_UINT(SCSI_SENSE_NOT_READY,sense);
 TEST_ASSERT_EQUAL_UINT(0x3a,asc);
 msc_enabled=false;
 sense=SCSI_SENSE_MEDIUM_ERROR; asc=0x11;
 TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0,0,false,true));
 TEST_ASSERT_EQUAL_UINT(SCSI_SENSE_NOT_READY,sense);
 TEST_ASSERT_EQUAL_UINT(0x3a,asc);
 TEST_ASSERT_EQUAL_UINT(0,syncs);
}

void test_sync_reserved_fields_and_completion_error_boundary(void)
{
 uint8_t cmd[16]={0x35};
 const unsigned fields[]={1,6,9};
 for (unsigned i=0;i<3;++i) {
  cmd[fields[i]]=1;
  TEST_ASSERT_EQUAL_INT(-1,tud_msc_scsi_cb(0,cmd,NULL,0));
  TEST_ASSERT_EQUAL_UINT(5,sense); TEST_ASSERT_EQUAL_UINT(0x24,asc);
  cmd[fields[i]]=0;
 }
 TEST_ASSERT_EQUAL_INT(-1,tud_msc_scsi_cb(0,cmd,NULL,1));
 TEST_ASSERT_EQUAL_UINT(0,syncs);
 sync_result=PF_SD_NOT_CONFIGURED;
 TEST_ASSERT_EQUAL_INT(-1,tud_msc_scsi_cb(0,cmd,NULL,0));
 TEST_ASSERT_EQUAL_UINT(2,sense); TEST_ASSERT_EQUAL_UINT(0x3a,asc);
 TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0,0,false,true));
 TEST_ASSERT_FALSE(msc_ejected);
 TEST_ASSERT_EQUAL_UINT(2,sense); TEST_ASSERT_EQUAL_UINT(0x3a,asc);
 unsigned before=syncs;
 tud_msc_write10_complete_cb(0);
 TEST_ASSERT_EQUAL_UINT(before,syncs); /* already after CSW */
}
