/** @file
 * @brief Serialize the pinned RP2040/RP2350 USB DCD against its fixed-core interrupt.
 */
#include "device/dcd.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#if CFG_TUSB_OS != OPT_OS_PICO
#error "The DCD guard requires TinyUSB's cross-core-safe Pico OS queues"
#endif

/* ponytail: one controller-wide gate serializes endpoints; this full-speed device has short DCD calls.
 * Consider finer endpoint locks only if measured IRQ latency warrants them, retaining reset/setup exclusion. */
static spin_lock_t *dcd_lock; /**< Dedicated lock, allocated during startup before the scheduler. */
static uint32_t lock_depth[NUM_CORES]; /**< IRQ-disabled nesting on each core; prevents recursive deadlock. */
static irq_handler_t usb_handler; /**< Upstream shared handler forwarded under the DCD lock. */
static uint irq_core; /**< Core owning USB NVIC configuration for the lifetime of this DCD instance. */
static dcd_event_t events[CFG_TUD_TASK_QUEUE_SZ]; /**< Copied controller events, protected by the DCD gate. */
static uint event_head; /**< Next controller-event insertion slot. */
static uint event_tail; /**< Oldest controller event awaiting task dispatch. */
static uint event_count; /**< Occupied controller-event slots; overflow is an explicit assertion. */

/** @brief Disable local interrupts and exclude DCD operations on the other core. */
static uint32_t dcd_enter(void)
{
    uint32_t saved = save_and_disable_interrupts();
    uint core = get_core_num();
    hard_assert(dcd_lock != NULL);
    if (lock_depth[core]++ == 0u) {
        spin_lock_unsafe_blocking(dcd_lock);
    }
    return saved;
}

/** @brief Release the outermost DCD reservation before restoring local interrupt state. */
static void dcd_exit(uint32_t saved)
{
    uint core = get_core_num();
    hard_assert(lock_depth[core] != 0u);
    if (--lock_depth[core] == 0u) {
        spin_unlock_unsafe(dcd_lock);
    }
    restore_interrupts(saved);
}

/** @brief Gate the upstream IRQ, including reset, setup, SOF and transfer completion paths. */
static void guarded_usb_irq(void)
{
    uint32_t saved = dcd_enter();
    usb_handler();
    dcd_exit(saved);
}

void __real_dcd_event_handler(const dcd_event_t *event, bool in_isr);
void __real_tud_task_ext(uint32_t timeout_ms, bool in_isr);
bool __real_tud_task_event_ready(void);

/** @brief Include deferred controller events in TinyUSB's public task-readiness query. */
bool __wrap_tud_task_event_ready(void)
{
    if (dcd_lock == NULL) {
        return __real_tud_task_event_ready();
    }
    uint32_t saved = dcd_enter();
    bool pending = event_count != 0u;
    dcd_exit(saved);
    return pending || __real_tud_task_event_ready();
}

/** @brief Copy controller events without letting the IRQ mutate TinyUSB's task-owned device state. */
void __wrap_dcd_event_handler(const dcd_event_t *event, bool in_isr)
{
    (void)in_isr;
    uint32_t saved = dcd_enter();
    uint previous = (event_head + CFG_TUD_TASK_QUEUE_SZ - 1u) % CFG_TUD_TASK_QUEUE_SZ;
    if (event->event_id == DCD_EVENT_SOF && event_count != 0u &&
        events[previous].event_id == DCD_EVENT_SOF) {
        events[previous] = *event;
    } else {
        hard_assert(event_count < CFG_TUD_TASK_QUEUE_SZ);
        events[event_head] = *event;
        event_head = (event_head + 1u) % CFG_TUD_TASK_QUEUE_SZ;
        ++event_count;
    }
    dcd_exit(saved);
}

/** @brief Dispatch a bounded controller batch on the USB task, outside the DCD reservation.
 * Consecutive SOFs are coalesced; this product has no per-frame ISR hooks. Clear the upstream queue first
 * so one full controller batch fits, retaining queued-SETUP suppression within the batch.
 */
void __wrap_tud_task_ext(uint32_t timeout_ms, bool in_isr)
{
    hard_assert(!in_isr);
    __real_tud_task_ext(0u, false);
    if (dcd_lock == NULL) {
        return;
    }
    uint32_t saved = dcd_enter();
    uint remaining = event_count;
    dcd_exit(saved);
    while (remaining-- != 0u) {
        saved = dcd_enter();
        dcd_event_t event = events[event_tail];
        event_tail = (event_tail + 1u) % CFG_TUD_TASK_QUEUE_SZ;
        --event_count;
        dcd_exit(saved);
        __real_dcd_event_handler(&event, false);
    }
    __real_tud_task_ext(timeout_ms, false);
}

void __real_irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority);
void __real_irq_remove_handler(uint num, irq_handler_t handler);

