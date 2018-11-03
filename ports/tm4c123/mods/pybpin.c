/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2015 Daniel Campora
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pybpin.h>

#include "pins.h"



/// \moduleref pyb
/// \class Pin - control I/O pins
///
/******************************************************************************
DECLARE PRIVATE FUNCTIONS
******************************************************************************/
STATIC pin_obj_t *pin_find_named_pin(const mp_obj_dict_t *named_pins, mp_obj_t name);
STATIC pin_obj_t *pin_find_pin_by_port_bit (const mp_obj_dict_t *named_pins, uint port, uint bit);
STATIC int8_t pin_obj_find_af (const pin_obj_t* pin, uint8_t fn, uint8_t unit, uint8_t type);
STATIC void pin_free_af_from_pins (uint8_t fn, uint8_t unit, uint8_t type);
STATIC void pin_deassign (pin_obj_t* pin);
STATIC void pin_obj_configure (const pin_obj_t *self);
// STATIC void pin_get_hibernate_pin_and_idx (const pin_obj_t *self, uint *wake_pin, uint *idx);
STATIC void pin_irq_enable (mp_obj_t self_in);
STATIC void pin_irq_disable (mp_obj_t self_in);
STATIC void pin_extint_register(pin_obj_t *self, uint32_t intmode, uint32_t priority);
STATIC void pin_validate_mode (uint mode);
STATIC void pin_validate_pull (uint pull);
STATIC void pin_validate_drive (uint strength);
STATIC void pin_validate_af(const pin_obj_t* pin, int8_t idx, uint8_t *fn, uint8_t *unit, uint8_t *type);
STATIC uint8_t pin_get_value(const pin_obj_t* self);
STATIC void GPIOAIntHandler (void);
STATIC void GPIOBIntHandler (void);
STATIC void GPIOCIntHandler (void);
STATIC void GPIODIntHandler (void);
STATIC void GPIOEIntHandler (void);
STATIC void GPIOFIntHandler (void);
STATIC void EXTI_Handler(uint port);

/******************************************************************************
DEFINE CONSTANTS
******************************************************************************/
#define PYBPIN_NUM_WAKE_PINS            (1)
#define PYBPIN_WAKES_NOT                (-1)

// #define GPIO_DIR_MODE_ALT               0x00000002  // Pin is NOT controlled by the PGIO module
// #define GPIO_DIR_MODE_ALT_OD            0x00000003  // Pin is NOT controlled by the PGIO module and is in open drain mode

#define PYB_PIN_FALLING_EDGE            0x01
#define PYB_PIN_RISING_EDGE             0x02
#define PYB_PIN_LOW_LEVEL               0x04
#define PYB_PIN_HIGH_LEVEL              0x08

/******************************************************************************
DEFINE TYPES
******************************************************************************/
typedef struct {
    bool       active;
    int8_t     lpds;
    int8_t     hib;
} pybpin_wake_pin_t;

/******************************************************************************
DECLARE PRIVATE DATA
******************************************************************************/
STATIC const mp_irq_methods_t pin_irq_methods;
//STATIC pybpin_wake_pin_t pybpin_wake_pin[PYBPIN_NUM_WAKE_PINS] =
//                                    { {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT}} ;

/******************************************************************************
 DEFINE PUBLIC FUNCTIONS
 ******************************************************************************/
void pin_init0(void) {
// this initalization also reconfigures the JTAG/SWD pins
#ifndef DEBUG
    // assign all pins to the GPIO module so that peripherals can be connected to any
    // pins without conflicts after a soft reset
    const mp_map_t *named_map = &pin_board_pins_locals_dict.map;
    for (uint i = 0; i < named_map->used - 1; i++) {
        pin_obj_t * pin = (pin_obj_t *)named_map->table[i].value;
        pin_deassign (pin);
    }
#endif
}

// C API used to convert a user-supplied pin name into an ordinal pin number.
pin_obj_t *pin_find(mp_obj_t user_obj) {
    pin_obj_t *pin_obj;

    // if a pin was provided, use it
    if (MP_OBJ_IS_TYPE(user_obj, &pin_type)) {
        pin_obj = user_obj;
        return pin_obj;
    }

    // otherwise see if the pin name matches a cpu pin
    pin_obj = pin_find_named_pin(&pin_board_pins_locals_dict, user_obj);
    if (pin_obj) {
        return pin_obj;
    }

    mp_raise_ValueError(mpexception_value_invalid_arguments);
}

