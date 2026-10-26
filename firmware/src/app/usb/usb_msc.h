/**
 * @file
 * @brief USB mass-storage ownership requests and status for the single-LUN SD card.
 * @note Every function is USB-task only; the state is not locked.
 */
#ifndef PF_USB_MSC_H
#define PF_USB_MSC_H

#include <stdbool.h>
#include <stdint.h>

/** @return True while the USB host owns the card. */
bool pf_usb_msc_is_enabled(void);

/** @return True after the host ejected the medium. */
bool pf_usb_msc_is_ejected(void);

/** @return True while an ownership change, including a forced-return remount, is in progress. */
bool pf_usb_msc_transition_pending(void);

/** @return True when the most recent valid ownership request failed. */
bool pf_usb_msc_request_rejected(void);

/**
 * @brief Queue an ownership change.
 * @param enabled True to give the card to the USB host, false to return it to firmware.
 * @return False when a remount already owns the transition.
 * @note Completion is observed through the status queries above.
 */
bool pf_usb_msc_request(bool enabled);

/**
 * @brief Queue an explicitly confirmed unsafe return that bypasses host eject and removal prevention.
 * @return False when a remount already owns the transition.
 * @note Device sync is still required. USB detaches before storage is released, and the pending state includes one
 *       worker remount attempt. A rejection after detach means firmware owns the card but the remount failed. This
 *       never implies a clean eject.
 */
bool pf_usb_msc_request_force_return(void);

/**
 * @brief Give the matching Feature 20 result GET time to complete before an ownership change.
 * @param transaction_id Accepted command transaction identifier; zero or no pending change is ignored.
 */
void pf_usb_msc_defer_for_hid_result(uint8_t transaction_id);

/**
 * @brief Record that the host fetched the matching Feature 20 transaction result.
 * @param transaction_id Result transaction identifier.
 */
void pf_usb_msc_note_hid_result_read(uint8_t transaction_id);

/**
 * @brief Request a faulted-media detach so the worker can reinitialize the card; not a clean eject.
 * @note Completion or rejection is exposed through the status queries above.
 */
void pf_usb_msc_request_recovery(void);

/** @brief Advance deferred results, reconnect timing and pending ownership requests; call every USB-task cycle. */
void pf_usb_msc_task(void);

#endif
