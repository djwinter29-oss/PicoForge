#ifndef PF_USB_MSC_H
#define PF_USB_MSC_H

#include <stdbool.h>
#include <stdint.h>

bool pf_usb_msc_is_enabled(void);
bool pf_usb_msc_is_ejected(void);
bool pf_usb_msc_transition_pending(void);
bool pf_usb_msc_request_rejected(void);
/** Queue an ownership request; false means a remount already owns the transition. */
bool pf_usb_msc_request(bool enabled);
/** @brief USB-task-only confirmed unsafe return; bypass host eject/removal prevention, not device sync.
 * Detach before releasing storage; pending includes one worker remount attempt.
 * Rejection after detach means firmware owns storage but remount failed. Never implies clean eject.
 */
/** Queue an explicitly confirmed unsafe return; false means a remount already owns the transition. */
bool pf_usb_msc_request_force_return(void);
/** @brief Give the matching Feature 20 result GET time to complete before an ownership change.
 * @param transaction_id Accepted command transaction identifier.
 */
void pf_usb_msc_defer_for_hid_result(uint8_t transaction_id);
/** @brief Record that the host fetched the matching Feature 20 transaction result.
 * @param transaction_id Result transaction identifier.
 */
void pf_usb_msc_note_hid_result_read(uint8_t transaction_id);
/** USB-task-only request for faulted-media detach, not a clean eject.
 * Completion/rejection is exposed through the existing MSC status queries.
 */
void pf_usb_msc_request_recovery(void);
void pf_usb_msc_task(void);

#endif