void pin_config (pin_obj_t *self, int af, uint mode, uint pull, int value, uint strength) {
    self->mode = mode, self->pull = pull, self->strength = strength;
    // if af is -1, then we want to keep it as it is
    if (af != -1) {
        self->af = af;
    }

    // if value is -1, then we want to keep it as it is
    if (value != -1) {
        self->value = value;
    }

    // mark the pin as used
    self->used = true;
    pin_obj_configure ((const pin_obj_t *)self);

    // register it with the sleep module
    pyb_sleep_add ((const mp_obj_t)self, (WakeUpCB_t)pin_obj_configure);
}

void pin_assign_pins_af (mp_obj_t *pins, uint32_t n_pins, uint32_t pull, uint32_t fn, uint32_t unit) {
    for (int i = 0; i < n_pins; i++) {
        pin_free_af_from_pins(fn, unit, i);
        if (pins[i] != mp_const_none) {
            pin_obj_t *pin = pin_find(pins[i]);
            pin_config (pin, pin_find_af_index(pin, fn, unit, i), 0, pull, -1, GPIO_STRENGTH_2MA);
        }
    }
}

uint8_t pin_find_peripheral_unit (const mp_obj_t pin, uint8_t fn, uint8_t type) {
    pin_obj_t *pin_o = pin_find(pin);
    for (int i = 0; i < pin_o->num_afs; i++) {
        if (pin_o->af_list[i].fn == fn && pin_o->af_list[i].type == type) {
            return pin_o->af_list[i].unit;
        }
    }
    mp_raise_ValueError(mpexception_value_invalid_arguments);
}

uint8_t pin_find_peripheral_type (const mp_obj_t pin, uint8_t fn, uint8_t unit) {
    pin_obj_t *pin_o = pin_find(pin);
    for (int i = 0; i < pin_o->num_afs; i++) {
        if (pin_o->af_list[i].fn == fn && pin_o->af_list[i].unit == unit) {
            return pin_o->af_list[i].type;
        }
    }
    mp_raise_ValueError(mpexception_value_invalid_arguments);
}

int8_t pin_find_af_index (const pin_obj_t* pin, uint8_t fn, uint8_t unit, uint8_t type) {
    int8_t af = pin_obj_find_af(pin, fn, unit, type);
    if (af < 0) {
        mp_raise_ValueError(mpexception_value_invalid_arguments);
    }
    return af;
}

