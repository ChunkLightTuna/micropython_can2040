// MicroPython binding for can2040 - a software CAN bus for RP2040/RP2350 PIO.
//
// Python API (module "can2040"):
//
//   can = can2040.CAN2040(pio=0, rx=5, tx=4, bitrate=500000)
//   can.start()                         # claims PIO block `pio` and its IRQ
//   can.send(id, data=b"", rtr=False, ext=False) -> bool   # True if queued (queue holds 4)
//   can.check_transmit() -> bool        # room in the transmit queue?
//   can.recv() -> (id, data, rtr, ext) | None               # pop one received frame
//   can.any() -> int                    # frames waiting in the receive queue
//   can.on_receive(cb)                  # cb(id, data, rtr, ext) via the scheduler; None clears
//   can.stats() -> dict                 # rx, tx, tx_attempts, parse_errors, rx_dropped, rx_overflow
//   can.stop()                          # release the PIO block and IRQ
//   can.running -> bool
//
// One instance exists per PIO block (the constructor returns the same object
// for the same `pio` and reconfigures it).  All state lives in static C memory
// so the IRQ path never touches the MicroPython heap.  Received frames are
// copied into a ring buffer from IRQ context; `on_receive` callbacks run later
// in normal context through mp_sched_schedule().
//
// can2040 is GPLv3 (Kevin O'Connor); this binding is released under the same license.

#include <stddef.h>
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mpstate.h"

#include "pico/platform.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "can2040.h"

#define CAN2040_MP_RX_QUEUE_SIZE 64   // must be a power of two
#define CAN2040_MP_RX_QUEUE_MASK (CAN2040_MP_RX_QUEUE_SIZE - 1)
#define CAN2040_MP_STD_ID_MAX 0x7FF
#define CAN2040_MP_EXT_ID_MAX 0x1FFFFFFF

typedef struct _can2040_mp_obj_t {
    mp_obj_base_t base;
    struct can2040 cbus;          // can2040 private state (must not move: static instance)
    uint32_t pio_num;
    uint32_t irq_num;
    int32_t gpio_rx;
    int32_t gpio_tx;
    uint32_t bitrate;
    volatile bool running;
    volatile bool cb_scheduled;   // a dispatch of the Python callback is pending
    irq_handler_t prev_handler;   // handler displaced from the PIO IRQ (restored on stop)

    // Receive ring buffer: producer = IRQ, consumer = main context.
    struct can2040_msg rx_queue[CAN2040_MP_RX_QUEUE_SIZE];
    volatile uint32_t rx_push_pos;
    uint32_t rx_pull_pos;

    volatile uint32_t rx_dropped;   // frames lost because rx_queue was full
    volatile uint32_t rx_overflow;  // CAN2040_NOTIFY_ERROR events (can2040 internal overflow)
} can2040_mp_obj_t;

static can2040_mp_obj_t can2040_mp_instances[NUM_PIOS];

static const uint32_t can2040_mp_irq_nums[NUM_PIOS] = {
    PIO0_IRQ_0,
    PIO1_IRQ_0,
    #if NUM_PIOS > 2
    PIO2_IRQ_0,
    #endif
};

// Python-level receive callbacks, one per PIO block. Registered as GC roots.
MP_REGISTER_ROOT_POINTER(mp_obj_t can2040_mp_rx_callback[NUM_PIOS]);

static mp_obj_t can2040_mp_dispatch(mp_obj_t self_in);
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_dispatch_obj, can2040_mp_dispatch);

/****************************************************************
 * IRQ context
 ****************************************************************/

// Main can2040 callback. Runs in IRQ context: copy the frame and get out.
static void __not_in_flash_func(can2040_mp_cb)(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
    can2040_mp_obj_t *self = (can2040_mp_obj_t *)((char *)cd - offsetof(can2040_mp_obj_t, cbus));

    if (notify == CAN2040_NOTIFY_RX) {
        uint32_t push_pos = self->rx_push_pos;
        if (push_pos - self->rx_pull_pos >= CAN2040_MP_RX_QUEUE_SIZE) {
            self->rx_dropped++;
            return;
        }
        self->rx_queue[push_pos & CAN2040_MP_RX_QUEUE_MASK] = *msg;
        self->rx_push_pos = push_pos + 1;

        // Schedule at most one dispatch per burst of frames.
        mp_obj_t cb = MP_STATE_PORT(can2040_mp_rx_callback)[self->pio_num];
        if (cb != MP_OBJ_NULL && cb != mp_const_none && !self->cb_scheduled) {
            self->cb_scheduled = true;
            if (!mp_sched_schedule(MP_OBJ_FROM_PTR(&can2040_mp_dispatch_obj), MP_OBJ_FROM_PTR(self))) {
                // Scheduler queue full; the next frame will retry.
                self->cb_scheduled = false;
            }
        }
    } else if (notify == CAN2040_NOTIFY_ERROR) {
        self->rx_overflow++;
    }
    // CAN2040_NOTIFY_TX is counted by can2040 itself (stats.tx_total).
}

