#include <stdbool.h>
#include "unity.h"

TEST_INCLUDE_PATH("firmware/tests/support/msc")
TEST_SOURCE_FILE("firmware/tests/support/msc/msc_reset_model.c")
TEST_SOURCE_FILE("firmware/tests/support/msc/msc_reset_queue.c")
#define PF_STORAGE_WORKER_H
static bool pf_fatfs_volume_is_mounted(void);
static void pf_fatfs_volume_forget_filesystem_format(void);
static bool pf_storage_worker_busy(void);
static bool pf_storage_worker_media_absent(void);
static bool pf_storage_worker_recover_media(void);
static bool pf_storage_worker_return_from_usb(void);
static bool pf_storage_worker_unmount_for_usb(void);
#include "app/usb/usb_msc.c"
uint64_t test_time_us;
static bool initialized=true, worker_busy, mounted, media_absent, recover_accepted;
static unsigned locks, syncs, sense, asc;
static pf_block_result_t read_result=PF_BLOCK_OK, write_result=PF_BLOCK_OK;
static pf_block_result_t sync_result=PF_BLOCK_OK, owner_result=PF_BLOCK_OK;
static enum { OWNER_FIRMWARE, OWNER_USB } owner=OWNER_USB;
static bool pf_fatfs_volume_is_mounted(void) { return mounted; }
static void pf_fatfs_volume_forget_filesystem_format(void) {}
static bool pf_storage_worker_busy(void) { return worker_busy; }
static bool pf_storage_worker_media_absent(void) { return media_absent; }
static bool pf_storage_worker_recover_media(void) { return recover_accepted; }
static bool pf_storage_worker_return_from_usb(void) { worker_busy = true; return true; }
static bool pf_storage_worker_unmount_for_usb(void) { worker_busy = true; return true; }
bool pf_storage_try_lock(void) { ++locks; return true; }
void pf_storage_lock(void) { ++locks; }
void pf_storage_unlock(void) { TEST_ASSERT_TRUE(locks); --locks; }
bool pf_storage_msc_ready(void) { return initialized; }
uint32_t pf_storage_msc_count(void) { return 128; }
pf_block_result_t pf_storage_msc_acquire(void)
{ if(owner_result==PF_BLOCK_OK) owner=OWNER_USB; return owner_result; }
pf_block_result_t pf_storage_msc_release(void)
{ if(owner_result==PF_BLOCK_OK) owner=OWNER_FIRMWARE; return owner_result; }
pf_block_result_t pf_storage_msc_release_for_recovery(void)
{ TEST_ASSERT_FALSE(initialized); return pf_storage_msc_release(); }
pf_block_result_t pf_storage_msc_sync(void) { ++syncs; return sync_result; }
pf_block_result_t pf_storage_msc_read(uint32_t l, void *b, uint32_t n) { (void)l;(void)b;(void)n; return read_result; }
pf_block_result_t pf_storage_msc_write(uint32_t l, const void *b, uint32_t n)
{ (void)l;(void)b;(void)n; return write_result; }
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
  read_result=PF_BLOCK_IO_ERROR;
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
  read_result=PF_BLOCK_OK; write_result=PF_BLOCK_IO_ERROR;
  TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,offset,block,size)==-1);
  TEST_ASSERT_TRUE(sense==3 && asc==0x0c);
  tud_msc_set_sense(0,2,0x3a,0); request_sense(3,0x0c); request_sense(0,0);
  for (unsigned not_configured=0; not_configured<2; ++not_configured) {
   read_result=write_result=not_configured ? PF_BLOCK_NOT_READY : PF_BLOCK_NOT_READY;
   TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
   read_result=PF_BLOCK_OK;
   TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,offset,block,size)==-1);
   tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
  }
  read_result=write_result=PF_BLOCK_OK;
 }
 /* A later application error supersedes pending transfer sense. */
 read_result=PF_BLOCK_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 initialized=false;
 TEST_ASSERT_TRUE(!tud_msc_test_unit_ready_cb(0)); request_sense(2,0x3a);
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,block,512)==-1);
 tud_msc_set_sense(0,2,0x3a,0); request_sense(2,0x3a);
 initialized=true; read_result=PF_BLOCK_OK;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==512);
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,0,block,512)==512);
 request_sense(0,0);
}
static void sense_lifecycle(void)
{
 uint8_t block[512]={0}, cmd[16]={0x35};
 /* TinyUSB overwrites failed transfer sense; stale app sense must not leak. */
 read_result=PF_BLOCK_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
 read_result=PF_BLOCK_OK;
 TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==512);
 tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_BLOCK_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_write10_cb(0,0,4,block,16)==-1);
 read_result=PF_BLOCK_OK;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==0);
 tud_msc_set_sense(0,0,0,0); request_sense(0,0);
 read_result=PF_BLOCK_IO_ERROR;
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
 read_result=PF_BLOCK_OK;
 /* Built-in TinyUSB commands call these callbacks, not our SCSI hook. */
 for (unsigned c=0; c<3; ++c) {
  uint32_t blocks; uint16_t size;
  read_result=PF_BLOCK_IO_ERROR;
  TEST_ASSERT_TRUE(tud_msc_read10_cb(0,0,0,block,512)==-1);
  tud_msc_set_sense(0,2,0x3a,0);
  if (c==0) TEST_ASSERT_TRUE(tud_msc_test_unit_ready_cb(0));
  else if (c==1) { tud_msc_capacity_cb(0,&blocks,&size); TEST_ASSERT_TRUE(blocks==128 && size==512); }
  else TEST_ASSERT_TRUE(tud_msc_is_writable_cb(0));
  tud_msc_set_sense(0,0,0,0); request_sense(0,0);
  read_result=PF_BLOCK_OK;
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
 read_result=PF_BLOCK_IO_ERROR;
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
 read_result=PF_BLOCK_OK;
}
void setUp(void)
{
 initialized=true; worker_busy=mounted=media_absent=recover_accepted=false;
 locks=syncs=sense=asc=0;
 read_result=write_result=sync_result=owner_result=PF_BLOCK_OK;
 owner=OWNER_USB;
 msc_enabled=true;
 msc_ejected=removal_prevented=request_pending=requested_enabled=false;
 transition_pending=request_rejected=recovery_requested=false;
 transfer_sense_pending=false;
 unmount_pending = force_return_requested = remount_pending = false;
 hid_result_deferred = hid_result_read = false;
 hid_result_transaction_id = 0u;
 hid_result_deadline_us = hid_result_settle_us = test_time_us = 0u;
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
 sync_result=PF_BLOCK_IO_ERROR;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==3);
 sync_result=PF_BLOCK_NOT_READY;
 TEST_ASSERT_TRUE(tud_msc_scsi_cb(0,cmd,NULL,0)==-1 && sense==2);
 sync_result=PF_BLOCK_OK;
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
 sync_result=PF_BLOCK_IO_ERROR;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected);
 sync_result=PF_BLOCK_NOT_READY;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,false,true) && !msc_ejected && sense==2);
 sync_result=PF_BLOCK_OK;
 TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0,0,false,true) && msc_ejected);
 before=syncs;
 TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0,0,false,true) && syncs==before);
 initialized=false;
 TEST_ASSERT_TRUE(!tud_msc_start_stop_cb(0,0,true,true) && msc_ejected);
 initialized=true;
 worker_busy=true; pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(msc_enabled);
 worker_busy=false; owner_result=PF_BLOCK_IO_ERROR;
 pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(msc_enabled && owner==OWNER_USB);
 owner_result=PF_BLOCK_OK; transfer_sense_pending=true;
 pf_usb_msc_request(false); pf_usb_msc_task(); TEST_ASSERT_TRUE(!msc_enabled && owner==OWNER_FIRMWARE && !transfer_sense_pending);
 before=syncs; tud_msc_write10_complete_cb(0); TEST_ASSERT_TRUE(syncs==before);
 TEST_ASSERT_TRUE(locks==0 && !mounted);
}