/******************************************************************************
DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/
STATIC pin_obj_t *pin_find_named_pin(const mp_obj_dict_t *named_pins, mp_obj_t name) {
    const mp_map_t *named_map = &named_pins->map;
    mp_map_elem_t *named_elem = mp_map_lookup((mp_map_t*)named_map, name, MP_MAP_LOOKUP);
    if (named_elem != NULL && named_elem->value != NULL) {
        return named_elem->value;
    }
    return NULL;
}

STATIC pin_obj_t *pin_find_pin_by_port_bit (const mp_obj_dict_t *named_pins, uint port, uint bit) {
    const mp_map_t *named_map = &named_pins->map;
    for (uint i = 0; i < named_map->used; i++) {
        if ((((pin_obj_t *)named_map->table[i].value)->port == port) &&
                (((pin_obj_t *)named_map->table[i].value)->bit == bit)) {
            return named_map->table[i].value;
        }
    }
    return NULL;
}

STATIC int8_t pin_obj_find_af (const pin_obj_t* pin, uint8_t fn, uint8_t unit, uint8_t type) {
    for (int i = 0; i < pin->num_afs; i++) {
        if (pin->af_list[i].fn == fn && pin->af_list[i].unit == unit && pin->af_list[i].type == type) {
            return pin->af_list[i].idx;
        }
    }
    return -1;
}

STATIC void pin_free_af_from_pins (uint8_t fn, uint8_t unit, uint8_t type) {
    const mp_map_t *named_map = &pin_board_pins_locals_dict.map;
    for (uint i = 0; i < named_map->used - 1; i++) {
        pin_obj_t * pin = (pin_obj_t *)named_map->table[i].value;
        // af is different than GPIO
        if (pin->af > GPIO_DIR_MODE_IN) {
            // check if the pin supports the target af
            int af = pin_obj_find_af(pin, fn, unit, type);
            if (af > 0 && af == pin->af) {
                // the pin supports the target af, de-assign it
                pin_deassign (pin);
            }
        }
    }
}

STATIC void pin_deassign (pin_obj_t* pin) {
    pin_config (pin, 0, GPIO_DIR_MODE_IN, GPIO_PIN_TYPE_STD, -1, GPIO_STRENGTH_2MA);
    pin->used = false;
}

STATIC void pin_obj_configure (const pin_obj_t *self) {
    uint32_t type;
    if (self->mode == GPIO_PIN_TYPE_ANALOG) {
        type = GPIO_PIN_TYPE_ANALOG;
    } else {
        type = self->pull;
        uint32_t direction = self->mode;
        if (direction == GPIO_PIN_TYPE_OD || direction == GPIO_DIR_MODE_HW) {
            direction = GPIO_DIR_MODE_OUT;
            type |= GPIO_PIN_TYPE_OD;
        }
        if (self->mode != GPIO_DIR_MODE_HW) {
            // enable the peripheral clock for the GPIO port of this pin
            MAP_SysCtlPeripheralEnable(self->port);
            // configure the direction
            MAP_GPIODirModeSet(self->port, self->bit, direction);
            // set the pin value
            if (self->value) {
                MAP_GPIOPinWrite(self->port, self->bit, self->bit);
            } else {
                MAP_GPIOPinWrite(self->port, self->bit, 0);
            }
        }
        // now set the alternate function
        MAP_GPIODirModeSet(self->port, self->pin_num, GPIO_DIR_MODE_HW);
    }
    MAP_GPIOPadConfigSet(self->port, self->pin_num, self->strength, type);

}

//STATIC void pin_get_hibernate_pin_and_idx (const pin_obj_t *self, uint *hib_pin, uint *idx) {
//    // pin_num is actually : (package_pin - 1)
//    switch (self->pin_num) {
//    case 31:    // GP2
//        *hib_pin = 31; //switch 2 & con to pf0
//        *idx = 0;
//        break;
//    default:
//        *idx = 0xFF;
//        break;
//    }
//}

STATIC void pin_irq_enable (mp_obj_t self_in) {
    const pin_obj_t *self = self_in;
    MAP_GPIOIntClear(self->port, self->bit);
    MAP_GPIOIntEnable(self->port, self->bit);
}

STATIC void pin_irq_disable (mp_obj_t self_in) {
    const pin_obj_t *self = self_in;
    MAP_GPIOIntDisable(self->port, self->bit);
}

STATIC int pin_irq_flags (mp_obj_t self_in) {
    const pin_obj_t *self = self_in;
    return self->irq_flags;
}

STATIC void pin_extint_register(pin_obj_t *self, uint32_t intmode, uint32_t priority) {
    void *handler;
    uint32_t intnum;

    // configure the interrupt type
    MAP_GPIOIntTypeSet(self->port, self->pin_num, intmode);
    switch (self->port) {
    case PORT_A:
        handler = GPIOAIntHandler;
        intnum = INT_GPIOA;
        break;
    case PORT_B:
        handler = GPIOBIntHandler;
        intnum = INT_GPIOB;
        break;
    case PORT_C:
        handler = GPIOCIntHandler;
        intnum = INT_GPIOC;
        break;
    case PORT_D:
        handler = GPIODIntHandler;
        intnum = INT_GPIOD;
        break;
    case PORT_E:
        handler = GPIOEIntHandler;
        intnum = INT_GPIOE;
        break;
    case PORT_F:
        handler = GPIOFIntHandler;
        intnum = INT_GPIOF;
        break;
    default:
        handler = NULL;
        intnum = 0;
        break;
    }
    GPIOIntRegister(self->port, handler);
    // MAP_GPIOIntRegister(self->port, handler);
    // set the interrupt to the lowest priority, to make sure that
    // no other ISRs will be preemted by this one
    MAP_IntPrioritySet(intnum, priority);
}

STATIC void pin_validate_mode (uint mode) {
    if (mode != GPIO_DIR_MODE_IN && mode != GPIO_DIR_MODE_OUT && mode != GPIO_PIN_TYPE_OD &&
        mode != GPIO_DIR_MODE_HW) {
        mp_raise_ValueError(mpexception_value_invalid_arguments);
    }
}
STATIC void pin_validate_pull (uint pull) {
    if (pull != GPIO_PIN_TYPE_STD && pull != GPIO_PIN_TYPE_STD_WPU && pull != GPIO_PIN_TYPE_STD_WPD) {
        mp_raise_ValueError(mpexception_value_invalid_arguments);
    }
}

STATIC void pin_validate_drive(uint strength) {
    if (strength != GPIO_STRENGTH_2MA && strength != GPIO_STRENGTH_4MA && strength != GPIO_STRENGTH_6MA) {
        mp_raise_ValueError(mpexception_value_invalid_arguments);
    }
}

STATIC void pin_validate_af(const pin_obj_t* pin, int8_t idx, uint8_t *fn, uint8_t *unit, uint8_t *type) {
    for (int i = 0; i < pin->num_afs; i++) {
        if (pin->af_list[i].idx == idx) {
            *fn = pin->af_list[i].fn;
            *unit = pin->af_list[i].unit;
            *type = pin->af_list[i].type;
            return;
        }
    }
    mp_raise_ValueError(mpexception_value_invalid_arguments);
}

STATIC uint8_t pin_get_value (const pin_obj_t* self) {
    uint32_t value;
    bool setdir = false;
    if (self->mode == GPIO_PIN_TYPE_OD) {
        setdir = true;
        // configure the direction to IN for a moment in order to read the pin value
        MAP_GPIODirModeSet(self->port, self->bit, GPIO_DIR_MODE_IN);
    }
    // now get the value
    value = MAP_GPIOPinRead(self->port, self->bit);
    if (setdir) {
        // set the direction back to output
        MAP_GPIODirModeSet(self->port, self->bit, GPIO_DIR_MODE_OUT);
        if (self->value) {
            MAP_GPIOPinWrite(self->port, self->bit, self->bit);
        } else {
            MAP_GPIOPinWrite(self->port, self->bit, 0);
        }
    }
    // return it
    return value ? 1 : 0;
}

STATIC void GPIOAIntHandler (void) {
    EXTI_Handler(PORT_A);
}

STATIC void GPIOBIntHandler (void) {
    EXTI_Handler(PORT_B);
}

STATIC void GPIOCIntHandler (void) {
    EXTI_Handler(PORT_C);
}

STATIC void GPIODIntHandler (void) {
    EXTI_Handler(PORT_D);
}

STATIC void GPIOEIntHandler (void) {
    EXTI_Handler(PORT_E);
}

STATIC void GPIOFIntHandler (void) {
    EXTI_Handler(PORT_F);
}

// common interrupt handler
STATIC void EXTI_Handler(uint port) {
    uint32_t bits = MAP_GPIOIntStatus(port, true);
    MAP_GPIOIntClear(port, bits);

    // might be that we have more than one pin interrupt pending
    // therefore we must loop through all of the 8 possible bits
    for (int i = 0; i < 8; i++) {
        uint32_t bit = (1 << i);
        if (bit & bits) {
            pin_obj_t *self = (pin_obj_t *)pin_find_pin_by_port_bit(&pin_board_pins_locals_dict, port, bit);
            if (self->irq_trigger == (PYB_PIN_FALLING_EDGE | PYB_PIN_RISING_EDGE)) {
                // read the pin value (hoping that the pin level has remained stable)
                self->irq_flags = MAP_GPIOPinRead(self->port, self->bit) ? PYB_PIN_RISING_EDGE : PYB_PIN_FALLING_EDGE;
            } else {
                // same as the triggers
                self->irq_flags = self->irq_trigger;
            }
            mp_irq_handler(mp_irq_find(self));
            // always clear the flags after leaving the user handler
            self->irq_flags = 0;
        }
    }
}


/******************************************************************************/
// MicroPython bindings