static void __not_in_flash_func(can2040_mp_irq0)(void) {
    can2040_pio_irq_handler(&can2040_mp_instances[0].cbus);
}

static void __not_in_flash_func(can2040_mp_irq1)(void) {
    can2040_pio_irq_handler(&can2040_mp_instances[1].cbus);
}

#if NUM_PIOS > 2
static void __not_in_flash_func(can2040_mp_irq2)(void) {
    can2040_pio_irq_handler(&can2040_mp_instances[2].cbus);
}
#endif

static const irq_handler_t can2040_mp_irq_handlers[NUM_PIOS] = {
    can2040_mp_irq0,
    can2040_mp_irq1,
    #if NUM_PIOS > 2
    can2040_mp_irq2,
    #endif
};

/****************************************************************
 * Hardware start / stop
 ****************************************************************/

static void can2040_mp_stop_hw(can2040_mp_obj_t *self) {
    if (!self->running) {
        return;
    }
    // can2040_stop() is not IRQ-safe: it must not be preempted by
    // can2040_pio_irq_handler(), so silence the PIO IRQ first.
    irq_set_enabled(self->irq_num, false);
    can2040_stop(&self->cbus);

    irq_remove_handler(self->irq_num, can2040_mp_irq_handlers[self->pio_num]);
    if (self->prev_handler != NULL) {
        irq_set_exclusive_handler(self->irq_num, self->prev_handler);
        irq_set_enabled(self->irq_num, true);
        self->prev_handler = NULL;
    }

    PIO pio = pio_get_instance(self->pio_num);
    for (uint sm = 0; sm < 4; sm++) {
        if (pio_sm_is_claimed(pio, sm)) {
            pio_sm_unclaim(pio, sm);
        }
    }
    self->running = false;
}

static void can2040_mp_start_hw(can2040_mp_obj_t *self) {
    if (self->running) {
        return;
    }

    // can2040 uses all four state machines and the whole instruction memory
    // of its PIO block. Claim the state machines so rp2.StateMachine refuses them.
    PIO pio = pio_get_instance(self->pio_num);
    for (uint sm = 0; sm < 4; sm++) {
        if (pio_sm_is_claimed(pio, sm)) {
            mp_raise_msg_varg(&mp_type_OSError,
                MP_ERROR_TEXT("PIO%u state machine %u is already in use"), (unsigned)self->pio_num, sm);
        }
    }
    if (irq_has_shared_handler(self->irq_num)) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("PIO%u IRQ has a shared handler installed"), (unsigned)self->pio_num);
    }
    for (uint sm = 0; sm < 4; sm++) {
        pio_sm_claim(pio, sm);
    }

    // Reset receive state.
    self->rx_push_pos = 0;
    self->rx_pull_pos = 0;
    self->rx_dropped = 0;
    self->rx_overflow = 0;
    self->cb_scheduled = false;

    // Take over the PIO IRQ (MicroPython's rp2 module may own it).
    irq_set_enabled(self->irq_num, false);
    self->prev_handler = irq_get_exclusive_handler(self->irq_num);
    if (self->prev_handler != NULL) {
        irq_remove_handler(self->irq_num, self->prev_handler);
    }
    irq_set_exclusive_handler(self->irq_num, can2040_mp_irq_handlers[self->pio_num]);
    irq_set_priority(self->irq_num, PICO_HIGHEST_IRQ_PRIORITY);

    can2040_setup(&self->cbus, self->pio_num);
    can2040_callback_config(&self->cbus, can2040_mp_cb);

    irq_set_enabled(self->irq_num, true);
    can2040_start(&self->cbus, clock_get_hz(clk_sys), self->bitrate, self->gpio_rx, self->gpio_tx);
    self->running = true;
}

// Called from the port's soft-reset hook (see can2040_mp_hooks.h): release
// hardware and forget heap references before the heap is torn down.
void can2040_mp_soft_reset(void) {
    for (uint32_t i = 0; i < NUM_PIOS; i++) {
        can2040_mp_stop_hw(&can2040_mp_instances[i]);
        can2040_mp_instances[i].cb_scheduled = false;
        MP_STATE_PORT(can2040_mp_rx_callback)[i] = MP_OBJ_NULL;
    }
}

/****************************************************************
 * Helpers
 ****************************************************************/

static bool can2040_mp_pop(can2040_mp_obj_t *self, struct can2040_msg *msg) {
    if (self->rx_pull_pos == self->rx_push_pos) {
        return false;
    }
    *msg = self->rx_queue[self->rx_pull_pos & CAN2040_MP_RX_QUEUE_MASK];
    self->rx_pull_pos++;
    return true;
}