void test_enable_queues_unmount_and_waits_before_sd_ownership_handoff(void)
{
 msc_enabled=false;
 owner=OWNER_FIRMWARE;
 mounted=true;
 pf_usb_msc_request(true);
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(unmount_pending && worker_busy && transition_pending && !request_rejected);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled);
 mounted=false;
 worker_busy=false;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && !transition_pending && !request_rejected);
 TEST_ASSERT_EQUAL(OWNER_USB,owner);
}

void test_force_return_bypasses_only_host_eject_and_waits_for_worker_mount(void)
{
 removal_prevented=true;
 pf_usb_msc_request(false);
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && request_rejected);
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled || msc_ejected || request_rejected);
 TEST_ASSERT_TRUE(remount_pending && transition_pending && worker_busy);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);
 TEST_ASSERT_EQUAL_UINT(1u,syncs);
 pf_usb_msc_request(true);
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled);
 mounted=true;
 worker_busy=false;
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(remount_pending || transition_pending || request_rejected);
}

void test_force_return_rejects_busy_or_failed_device_sync_without_releasing_sd(void)
{
 worker_busy=true;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && request_rejected);
 TEST_ASSERT_EQUAL_UINT(0u,syncs);
 worker_busy=false;
 sync_result=PF_BLOCK_IO_ERROR;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && request_rejected);
 TEST_ASSERT_FALSE(transition_pending || remount_pending);
 TEST_ASSERT_EQUAL(OWNER_USB,owner);
}