STATIC const mp_arg_t pin_init_args[] = {
    { MP_QSTR_mode,                        MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_pull,                        MP_ARG_OBJ, {.u_obj = mp_const_none} },
    { MP_QSTR_value,    MP_ARG_KW_ONLY  |  MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_drive,    MP_ARG_KW_ONLY  |  MP_ARG_INT, {.u_int = GPIO_STRENGTH_4MA} },
    { MP_QSTR_alt,      MP_ARG_KW_ONLY  |  MP_ARG_INT, {.u_int = -1} },
};
#define pin_INIT_NUM_ARGS MP_ARRAY_SIZE(pin_init_args)

STATIC mp_obj_t pin_obj_init_helper(pin_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // parse args
    mp_arg_val_t args[pin_INIT_NUM_ARGS];
    mp_arg_parse_all(n_args, pos_args, kw_args, pin_INIT_NUM_ARGS, pin_init_args, args);

    // get the io mode
    uint mode;
    //  default is input
    if (args[0].u_obj == MP_OBJ_NULL) {
        mode = GPIO_DIR_MODE_IN;
    } else {
        mode = mp_obj_get_int(args[0].u_obj);
        pin_validate_mode (mode);
    }

    // get the pull type
    uint pull;
    if (args[1].u_obj == mp_const_none) {
        pull = GPIO_PIN_TYPE_STD;
    } else {
        pull = mp_obj_get_int(args[1].u_obj);
        pin_validate_pull (pull);
    }

    // get the value
    int value = -1;
    if (args[2].u_obj != MP_OBJ_NULL) {
        if (mp_obj_is_true(args[2].u_obj)) {
            value = 1;
        } else {
            value = 0;
        }
    }

    // get the strenght
    uint strength = args[3].u_int;
    pin_validate_drive(strength);

    // get the alternate function
    int af = args[4].u_int;
    if (mode != GPIO_DIR_MODE_HW) {
        if (af == -1) {
            af = 0;
        } else {
            goto invalid_args;
        }
    } else if (af < -1 || af > 15) {
        goto invalid_args;
    }

    // check for a valid af and then free it from any other pins
    if (af > 0) {
        uint8_t fn, unit, type;
        pin_validate_af (self, af, &fn, &unit, &type);
        pin_free_af_from_pins(fn, unit, type);
    }
    pin_config (self, af, mode, pull, value, strength);

    return mp_const_none;

invalid_args:
    mp_raise_ValueError(mpexception_value_invalid_arguments);
}

