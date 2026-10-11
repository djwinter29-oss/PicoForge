#ifndef PF_USB_MSC_H
#define PF_USB_MSC_H

#include <stdbool.h>

bool pf_usb_msc_is_enabled(void);
bool pf_usb_msc_is_ejected(void);
bool pf_usb_msc_transition_pending(void);
bool pf_usb_msc_request_rejected(void);
void pf_usb_msc_request(bool enabled);
void pf_usb_msc_task(void);

#endif