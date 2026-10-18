#ifndef PF_USB_MSC_H
#define PF_USB_MSC_H

#include <stdbool.h>

bool pf_usb_msc_is_enabled(void);
bool pf_usb_msc_is_ejected(void);
bool pf_usb_msc_transition_pending(void);
bool pf_usb_msc_request_rejected(void);
void pf_usb_msc_request(bool enabled);
/** @brief USB-task-only confirmed unsafe return; bypass host eject/removal prevention, not device sync.
 * Detach before releasing storage; pending includes one worker remount attempt.
 * Rejection after detach means firmware owns storage but remount failed. Never implies clean eject.
 */
void pf_usb_msc_request_force_return(void);
/** USB-task-only request for faulted-media detach, not a clean eject.
 * Completion/rejection is exposed through the existing MSC status queries.
 */
void pf_usb_msc_request_recovery(void);
void pf_usb_msc_task(void);

#endif