STATIC void pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pin_obj_t *self = self_in;
    uint32_t pull = self->pull;
    uint32_t drive = self->strength;

    // pin name
    mp_printf(print, "Pin('%q'", self->name);

    // pin mode
    qstr mode_qst;
    uint32_t mode = self->mode;
    if (mode == GPIO_DIR_MODE_IN) {
        mode_qst = MP_QSTR_IN;
    } else if (mode == GPIO_DIR_MODE_OUT) {
        mode_qst = MP_QSTR_OUT;
    } else if (mode == GPIO_DIR_MODE_HW) {
        mode_qst = MP_QSTR_ALT;
    } else {
        mode_qst = MP_QSTR_OPEN_DRAIN;
    }
    mp_printf(print, ", mode=Pin.%q", mode_qst);

    // pin pull
    qstr pull_qst;
    if (pull == GPIO_PIN_TYPE_STD) {
        mp_printf(print, ", pull=%q", MP_QSTR_None);
    } else {
        if (pull == GPIO_PIN_TYPE_STD_WPU) {
            pull_qst = MP_QSTR_PULL_UP;
        } else {
            pull_qst = MP_QSTR_PULL_DOWN;
        }
        mp_printf(print, ", pull=Pin.%q", pull_qst);
    }

    // pin drive
    qstr drv_qst;
    if (drive == GPIO_STRENGTH_2MA) {
        drv_qst = MP_QSTR_LOW_POWER;
    } else if (drive == GPIO_STRENGTH_4MA) {
        drv_qst = MP_QSTR_MED_POWER;
    } else {
        drv_qst = MP_QSTR_HIGH_POWER;
    }
    mp_printf(print, ", drive=Pin.%q", drv_qst);

    // pin af
    int alt = (self->af == 0) ? -1 : self->af;
    mp_printf(print, ", alt=%d)", alt);
}

STATIC mp_obj_t pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    // Run an argument through the mapper and return the result.
    pin_obj_t *pin = (pin_obj_t *)pin_find(args[0]);

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    pin_obj_init_helper(pin, n_args - 1, args + 1, &kw_args);

    return (mp_obj_t)pin;
}

