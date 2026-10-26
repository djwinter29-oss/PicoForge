/** @file
 * @brief USB service task: TinyUSB device stack, CDC CLI, HID reports and MSC ownership.
 */
#ifndef PF_USB_TASK_H
#define PF_USB_TASK_H

/**
 * @brief Create the storage worker, initialize TinyUSB and start the statically allocated USB service task.
 * @note Call once before the scheduler starts. Failure panics.
 */
void pf_usb_start(void);

#endif
