#include "tusb.h"
#include "unity.h"
static unsigned queue[8], head, tail;
void model_queue_reset(void) { head=tail=0; }
extern int32_t tud_msc_read10_cb(uint8_t, uint32_t, uint32_t, void *, uint32_t);
void model_enqueue(unsigned event) { TEST_ASSERT_TRUE(tail-head<8); queue[tail++%8]=event; }
void model_control(uint8_t stage, const tusb_control_request_t *r) { (void)mscd_control_xfer_cb(0,stage,r); }
void model_dispatch(void)
{
 TEST_ASSERT_TRUE(head<tail);
 unsigned event=queue[head++%8];
 if (event==0) { uint8_t b[512]; TEST_ASSERT_EQUAL_INT32(-1,tud_msc_read10_cb(0,0,0,b,512)); tud_msc_set_sense(0,2,0x3a,0); }
 else if (event==1) { tusb_control_request_t r={.bmRequestType=0x21,.bRequest=MSC_REQ_RESET}; model_control(CONTROL_STAGE_SETUP,&r); }
 else mscd_reset(0);
}