static void can2040_mp_msg_to_objs(const struct can2040_msg *msg, mp_obj_t *out) {
    uint32_t dlc = msg->dlc > 8 ? 8 : msg->dlc;
    out[0] = mp_obj_new_int_from_uint(msg->id & CAN2040_MP_EXT_ID_MAX);
    out[1] = mp_obj_new_bytes(msg->data, dlc);
    out[2] = mp_obj_new_bool(msg->id & CAN2040_ID_RTR);
    out[3] = mp_obj_new_bool(msg->id & CAN2040_ID_EFF);
}

// Scheduled in normal context: drain the queue into the Python callback.
static mp_obj_t can2040_mp_dispatch(mp_obj_t self_in) {
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // Clear first so frames arriving while we drain schedule another pass.
    self->cb_scheduled = false;
    struct can2040_msg msg;
    while (can2040_mp_pop(self, &msg)) {
        mp_obj_t cb = MP_STATE_PORT(can2040_mp_rx_callback)[self->pio_num];
        if (cb == MP_OBJ_NULL || cb == mp_const_none) {
            break;
        }
        mp_obj_t args[4];
        can2040_mp_msg_to_objs(&msg, args);
        mp_call_function_n_kw(cb, 4, 0, args);
    }
    return mp_const_none;
}

static void can2040_mp_check_running(can2040_mp_obj_t *self) {
    if (!self->running) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN2040 not started"));
    }
}

/****************************************************************
 * Python type
 ****************************************************************/

static mp_obj_t can2040_mp_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pio, ARG_rx, ARG_tx, ARG_bitrate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pio, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_rx, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_tx, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_bitrate, MP_ARG_INT, {.u_int = 500000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t pio_num = args[ARG_pio].u_int;
    mp_int_t rx = args[ARG_rx].u_int;
    mp_int_t tx = args[ARG_tx].u_int;
    mp_int_t bitrate = args[ARG_bitrate].u_int;

    // NUM_PIOS / NUM_BANK0_GPIOS are unsigned literals; compare as signed so -1 is not promoted.
    if (pio_num < 0 || pio_num >= (mp_int_t)NUM_PIOS) {
        mp_raise_ValueError(MP_ERROR_TEXT("pio out of range"));
    }
    if (rx < 0 || rx >= (mp_int_t)NUM_BANK0_GPIOS) {
        mp_raise_ValueError(MP_ERROR_TEXT("rx must be a valid GPIO number"));
    }
    if (tx < -1 || tx >= (mp_int_t)NUM_BANK0_GPIOS || tx == rx) {
        mp_raise_ValueError(MP_ERROR_TEXT("tx must be a valid GPIO number, or -1 for listen-only"));
    }
    if (bitrate < 10000 || bitrate > 1000000) {
        mp_raise_ValueError(MP_ERROR_TEXT("bitrate must be 10000..1000000"));
    }

    can2040_mp_obj_t *self = &can2040_mp_instances[pio_num];
    can2040_mp_stop_hw(self);   // reconfiguring a live instance stops it first
    self->base.type = type;
    self->pio_num = pio_num;
    self->irq_num = can2040_mp_irq_nums[pio_num];
    self->gpio_rx = rx;
    self->gpio_tx = tx;
    self->bitrate = bitrate;
    return MP_OBJ_FROM_PTR(self);
}

static void can2040_mp_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "CAN2040(pio=%u, rx=%d, tx=%d, bitrate=%u, running=%s)",
        (unsigned)self->pio_num, (int)self->gpio_rx, (int)self->gpio_tx,
        (unsigned)self->bitrate, self->running ? "True" : "False");
}

static mp_obj_t can2040_mp_start(mp_obj_t self_in) {
    can2040_mp_start_hw(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_start_obj, can2040_mp_start);

static mp_obj_t can2040_mp_stop(mp_obj_t self_in) {
    can2040_mp_stop_hw(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_stop_obj, can2040_mp_stop);

static mp_obj_t can2040_mp_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_id, ARG_data, ARG_rtr, ARG_ext };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_data, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_rtr, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_ext, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    can2040_mp_check_running(self);
    if (self->gpio_tx < 0) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN2040 is listen-only (tx=-1)"));
    }

    mp_int_t id = args[ARG_id].u_int;
    bool ext = args[ARG_ext].u_bool;
    if (id < 0 || id > (ext ? CAN2040_MP_EXT_ID_MAX : CAN2040_MP_STD_ID_MAX)) {
        mp_raise_ValueError(MP_ERROR_TEXT("CAN id out of range"));
    }

    struct can2040_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id = id | (args[ARG_rtr].u_bool ? CAN2040_ID_RTR : 0) | (ext ? CAN2040_ID_EFF : 0);
    if (args[ARG_data].u_obj != mp_const_none) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[ARG_data].u_obj, &bufinfo, MP_BUFFER_READ);
        if (bufinfo.len > 8) {
            mp_raise_ValueError(MP_ERROR_TEXT("data must be at most 8 bytes"));
        }
        msg.dlc = bufinfo.len;
        memcpy(msg.data, bufinfo.buf, bufinfo.len);
    }
    return mp_obj_new_bool(can2040_transmit(&self->cbus, &msg) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(can2040_mp_send_obj, 2, can2040_mp_send);

static mp_obj_t can2040_mp_check_transmit(mp_obj_t self_in) {
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    can2040_mp_check_running(self);
    return mp_obj_new_bool(can2040_check_transmit(&self->cbus));
}
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_check_transmit_obj, can2040_mp_check_transmit);