STATIC mp_obj_t pin_obj_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return pin_obj_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
MP_DEFINE_CONST_FUN_OBJ_KW(pin_init_obj, 1, pin_obj_init);

STATIC mp_obj_t pin_value(size_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        // get the value
        return MP_OBJ_NEW_SMALL_INT(pin_get_value(self));
    } else {
        // set the pin value
        if (mp_obj_is_true(args[1])) {
            self->value = 1;
            MAP_GPIOPinWrite(self->port, self->bit, self->bit);
        } else {
            self->value = 0;
            MAP_GPIOPinWrite(self->port, self->bit, 0);
        }
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_value_obj, 1, 2, pin_value);

STATIC mp_obj_t pin_id(mp_obj_t self_in) {
    pin_obj_t *self = self_in;
    return MP_OBJ_NEW_QSTR(self->name);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pin_id_obj, pin_id);

STATIC mp_obj_t pin_mode(size_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        return mp_obj_new_int(self->mode);
    } else {
        uint32_t mode = mp_obj_get_int(args[1]);
        pin_validate_mode (mode);
        self->mode = mode;
        pin_obj_configure(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_mode_obj, 1, 2, pin_mode);

STATIC mp_obj_t pin_pull(size_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        if (self->pull == GPIO_PIN_TYPE_STD) {
            return mp_const_none;
        }
        return mp_obj_new_int(self->pull);
    } else {
        uint32_t pull;
        if (args[1] == mp_const_none) {
            pull = GPIO_PIN_TYPE_STD;
        } else {
            pull = mp_obj_get_int(args[1]);
            pin_validate_pull (pull);
        }
        self->pull = pull;
        pin_obj_configure(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_pull_obj, 1, 2, pin_pull);

STATIC mp_obj_t pin_drive(size_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        return mp_obj_new_int(self->strength);
    } else {
        uint32_t strength = mp_obj_get_int(args[1]);
        pin_validate_drive (strength);
        self->strength = strength;
        pin_obj_configure(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_drive_obj, 1, 2, pin_drive);

STATIC mp_obj_t pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    mp_obj_t _args[2] = {self_in, *args};
    return pin_value (n_args + 1, _args);
}

STATIC mp_obj_t pin_alt_list(mp_obj_t self_in) {
    pin_obj_t *self = self_in;
    mp_obj_t af[2];
    mp_obj_t afs = mp_obj_new_list(0, NULL);

    for (int i = 0; i < self->num_afs; i++) {
        af[0] = MP_OBJ_NEW_QSTR(self->af_list[i].name);
        af[1] = mp_obj_new_int(self->af_list[i].idx);
        mp_obj_list_append(afs, mp_obj_new_tuple(MP_ARRAY_SIZE(af), af));
    }
    return afs;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pin_alt_list_obj, pin_alt_list);

/// \method irq(trigger, priority, handler, wake)
STATIC mp_obj_t pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[mp_irq_INIT_NUM_ARGS];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, mp_irq_INIT_NUM_ARGS, mp_irq_init_args, args);
    pin_obj_t *self = pos_args[0];

    // convert the priority to the correct value
    uint priority = mp_irq_translate_priority (args[1].u_int);

    // verify and translate the interrupt mode
    uint mp_trigger = mp_obj_get_int(args[0].u_obj);
    uint trigger;
    if (mp_trigger == (GPIO_FALLING_EDGE | GPIO_RISING_EDGE)) {
        trigger = GPIO_BOTH_EDGES;
    } else {
        switch (mp_trigger) {
        case PYB_PIN_FALLING_EDGE:
            trigger = GPIO_FALLING_EDGE;
            break;
        case PYB_PIN_RISING_EDGE:
            trigger = GPIO_RISING_EDGE;
            break;
        case PYB_PIN_LOW_LEVEL:
            trigger = GPIO_LOW_LEVEL;
            break;
        case PYB_PIN_HIGH_LEVEL:
            trigger = GPIO_HIGH_LEVEL;
            break;
        default:
            goto invalid_args;
        }
    }

    uint8_t pwrmode = (args[3].u_obj == mp_const_none) ? PYB_PWR_MODE_ACTIVE : mp_obj_get_int(args[3].u_obj);
    if (pwrmode > (PYB_PWR_MODE_ACTIVE | PYB_PWR_MODE_LPDS | PYB_PWR_MODE_HIBERNATE)) {
        goto invalid_args;
    }

    // we need to update the callback atomically, so we disable the
    // interrupt before we update anything.
    pin_irq_disable(self);
    if (pwrmode & PYB_PWR_MODE_ACTIVE) {
        // register the interrupt
        pin_extint_register((pin_obj_t *)self, trigger, priority);
    }

    // all checks have passed, we can create the irq object
    mp_obj_t _irq = mp_irq_new (self, args[2].u_obj, &pin_irq_methods);
    if (pwrmode & PYB_PWR_MODE_LPDS) {
        pyb_sleep_set_gpio_lpds_callback (_irq);
    }

    // save the mp_trigge for later
    self->irq_trigger = mp_trigger;

    // enable the interrupt just before leaving
    pin_irq_enable(self);

    return _irq;

invalid_args:
    mp_raise_ValueError(mpexception_value_invalid_arguments);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pin_irq_obj, 1, pin_irq);