void test_force_return_reports_failed_mount_without_reexposing_sd(void)
{
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 worker_busy=false;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(request_rejected);
 TEST_ASSERT_FALSE(msc_enabled || msc_ejected || transition_pending);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);
}

void test_failed_sd_unmount_rejects_enable_without_acquiring_usb(void)
{
 msc_enabled=false;
 owner=OWNER_FIRMWARE;
 mounted=true;
 pf_usb_msc_request(true);
 pf_usb_msc_task();
 worker_busy=false;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(request_rejected && mounted && !msc_enabled && !transition_pending);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);
}

void test_hid_msc_transition_waits_for_matching_result_read_and_control_completion(void)
{
 msc_ejected=true;
 pf_usb_msc_request(false);
 pf_usb_msc_defer_for_hid_result(42u);
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && transition_pending);

 pf_usb_msc_note_hid_result_read(41u);
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && transition_pending);

 pf_usb_msc_note_hid_result_read(42u);
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && transition_pending);
 test_time_us=9999u;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && transition_pending);
 test_time_us=10000u;
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled || transition_pending || request_rejected);
}

void test_hid_msc_transition_uses_bounded_fallback_when_result_is_not_read(void)
{
 msc_ejected=true;
 pf_usb_msc_request(false);
 pf_usb_msc_defer_for_hid_result(43u);
 test_time_us=499999u;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && transition_pending);
 test_time_us=500000u;
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled || transition_pending || request_rejected);
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
 sync_result=PF_BLOCK_NOT_READY;
 TEST_ASSERT_EQUAL_INT(-1,tud_msc_scsi_cb(0,cmd,NULL,0));
 TEST_ASSERT_EQUAL_UINT(2,sense); TEST_ASSERT_EQUAL_UINT(0x3a,asc);
 TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0,0,false,true));
 TEST_ASSERT_FALSE(msc_ejected);
 TEST_ASSERT_EQUAL_UINT(2,sense); TEST_ASSERT_EQUAL_UINT(0x3a,asc);
 unsigned before=syncs;
 tud_msc_write10_complete_cb(0);
 TEST_ASSERT_EQUAL_UINT(before,syncs); /* already after CSW */
}