static mp_obj_t can2040_mp_recv(mp_obj_t self_in) {
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    struct can2040_msg msg;
    if (!can2040_mp_pop(self, &msg)) {
        return mp_const_none;
    }
    mp_obj_t items[4];
    can2040_mp_msg_to_objs(&msg, items);
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_recv_obj, can2040_mp_recv);

static mp_obj_t can2040_mp_any(mp_obj_t self_in) {
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(self->rx_push_pos - self->rx_pull_pos);
}
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_any_obj, can2040_mp_any);

static mp_obj_t can2040_mp_on_receive(mp_obj_t self_in, mp_obj_t cb) {
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable or None"));
    }
    MP_STATE_PORT(can2040_mp_rx_callback)[self->pio_num] = cb;
    // Frames may already be waiting; make sure they get delivered.
    if (cb != mp_const_none && self->rx_push_pos != self->rx_pull_pos && !self->cb_scheduled) {
        self->cb_scheduled = true;
        if (!mp_sched_schedule(MP_OBJ_FROM_PTR(&can2040_mp_dispatch_obj), self_in)) {
            self->cb_scheduled = false;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(can2040_mp_on_receive_obj, can2040_mp_on_receive);

static mp_obj_t can2040_mp_stats(mp_obj_t self_in) {
    can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    struct can2040_stats st;
    memset(&st, 0, sizeof(st));
    if (self->running) {
        can2040_get_statistics(&self->cbus, &st);
    }
    mp_obj_t dict = mp_obj_new_dict(6);
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_rx), mp_obj_new_int_from_uint(st.rx_total));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_tx), mp_obj_new_int_from_uint(st.tx_total));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_tx_attempts), mp_obj_new_int_from_uint(st.tx_attempt));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_parse_errors), mp_obj_new_int_from_uint(st.parse_error));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_rx_dropped), mp_obj_new_int_from_uint(self->rx_dropped));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_rx_overflow), mp_obj_new_int_from_uint(self->rx_overflow));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(can2040_mp_stats_obj, can2040_mp_stats);

static void can2040_mp_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (attr == MP_QSTR_running && dest[0] == MP_OBJ_NULL) {
        can2040_mp_obj_t *self = MP_OBJ_TO_PTR(self_in);
        dest[0] = mp_obj_new_bool(self->running);
        return;
    }
    dest[1] = MP_OBJ_SENTINEL;   // continue lookup in locals_dict
}

static const mp_rom_map_elem_t can2040_mp_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&can2040_mp_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&can2040_mp_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&can2040_mp_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_transmit), MP_ROM_PTR(&can2040_mp_check_transmit_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&can2040_mp_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_any), MP_ROM_PTR(&can2040_mp_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_receive), MP_ROM_PTR(&can2040_mp_on_receive_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&can2040_mp_stats_obj) },
};
static MP_DEFINE_CONST_DICT(can2040_mp_locals_dict, can2040_mp_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    can2040_mp_type,
    MP_QSTR_CAN2040,
    MP_TYPE_FLAG_NONE,
    make_new, can2040_mp_make_new,
    print, can2040_mp_print,
    attr, can2040_mp_attr,
    locals_dict, &can2040_mp_locals_dict
    );

/****************************************************************
 * Module
 ****************************************************************/

static const mp_rom_map_elem_t can2040_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_can2040) },
    { MP_ROM_QSTR(MP_QSTR_CAN2040), MP_ROM_PTR(&can2040_mp_type) },
    { MP_ROM_QSTR(MP_QSTR_RX_QUEUE_SIZE), MP_ROM_INT(CAN2040_MP_RX_QUEUE_SIZE) },
    { MP_ROM_QSTR(MP_QSTR_TX_QUEUE_SIZE), MP_ROM_INT(4) },
};
static MP_DEFINE_CONST_DICT(can2040_module_globals, can2040_module_globals_table);

const mp_obj_module_t can2040_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&can2040_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_can2040, can2040_user_cmodule);
