#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/usb_dcd")

#include "../../../src/integration/usb/dcd_guard.c"

#include <pthread.h>
#include <sched.h>

static spin_lock_t fake_lock;
static _Thread_local uint core;
static pthread_mutex_t hardware_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t interrupts[NUM_CORES];
static unsigned claims;
static unsigned acquisitions;
static unsigned irq_calls;
static irq_handler_t installed;
static irq_handler_t removed;
static uint installed_irq;
static uint8_t installed_order;
static bool transfer_result;
static uint8_t *transfer_buffer;
static uint16_t transfer_size;
static dcd_event_t dispatched[CFG_TUD_TASK_QUEUE_SZ];
static unsigned dispatched_count;
static unsigned task_calls;
static bool enqueue_during_dispatch;
static bool upstream_ready;

void setUp(void)
{
    dcd_lock = NULL;
    usb_handler = NULL;
    irq_core = core = 0u;
    for (uint i = 0; i < NUM_CORES; ++i) {
        lock_depth[i] = 0u;
        interrupts[i] = 1u;
    }
    fake_lock.held = false;
    claims = acquisitions = irq_calls = 0u;
    installed = removed = NULL;
    transfer_result = true;
    event_head = event_tail = event_count = dispatched_count = task_calls = 0u;
    enqueue_during_dispatch = false;
    upstream_ready = false;
}

void tearDown(void)
{
    TEST_ASSERT_FALSE(fake_lock.held);
    TEST_ASSERT_EQUAL_UINT(0u, lock_depth[0]);
    TEST_ASSERT_EQUAL_UINT(0u, lock_depth[1]);
    TEST_ASSERT_EQUAL_UINT(1u, interrupts[0]);
    TEST_ASSERT_EQUAL_UINT(1u, interrupts[1]);
}

uint get_core_num(void) { return core; }

uint32_t save_and_disable_interrupts(void)
{
    uint32_t saved = interrupts[core];
    interrupts[core] = 0u;
    return saved;
}

void restore_interrupts(uint32_t saved) { interrupts[core] = saved; }

int spin_lock_claim_unused(bool required)
{
    TEST_ASSERT_TRUE(required);
    ++claims;
    return 23;
}

spin_lock_t *spin_lock_instance(uint number)
{
    TEST_ASSERT_EQUAL_UINT(23u, number);
    return &fake_lock;
}

void spin_lock_unsafe_blocking(spin_lock_t *lock)
{
    TEST_ASSERT_EQUAL_PTR(&fake_lock, lock);
    TEST_ASSERT_EQUAL_UINT(0u, interrupts[core]);
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&hardware_lock));
    TEST_ASSERT_FALSE(lock->held);
    lock->held = true;
    ++acquisitions;
}

void spin_unlock_unsafe(spin_lock_t *lock)
{
    TEST_ASSERT_TRUE(lock->held);
    TEST_ASSERT_EQUAL_UINT(0u, interrupts[core]);
    lock->held = false;
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&hardware_lock));
}

static void check_guard(void)
{
    TEST_ASSERT_TRUE(fake_lock.held);
    TEST_ASSERT_EQUAL_UINT(0u, interrupts[core]);
    TEST_ASSERT_NOT_EQUAL(0u, lock_depth[core]);
}

static void upstream_irq(void)
{
    check_guard();
    ++irq_calls;
}

void __real_irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t priority)
{
    installed_irq = num;
    installed = handler;
    installed_order = priority;
}

void __real_irq_remove_handler(uint num, irq_handler_t handler)
{
    TEST_ASSERT_EQUAL_UINT(installed_irq, num);
    removed = handler;
}

bool __real_dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    check_guard();
    TEST_ASSERT_EQUAL_UINT8(0u, rhport);
    TEST_ASSERT_NULL(rh_init);
    __wrap_irq_add_shared_handler(USBCTRL_IRQ, upstream_irq, 255u);
    return true;
}

bool __real_dcd_deinit(uint8_t rhport)
{
    check_guard();
    TEST_ASSERT_EQUAL_UINT8(0u, rhport);
    __wrap_irq_remove_handler(USBCTRL_IRQ, upstream_irq);
    return true;
}

void __real_dcd_int_enable(uint8_t rhport)
{
    check_guard();
    TEST_ASSERT_EQUAL_UINT8(0u, rhport);
}

void __real_dcd_int_disable(uint8_t rhport) { __real_dcd_int_enable(rhport); }