/** @brief Interpose only USB registration; other peripheral handlers remain unchanged. */
void __wrap_irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority)
{
    if (num == USBCTRL_IRQ) {
        hard_assert(usb_handler == NULL);
        usb_handler = handler;
        handler = guarded_usb_irq;
    }
    __real_irq_add_shared_handler(num, handler, order_priority);
}

/** @brief Remove the same guarded callback that was installed during DCD initialization. */
void __wrap_irq_remove_handler(uint num, irq_handler_t handler)
{
    if (num == USBCTRL_IRQ && handler == usb_handler) {
        handler = guarded_usb_irq;
        usb_handler = NULL;
    }
    __real_irq_remove_handler(num, handler);
}

bool __real_dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init);

/** @brief Allocate the dedicated gate before upstream initialization can enable USB interrupts.
 * Startup must call this before the scheduler; runtime reinitialization must use the IRQ-owning core.
 */
bool __wrap_dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    if (dcd_lock == NULL) {
        dcd_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
        irq_core = get_core_num();
    }
    hard_assert(get_core_num() == irq_core);
    uint32_t saved = dcd_enter();
    event_head = event_tail = event_count = 0u;
    bool result = __real_dcd_init(rhport, rh_init);
    dcd_exit(saved);
    return result;
}

/** @brief Wrap nonblocking DCD calls only; task-dispatched callbacks and media I/O never hold this gate. */
#define PF_DCD_VOID(name, parameters, arguments) \
    void __real_##name parameters; \
    void __wrap_##name parameters \
    { \
        uint32_t saved = dcd_enter(); \
        __real_##name arguments; \
        dcd_exit(saved); \
    }

/** @brief Preserve boolean results while excluding simultaneous IRQ endpoint updates. */
#define PF_DCD_BOOL(name, parameters, arguments) \
    bool __real_##name parameters; \
    bool __wrap_##name parameters \
    { \
        uint32_t saved = dcd_enter(); \
        bool result = __real_##name arguments; \
        dcd_exit(saved); \
        return result; \
    }

/* NVIC operations are core-local. Initialization/deinitialization stay on the IRQ core;
 * normal USB task work may migrate, and never calls these lifecycle operations. */
void __real_dcd_int_enable(uint8_t rhport);
void __real_dcd_int_disable(uint8_t rhport);
bool __real_dcd_deinit(uint8_t rhport);

/** @brief Enable USB NVIC delivery only on the core that installed the handler. */
void __wrap_dcd_int_enable(uint8_t rhport)
{
    hard_assert(get_core_num() == irq_core);
    uint32_t saved = dcd_enter();
    __real_dcd_int_enable(rhport);
    dcd_exit(saved);
}

/** @brief Disable USB NVIC delivery only on its owning core. */
void __wrap_dcd_int_disable(uint8_t rhport)
{
    hard_assert(get_core_num() == irq_core);
    uint32_t saved = dcd_enter();
    __real_dcd_int_disable(rhport);
    dcd_exit(saved);
}

/** @brief Remove the fixed-core handler while excluding outstanding driver operations. */
bool __wrap_dcd_deinit(uint8_t rhport)
{
    hard_assert(get_core_num() == irq_core);
    uint32_t saved = dcd_enter();
    bool result = __real_dcd_deinit(rhport);
    if (result) {
        event_head = event_tail = event_count = 0u;
    }
    dcd_exit(saved);
    return result;
}

PF_DCD_VOID(dcd_set_address, (uint8_t rhport, uint8_t address), (rhport, address))
PF_DCD_VOID(dcd_remote_wakeup, (uint8_t rhport), (rhport))
PF_DCD_VOID(dcd_disconnect, (uint8_t rhport), (rhport))
PF_DCD_VOID(dcd_connect, (uint8_t rhport), (rhport))
PF_DCD_VOID(dcd_sof_enable, (uint8_t rhport, bool enabled), (rhport, enabled))
PF_DCD_VOID(dcd_edpt0_status_complete, (uint8_t rhport, const tusb_control_request_t *request), (rhport, request))
PF_DCD_BOOL(dcd_edpt_open, (uint8_t rhport, const tusb_desc_endpoint_t *descriptor), (rhport, descriptor))
PF_DCD_BOOL(dcd_edpt_iso_alloc, (uint8_t rhport, uint8_t address, uint16_t size), (rhport, address, size))
PF_DCD_BOOL(dcd_edpt_iso_activate, (uint8_t rhport, const tusb_desc_endpoint_t *descriptor), (rhport, descriptor))
PF_DCD_VOID(dcd_edpt_close_all, (uint8_t rhport), (rhport))
PF_DCD_BOOL(dcd_edpt_xfer, (uint8_t rhport, uint8_t address, uint8_t *buffer, uint16_t size),
            (rhport, address, buffer, size))
PF_DCD_VOID(dcd_edpt_stall, (uint8_t rhport, uint8_t address), (rhport, address))
PF_DCD_VOID(dcd_edpt_clear_stall, (uint8_t rhport, uint8_t address), (rhport, address))
PF_DCD_VOID(dcd_int_handler, (uint8_t rhport), (rhport))
