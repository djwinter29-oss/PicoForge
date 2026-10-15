/* Pinned TinyUSB boundary/queue model; the real stack is NOT linked.
 * Separate TU ensures GNU --wrap redirects external driver references. */
#include "tusb.h"
#include <assert.h>
extern int32_t tud_msc_read10_cb(uint8_t, uint32_t, uint32_t, void *, uint32_t);
void mscd_reset(uint8_t port) { (void)port; tud_msc_set_sense(0,0,0,0); }
bool mscd_control_xfer_cb(uint8_t port, uint8_t stage, const tusb_control_request_t *r)
{
 (void)port;
 if (stage!=CONTROL_STAGE_SETUP) return true;
 if (r->bmRequestType_bit.type!=TUSB_REQ_TYPE_CLASS || r->bRequest!=MSC_REQ_RESET || r->wValue || r->wLength) return false;
 tud_msc_set_sense(0,0,0,0); return true;
}
/* Baseline passthroughs allow the behavior test to fail before production fix. */
extern void __real_mscd_reset(uint8_t);
extern bool __real_mscd_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
__attribute__((weak)) void __wrap_mscd_reset(uint8_t p) { __real_mscd_reset(p); }
__attribute__((weak)) bool __wrap_mscd_control_xfer_cb(uint8_t p, uint8_t s, const tusb_control_request_t *r) { return __real_mscd_control_xfer_cb(p,s,r); }