STATIC const mp_rom_map_elem_t pin_locals_dict_table[] = {
    // instance methods
    { MP_ROM_QSTR(MP_QSTR_init),                    MP_ROM_PTR(&pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value),                   MP_ROM_PTR(&pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_id),                      MP_ROM_PTR(&pin_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode),                    MP_ROM_PTR(&pin_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_pull),                    MP_ROM_PTR(&pin_pull_obj) },
    { MP_ROM_QSTR(MP_QSTR_drive),                   MP_ROM_PTR(&pin_drive_obj) },
    { MP_ROM_QSTR(MP_QSTR_alt_list),                MP_ROM_PTR(&pin_alt_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq),                     MP_ROM_PTR(&pin_irq_obj) },

    // class attributes
    { MP_ROM_QSTR(MP_QSTR_board),                   MP_ROM_PTR(&pin_board_pins_obj_type) },

    // class constants
    { MP_ROM_QSTR(MP_QSTR_IN),                      MP_ROM_INT(GPIO_DIR_MODE_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT),                     MP_ROM_INT(GPIO_DIR_MODE_OUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN),              MP_ROM_INT(GPIO_PIN_TYPE_OD) },
    { MP_ROM_QSTR(MP_QSTR_ALT),                     MP_ROM_INT(GPIO_DIR_MODE_HW) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP),                 MP_ROM_INT(GPIO_PIN_TYPE_STD_WPU) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN),               MP_ROM_INT(GPIO_PIN_TYPE_STD_WPD) },
    { MP_ROM_QSTR(MP_QSTR_LOW_POWER),               MP_ROM_INT(GPIO_STRENGTH_2MA) },
    { MP_ROM_QSTR(MP_QSTR_MED_POWER),               MP_ROM_INT(GPIO_STRENGTH_4MA) },
    { MP_ROM_QSTR(MP_QSTR_HIGH_POWER),              MP_ROM_INT(GPIO_STRENGTH_6MA) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_FALLING),             MP_ROM_INT(PYB_PIN_FALLING_EDGE) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING),              MP_ROM_INT(PYB_PIN_RISING_EDGE) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_LOW_LEVEL),           MP_ROM_INT(PYB_PIN_LOW_LEVEL) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_HIGH_LEVEL),          MP_ROM_INT(PYB_PIN_HIGH_LEVEL) },
};

STATIC MP_DEFINE_CONST_DICT(pin_locals_dict, pin_locals_dict_table);

const mp_obj_type_t pin_type = {
    { &mp_type_type },
    .name = MP_QSTR_Pin,
    .print = pin_print,
    .make_new = pin_make_new,
    .call = pin_call,
    .locals_dict = (mp_obj_t)&pin_locals_dict,
};

STATIC const mp_irq_methods_t pin_irq_methods = {
    .init = pin_irq,
    .enable = pin_irq_enable,
    .disable = pin_irq_disable,
    .flags = pin_irq_flags,
};

STATIC void pin_named_pins_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pin_named_pins_obj_t *self = self_in;
    mp_printf(print, "<Pin.%q>", self->name);
}

const mp_obj_type_t pin_board_pins_obj_type = {
    { &mp_type_type },
    .name = MP_QSTR_board,
    .print = pin_named_pins_obj_print,
    .locals_dict = (mp_obj_t)&pin_board_pins_locals_dict,
};