bool __real_dcd_edpt_xfer(uint8_t rhport, uint8_t address, uint8_t *buffer, uint16_t size)
{
    check_guard();
    TEST_ASSERT_EQUAL_UINT8(0u, rhport);
    TEST_ASSERT_EQUAL_UINT8(0x81u, address);
    transfer_buffer = buffer;
    transfer_size = size;
    sched_yield();
    /* A nested driver entry must not acquire the nonrecursive hardware lock again. */
    __wrap_dcd_int_handler(rhport);
    return transfer_result;
}

void __real_dcd_int_handler(uint8_t rhport)
{
    TEST_ASSERT_EQUAL_UINT8(0u, rhport);
    upstream_irq();
}

void __real_dcd_event_handler(const dcd_event_t *event, bool in_isr)
{
    TEST_ASSERT_FALSE(in_isr);
    TEST_ASSERT_FALSE(fake_lock.held);
    TEST_ASSERT_LESS_THAN_UINT(CFG_TUD_TASK_QUEUE_SZ, dispatched_count);
    dispatched[dispatched_count++] = *event;
    upstream_ready = true;
    if (enqueue_during_dispatch) {
        dcd_event_t next = { .event_id = DCD_EVENT_SETUP_RECEIVED, .frame = 999u };
        __wrap_dcd_event_handler(&next, true);
    }
}

void __real_tud_task_ext(uint32_t timeout_ms, bool in_isr)
{
    (void)timeout_ms;
    TEST_ASSERT_FALSE(in_isr);
    TEST_ASSERT_FALSE(fake_lock.held);
    ++task_calls;
    upstream_ready = false;
}

bool __real_tud_task_event_ready(void) { return upstream_ready; }

void test_driver_and_irq_share_gate_and_preserve_lifecycle_registration(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, claims);
    TEST_ASSERT_EQUAL_UINT8(255u, installed_order);
    TEST_ASSERT_TRUE(installed != upstream_irq);
    installed();
    TEST_ASSERT_EQUAL_UINT(1u, irq_calls);
    __wrap_dcd_int_enable(0u);
    __wrap_dcd_int_disable(0u);
    dcd_event_t event = { .event_id = DCD_EVENT_SETUP_RECEIVED };
    __wrap_dcd_event_handler(&event, true);
    TEST_ASSERT_EQUAL_UINT(1u, event_count);
    TEST_ASSERT_TRUE(__wrap_dcd_deinit(0u));
    TEST_ASSERT_TRUE(installed == removed);
    TEST_ASSERT_NULL(usb_handler);
    TEST_ASSERT_EQUAL_UINT(0u, event_count);
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, claims);
}

void test_migrated_endpoint_call_preserves_result_arguments_and_nested_irq_mask(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    core = 1u;
    uint8_t buffer[4] = {0};
    unsigned before = acquisitions;
    TEST_ASSERT_TRUE(__wrap_dcd_edpt_xfer(0u, 0x81u, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_PTR(buffer, transfer_buffer);
    TEST_ASSERT_EQUAL_UINT16(sizeof(buffer), transfer_size);
    TEST_ASSERT_EQUAL_UINT(before + 1u, acquisitions);
    TEST_ASSERT_EQUAL_UINT(1u, interrupts[core]);
    transfer_result = false;
    TEST_ASSERT_FALSE(__wrap_dcd_edpt_xfer(0u, 0x81u, buffer, 0u));
    core = 0u;
    installed();
    TEST_ASSERT_EQUAL_UINT(3u, irq_calls);
}

void test_other_peripheral_irq_registration_is_untouched(void)
{
    __wrap_irq_add_shared_handler(7u, upstream_irq, 80u);
    TEST_ASSERT_TRUE(installed == upstream_irq);
    TEST_ASSERT_EQUAL_UINT8(80u, installed_order);
    __wrap_irq_remove_handler(7u, upstream_irq);
    TEST_ASSERT_TRUE(removed == upstream_irq);
    TEST_ASSERT_NULL(usb_handler);
    TEST_ASSERT_EQUAL_UINT(0u, acquisitions);
}

static void *migrating_task(void *context)
{
    core = 1u;
    uint8_t *buffer = context;
    for (unsigned i = 0; i < 1000u; ++i) {
        assert(__wrap_dcd_edpt_xfer(0u, 0x81u, buffer, 4u));
    }
    return NULL;
}

void test_core_one_transfers_and_core_zero_irqs_share_one_hardware_lock(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    uint8_t buffer[4] = {0};
    pthread_t task;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&task, NULL, migrating_task, buffer));
    for (unsigned i = 0; i < 1000u; ++i) {
        installed();
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_join(task, NULL));
    TEST_ASSERT_EQUAL_UINT(2000u, irq_calls);
    TEST_ASSERT_EQUAL_UINT(2001u, acquisitions);
}