/* ponytail: exercise app lifecycle boundaries with the existing reset model; this test does not execute TinyUSB BOT itself. */
void test_volatile_prevention_clears_on_bus_reset_or_disconnect_but_not_bot_reset(void)
{
 extern void model_control(uint8_t, const tusb_control_request_t *);
 tusb_control_request_t reset={.bmRequestType=0x21,.bRequest=MSC_REQ_RESET};
 TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
 model_control(CONTROL_STAGE_SETUP,&reset);
 TEST_ASSERT_TRUE(removal_prevented);
 for (unsigned boundary=0; boundary<2; ++boundary) {
  TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
  if (boundary==0) __wrap_mscd_reset(0);
  else tud_umount_cb();
  TEST_ASSERT_FALSE(removal_prevented);
  pf_usb_msc_request(false); pf_usb_msc_task();
  TEST_ASSERT_TRUE(msc_enabled && request_rejected);
  TEST_ASSERT_EQUAL(OWNER_USB,owner);
 }
 TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0,0,false,true));
 pf_usb_msc_request(false); pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled || request_rejected);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);
}

void test_prevent_disconnect_allows_absent_disable_and_unavailable_recovery(void)
{
 TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
 media_absent=true; initialized=false;
 tud_umount_cb();
 pf_usb_msc_request(false); pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled || request_rejected);
 TEST_ASSERT_EQUAL_UINT(0u,syncs);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);

 msc_enabled=true; owner=OWNER_USB;
 TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
 __wrap_mscd_reset(0);
 recover_accepted=true;
 pf_usb_msc_request_recovery(); pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled || request_rejected);
 TEST_ASSERT_EQUAL(OWNER_FIRMWARE,owner);
}

void test_request_during_the_reconnect_window_restarts_the_deadline_and_umount_keeps_it(void)
{
 msc_enabled=false;
 owner=OWNER_FIRMWARE;
 mounted=false;
 pf_usb_msc_request(true);
 pf_usb_msc_task();
 worker_busy=false;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && reconnect_pending);
 test_time_us=100000u;
 msc_ejected=true;
 pf_usb_msc_request(false);
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(msc_enabled);
 TEST_ASSERT_TRUE(reconnect_pending);
 TEST_ASSERT_EQUAL_UINT64(100000u+MSC_RECONNECT_DETACH_US,reconnect_at_us);
 tud_umount_cb();
 test_time_us=reconnect_at_us-1u;
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(reconnect_pending);
 test_time_us=reconnect_at_us;
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(reconnect_pending);
}

void test_inquiry_fields_are_fixed_width_and_space_padded(void)
{
 uint8_t vendor[8], product[16], revision[4];
 memset(vendor,0xAA,sizeof(vendor)); memset(product,0xAA,sizeof(product)); memset(revision,0xAA,sizeof(revision));
 tud_msc_inquiry_cb(0,vendor,product,revision);
 TEST_ASSERT_EQUAL_MEMORY("PicoForg",vendor,8);
 TEST_ASSERT_EQUAL_MEMORY("SD Card         ",product,16);
 TEST_ASSERT_EQUAL_MEMORY("1.0 ",revision,4);
}

void test_failed_recovery_release_reconnects_through_the_deadline_path(void)
{
 msc_enabled=true; owner=OWNER_USB; initialized=false; mounted=false; worker_busy=false;
 recover_accepted=true; owner_result=PF_BLOCK_NOT_READY;
 test_time_us=1000u;
 pf_usb_msc_request_recovery(); pf_usb_msc_task();
 TEST_ASSERT_TRUE(request_rejected && msc_enabled && reconnect_pending);
 TEST_ASSERT_EQUAL_UINT64(1000u,reconnect_at_us);
 pf_usb_msc_task();
 TEST_ASSERT_FALSE(reconnect_pending);
 owner_result=PF_BLOCK_OK;
}