void test_controller_events_are_copied_and_delivered_in_order_only_on_task_dispatch(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    for (unsigned i = 0; i < CFG_TUD_TASK_QUEUE_SZ; ++i) {
        dcd_event_t event = { .event_id = DCD_EVENT_SETUP_RECEIVED, .frame = i };
        __wrap_dcd_event_handler(&event, true);
        event.frame = 1000u;
    }
    TEST_ASSERT_EQUAL_UINT(0u, dispatched_count);
    core = 1u;
    __wrap_tud_task_ext(UINT32_MAX, false);
    TEST_ASSERT_EQUAL_UINT(CFG_TUD_TASK_QUEUE_SZ, dispatched_count);
    TEST_ASSERT_EQUAL_UINT(0u, event_count);
    TEST_ASSERT_EQUAL_UINT(2u, task_calls);
    for (unsigned i = 0; i < dispatched_count; ++i) {
        TEST_ASSERT_EQUAL_UINT(i, dispatched[i].frame);
    }
}

void test_consecutive_sofs_coalesce_without_crossing_setup_boundaries(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    for (unsigned i = 0; i < 1000u; ++i) {
        dcd_event_t event = { .event_id = DCD_EVENT_SOF, .frame = i };
        __wrap_dcd_event_handler(&event, true);
    }
    TEST_ASSERT_EQUAL_UINT(1u, event_count);
    dcd_event_t setup = { .event_id = DCD_EVENT_SETUP_RECEIVED };
    __wrap_dcd_event_handler(&setup, true);
    dcd_event_t sof = { .event_id = DCD_EVENT_SOF, .frame = 1001u };
    __wrap_dcd_event_handler(&sof, true);
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_EQUAL_UINT(3u, dispatched_count);
    TEST_ASSERT_EQUAL_UINT(999u, dispatched[0].frame);
    TEST_ASSERT_EQUAL_UINT(DCD_EVENT_SETUP_RECEIVED, dispatched[1].event_id);
    TEST_ASSERT_EQUAL_UINT(1001u, dispatched[2].frame);
}

void test_dispatch_is_bounded_when_irqs_enqueue_while_callbacks_run(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    dcd_event_t event = { .event_id = DCD_EVENT_SETUP_RECEIVED };
    __wrap_dcd_event_handler(&event, true);
    enqueue_during_dispatch = true;
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_EQUAL_UINT(1u, dispatched_count);
    TEST_ASSERT_EQUAL_UINT(1u, event_count);
    enqueue_during_dispatch = false;
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_EQUAL_UINT(2u, dispatched_count);
    TEST_ASSERT_EQUAL_UINT(999u, dispatched[1].frame);
    TEST_ASSERT_EQUAL_UINT(0u, event_count);
}

void test_task_readiness_includes_both_queues_and_preinit_task_calls_remain_safe(void)
{
    TEST_ASSERT_FALSE(__wrap_tud_task_event_ready());
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_EQUAL_UINT(1u, task_calls);
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    TEST_ASSERT_FALSE(__wrap_tud_task_event_ready());
    upstream_ready = true;
    TEST_ASSERT_TRUE(__wrap_tud_task_event_ready());
    upstream_ready = false;
    dcd_event_t event = { .event_id = DCD_EVENT_SETUP_RECEIVED };
    __wrap_dcd_event_handler(&event, true);
    TEST_ASSERT_TRUE(__wrap_tud_task_event_ready());
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_FALSE(__wrap_tud_task_event_ready());
}

void test_event_fifo_wraps_from_a_partially_consumed_ring(void)
{
    TEST_ASSERT_TRUE(__wrap_dcd_init(0u, NULL));
    for (unsigned i = 0; i < 10u; ++i) {
        dcd_event_t event = { .event_id = DCD_EVENT_SETUP_RECEIVED, .frame = i };
        __wrap_dcd_event_handler(&event, true);
    }
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_EQUAL_UINT(10u, dispatched_count);
    dispatched_count = 0u;
    for (unsigned i = 0; i < CFG_TUD_TASK_QUEUE_SZ; ++i) {
        dcd_event_t event = { .event_id = DCD_EVENT_SETUP_RECEIVED, .frame = 100u + i };
        __wrap_dcd_event_handler(&event, true);
    }
    __wrap_tud_task_ext(0u, false);
    TEST_ASSERT_EQUAL_UINT(CFG_TUD_TASK_QUEUE_SZ, dispatched_count);
    for (unsigned i = 0; i < dispatched_count; ++i) {
        TEST_ASSERT_EQUAL_UINT(100u + i, dispatched[i].frame);
    }
}
