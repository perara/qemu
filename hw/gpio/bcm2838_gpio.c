/*
 * Raspberry Pi (BCM2838) GPIO Controller
 * This implementation is based on bcm2835_gpio (hw/gpio/bcm2835_gpio.c)
 *
 * Copyright (c) 2022 Auriga LLC
 *
 * Authors:
 *  Lotosh, Aleksey <aleksey.lotosh@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "hw/sd/sd.h"
#include "hw/gpio/bcm2838_gpio.h"
#include "hw/core/irq.h"

#define GPFSEL0   0x00
#define GPFSEL1   0x04
#define GPFSEL2   0x08
#define GPFSEL3   0x0C
#define GPFSEL4   0x10
#define GPFSEL5   0x14
#define GPSET0    0x1C
#define GPSET1    0x20
#define GPCLR0    0x28
#define GPCLR1    0x2C
#define GPLEV0    0x34
#define GPLEV1    0x38
#define GPEDS0    0x40
#define GPEDS1    0x44
#define GPREN0    0x4C
#define GPREN1    0x50
#define GPFEN0    0x58
#define GPFEN1    0x5C
#define GPHEN0    0x64
#define GPHEN1    0x68
#define GPLEN0    0x70
#define GPLEN1    0x74
#define GPAREN0   0x7C
#define GPAREN1   0x80
#define GPAFEN0   0x88
#define GPAFEN1   0x8C

#define GPIO_PUP_PDN_CNTRL_REG0 0xE4
#define GPIO_PUP_PDN_CNTRL_REG1 0xE8
#define GPIO_PUP_PDN_CNTRL_REG2 0xEC
#define GPIO_PUP_PDN_CNTRL_REG3 0xF0

#define RESET_VAL_CNTRL_REG0 0xAAA95555
#define RESET_VAL_CNTRL_REG1 0xA0AAAAAA
#define RESET_VAL_CNTRL_REG2 0x50AAA95A
#define RESET_VAL_CNTRL_REG3 0x00055555

#define NUM_FSELN_IN_GPFSELN 10
#define NUM_BITS_FSELN       3
#define MASK_FSELN           0x7

#define BYTES_IN_WORD        4

/* bcm,function property */
#define BCM2838_FSEL_GPIO_IN    0
#define BCM2838_FSEL_GPIO_OUT   1
#define BCM2838_FSEL_ALT5       2
#define BCM2838_FSEL_ALT4       3
#define BCM2838_FSEL_ALT0       4
#define BCM2838_FSEL_ALT1       5
#define BCM2838_FSEL_ALT2       6
#define BCM2838_FSEL_ALT3       7

static void gpio_update_output(BCM2838GpioState *s, unsigned int index);
static int gpfsel_is_out(BCM2838GpioState *s, int index);
static bool gpio_is_output(BCM2838GpioState *s, unsigned int index);
static bool gpio_level(BCM2838GpioState *s, unsigned int index);
static void gpio_update_event(BCM2838GpioState *s, unsigned int index,
                              bool old_level);
static void bcm2838_gpio_set(void *opaque, int index, int level);

static void gpio_bridge_write(BCM2838GpioState *s, const char *text)
{
    if (s->bridge_open) {
        qemu_chr_fe_write_all(&s->bridge_chr, (const uint8_t *)text,
                              strlen(text));
    }
}

static void gpio_bridge_hello(BCM2838GpioState *s)
{
    gpio_bridge_write(s, "RPI-GPIO 1 58\n");
}

static void gpio_bridge_emit_pin(BCM2838GpioState *s, unsigned int index)
{
    g_autofree char *line = NULL;

    if (!s->bridge_open) {
        return;
    }
    line = g_strdup_printf("PIN %u %u %u\n", index, gpio_level(s, index),
                           gpio_is_output(s, index));
    gpio_bridge_write(s, line);
}

static void gpio_bridge_emit_eeprom_nwp(BCM2838GpioState *s)
{
    g_autofree char *line = g_strdup_printf(
        "SIGNAL EEPROM_NWP %c\n",
        s->bridge_eeprom_nwp < 0 ? 'Z' : '0' + s->bridge_eeprom_nwp);

    gpio_bridge_write(s, line);
}

static void gpio_bridge_emit_sd_overcurrent(BCM2838GpioState *s)
{
    g_autofree char *line = g_strdup_printf(
        "SIGNAL SD_OVERCURRENT %c\n",
        s->bridge_sd_overcurrent < 0 ? 'Z' :
                                      '0' + s->bridge_sd_overcurrent);

    gpio_bridge_write(s, line);
}

static void gpio_bridge_release_inputs(BCM2838GpioState *s)
{
    unsigned int index;

    for (index = 0; index < BCM2838_GPIO_NUM; index++) {
        bcm2838_gpio_set(s, index, -1);
    }
    s->bridge_eeprom_nwp = -1;
    s->bridge_sd_overcurrent = -1;
}

static void gpio_bridge_handle_line(BCM2838GpioState *s, const char *line)
{
    unsigned int pin;
    char state;
    char extra;

    if (!strcmp(line, "PING")) {
        gpio_bridge_write(s, "PONG 1\n");
        return;
    }
    if (!strcmp(line, "GET ALL")) {
        for (pin = 0; pin < BCM2838_GPIO_NUM; pin++) {
            gpio_bridge_emit_pin(s, pin);
        }
        gpio_bridge_write(s, "END\n");
        return;
    }
    if (!strcmp(line, "GET SIGNALS")) {
        gpio_bridge_emit_eeprom_nwp(s);
        gpio_bridge_emit_sd_overcurrent(s);
        gpio_bridge_write(s, "END\n");
        return;
    }
    if (!strcmp(line, "GET SIGNAL EEPROM_NWP")) {
        gpio_bridge_emit_eeprom_nwp(s);
        return;
    }
    if (!strcmp(line, "GET SIGNAL SD_OVERCURRENT")) {
        gpio_bridge_emit_sd_overcurrent(s);
        return;
    }
    if (!strcmp(line, "RELEASE ALL")) {
        gpio_bridge_release_inputs(s);
        gpio_bridge_write(s, "OK\n");
        return;
    }
    if (sscanf(line, "GET %u %c", &pin, &extra) == 1) {
        if (pin >= BCM2838_GPIO_NUM) {
            gpio_bridge_write(s, "ERR pin-range\n");
        } else {
            gpio_bridge_emit_pin(s, pin);
        }
        return;
    }
    if (sscanf(line, "SET %u %c %c", &pin, &state, &extra) == 2) {
        if (pin >= BCM2838_GPIO_NUM) {
            gpio_bridge_write(s, "ERR pin-range\n");
        } else if (state != '0' && state != '1' && state != 'Z') {
            gpio_bridge_write(s, "ERR pin-state\n");
        } else {
            bcm2838_gpio_set(s, pin, state == 'Z' ? -1 : state - '0');
            gpio_bridge_write(s, "OK\n");
        }
        return;
    }
    if (sscanf(line, "SET SIGNAL EEPROM_NWP %c %c",
               &state, &extra) == 1) {
        if (state != '0' && state != '1' && state != 'Z') {
            gpio_bridge_write(s, "ERR signal-state\n");
        } else {
            s->bridge_eeprom_nwp = state == 'Z' ? -1 : state - '0';
            gpio_bridge_emit_eeprom_nwp(s);
            gpio_bridge_write(s, "OK\n");
        }
        return;
    }
    if (sscanf(line, "SET SIGNAL SD_OVERCURRENT %c %c",
               &state, &extra) == 1) {
        if (state != '0' && state != '1' && state != 'Z') {
            gpio_bridge_write(s, "ERR signal-state\n");
        } else {
            s->bridge_sd_overcurrent =
                state == 'Z' ? -1 : state - '0';
            gpio_bridge_emit_sd_overcurrent(s);
            gpio_bridge_write(s, "OK\n");
        }
        return;
    }
    gpio_bridge_write(s, "ERR syntax\n");
}

static int gpio_bridge_can_receive(void *opaque)
{
    return 1024;
}

static void gpio_bridge_receive(void *opaque, const uint8_t *buf, int size)
{
    BCM2838GpioState *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        uint8_t byte = buf[i];

        if (byte == '\n') {
            if (!s->bridge_discard_line && s->bridge_line_len) {
                s->bridge_line[s->bridge_line_len] = 0;
                gpio_bridge_handle_line(s, s->bridge_line);
            }
            s->bridge_line_len = 0;
            s->bridge_discard_line = false;
        } else if (byte == '\r') {
            continue;
        } else if (s->bridge_discard_line) {
            continue;
        } else if (byte < 0x20 || byte > 0x7e) {
            gpio_bridge_write(s, "ERR invalid-byte\n");
            s->bridge_discard_line = true;
        } else if (s->bridge_line_len + 1 >= sizeof(s->bridge_line)) {
            gpio_bridge_write(s, "ERR line-too-long\n");
            s->bridge_discard_line = true;
        } else {
            s->bridge_line[s->bridge_line_len++] = byte;
        }
    }
}

static void gpio_bridge_event(void *opaque, QEMUChrEvent event)
{
    BCM2838GpioState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->bridge_open = true;
        /*
         * An incoming destination must not expose reset defaults to a
         * physical bridge.  Post-load sends the banner only after migrated
         * direction, latch, input, pull, and event state has been restored.
         */
        if (!runstate_check(RUN_STATE_INMIGRATE)) {
            gpio_bridge_hello(s);
        }
        break;
    case CHR_EVENT_CLOSED:
        s->bridge_open = false;
        s->bridge_line_len = 0;
        s->bridge_discard_line = false;
        gpio_bridge_release_inputs(s);
        break;
    default:
        break;
    }
}

static uint32_t gpfsel_get(BCM2838GpioState *s, uint8_t reg)
{
    int i;
    uint32_t value = 0;
    for (i = 0; i < NUM_FSELN_IN_GPFSELN; i++) {
        uint32_t index = NUM_FSELN_IN_GPFSELN * reg + i;
        if (index < sizeof(s->fsel)) {
            value |= (s->fsel[index] & MASK_FSELN) << (NUM_BITS_FSELN * i);
        }
    }
    return value;
}

static void gpfsel_set(BCM2838GpioState *s, uint8_t reg, uint32_t value)
{
    int i;
    for (i = 0; i < NUM_FSELN_IN_GPFSELN; i++) {
        uint32_t index = NUM_FSELN_IN_GPFSELN * reg + i;
        if (index < sizeof(s->fsel)) {
            int fsel = (value >> (NUM_BITS_FSELN * i)) & MASK_FSELN;

            if (s->fsel[index] != fsel) {
                bool old_level = gpio_level(s, index);

                s->fsel[index] = fsel;
                gpio_update_output(s, index);
                gpio_update_event(s, index, old_level);
            }
        }
    }

    /* SD controller selection (48-53) */
    if (s->sd_fsel != BCM2838_FSEL_GPIO_IN
        && (s->fsel[48] == BCM2838_FSEL_GPIO_IN)
        && (s->fsel[49] == BCM2838_FSEL_GPIO_IN)
        && (s->fsel[50] == BCM2838_FSEL_GPIO_IN)
        && (s->fsel[51] == BCM2838_FSEL_GPIO_IN)
        && (s->fsel[52] == BCM2838_FSEL_GPIO_IN)
        && (s->fsel[53] == BCM2838_FSEL_GPIO_IN)
       ) {
        /* SDHCI controller selected */
        sdbus_reparent_card(s->sdbus_sdhost, s->sdbus_sdhci);
        s->sd_fsel = BCM2838_FSEL_GPIO_IN;
    } else if (s->sd_fsel != BCM2838_FSEL_ALT0
               && (s->fsel[48] == BCM2838_FSEL_ALT0) /* SD_CLK_R */
               && (s->fsel[49] == BCM2838_FSEL_ALT0) /* SD_CMD_R */
               && (s->fsel[50] == BCM2838_FSEL_ALT0) /* SD_DATA0_R */
               && (s->fsel[51] == BCM2838_FSEL_ALT0) /* SD_DATA1_R */
               && (s->fsel[52] == BCM2838_FSEL_ALT0) /* SD_DATA2_R */
               && (s->fsel[53] == BCM2838_FSEL_ALT0) /* SD_DATA3_R */
              ) {
        /* SDHost controller selected */
        sdbus_reparent_card(s->sdbus_sdhci, s->sdbus_sdhost);
        s->sd_fsel = BCM2838_FSEL_ALT0;
    }
}

static int gpfsel_is_out(BCM2838GpioState *s, int index)
{
    if (index >= 0 && index < BCM2838_GPIO_NUM) {
        return s->fsel[index] == 1;
    }
    return 0;
}

static int gpio_pwm_signal(BCM2838GpioState *s, unsigned int index)
{
    switch (index) {
    case 12:
        return s->fsel[index] == BCM2838_FSEL_ALT0 ? 0 : -1;
    case 13:
        return s->fsel[index] == BCM2838_FSEL_ALT0 ? 1 : -1;
    case 18:
        return s->fsel[index] == BCM2838_FSEL_ALT5 ? 0 : -1;
    case 19:
        return s->fsel[index] == BCM2838_FSEL_ALT5 ? 1 : -1;
    case 40:
        return s->fsel[index] == BCM2838_FSEL_ALT0 ? 2 : -1;
    case 41:
        return s->fsel[index] == BCM2838_FSEL_ALT0 ? 3 : -1;
    case 45:
        return s->fsel[index] == BCM2838_FSEL_ALT0 ? 1 : -1;
    default:
        return -1;
    }
}

static bool gpio_is_output(BCM2838GpioState *s, unsigned int index)
{
    return gpfsel_is_out(s, index) || gpio_pwm_signal(s, index) >= 0;
}

static bool gpio_word_bit(uint32_t low, uint32_t high, unsigned int index)
{
    return index < 32 ? extract32(low, index, 1) :
                        extract32(high, index - 32, 1);
}

static unsigned int gpio_pull(BCM2838GpioState *s, unsigned int index)
{
    return extract32(s->pup_cntrl_reg[index / 16], (index % 16) * 2, 2);
}

static bool gpio_level(BCM2838GpioState *s, unsigned int index)
{
    int pwm_signal = gpio_pwm_signal(s, index);

    if (gpfsel_is_out(s, index)) {
        return gpio_word_bit(s->lev0, s->lev1, index);
    }
    if (pwm_signal >= 0) {
        return pwm_signal < 2 ? s->pwm_level[pwm_signal] :
                                s->pwm1_level[pwm_signal - 2];
    }
    if (gpio_word_bit(s->input_mask0, s->input_mask1, index)) {
        return gpio_word_bit(s->input0, s->input1, index);
    }
    /* Pull value 1 is pull-up; 0, 2, and reserved 3 resolve low. */
    return gpio_pull(s, index) == 1;
}

static uint32_t gpio_levels(BCM2838GpioState *s, unsigned int start,
                            unsigned int count)
{
    uint32_t levels = 0;
    unsigned int i;

    for (i = 0; i < count; i++) {
        levels = deposit32(levels, i, 1, gpio_level(s, start + i));
    }
    return levels;
}

static uint32_t gpio_bank_mask(unsigned int bank)
{
    return bank == 0 ? UINT32_MAX :
                       MAKE_64BIT_MASK(0, BCM2838_GPIO_NUM - 32);
}

static void gpio_update_irqs(BCM2838GpioState *s)
{
    qemu_set_irq(s->irq[0], s->event_status[0] & 0x0fffffff);
    qemu_set_irq(s->irq[1], (s->event_status[0] & 0xf0000000) ||
                            (s->event_status[1] & 0x00003fff));
    qemu_set_irq(s->irq[2], s->event_status[1] & 0x03ffc000);
    /* The fourth BCM2711 line is the wake IRQ, which is not yet modeled. */
    qemu_set_irq(s->irq[3], 0);
}

static void gpio_update_event(BCM2838GpioState *s, unsigned int index,
                              bool old_level)
{
    unsigned int bank = index / 32;
    unsigned int bit = index % 32;
    uint32_t mask = BIT(bit);
    bool new_level = gpio_level(s, index);
    bool rising = !old_level && new_level;
    bool falling = old_level && !new_level;

    if ((rising && ((s->rising_enable[bank] |
                     s->async_rising_enable[bank]) & mask)) ||
        (falling && ((s->falling_enable[bank] |
                      s->async_falling_enable[bank]) & mask)) ||
        (new_level && (s->high_enable[bank] & mask)) ||
        (!new_level && (s->low_enable[bank] & mask))) {
        s->event_status[bank] |= mask;
    }
    gpio_update_irqs(s);
    gpio_bridge_emit_pin(s, index);
}

static void gpio_update_level_events(BCM2838GpioState *s, unsigned int bank)
{
    uint32_t levels = gpio_levels(s, bank * 32,
                                  bank ? BCM2838_GPIO_NUM - 32 : 32);
    uint32_t valid = gpio_bank_mask(bank);

    s->event_status[bank] |= ((levels & s->high_enable[bank]) |
                              (~levels & s->low_enable[bank])) & valid;
    gpio_update_irqs(s);
}

static void gpio_update_output(BCM2838GpioState *s, unsigned int index)
{
    int pwm_signal = gpio_pwm_signal(s, index);
    bool enabled = gpio_is_output(s, index);
    bool value;

    if (pwm_signal < 0) {
        value = enabled && gpio_word_bit(s->lev0, s->lev1, index);
    } else {
        value = pwm_signal < 2 ? s->pwm_level[pwm_signal] :
                                 s->pwm1_level[pwm_signal - 2];
    }

    qemu_set_irq(s->out[index], value);
    qemu_set_irq(s->out_enable[index], enabled);
}

static void gpio_update_outputs(BCM2838GpioState *s)
{
    unsigned int i;

    for (i = 0; i < BCM2838_GPIO_NUM; i++) {
        gpio_update_output(s, i);
    }
}

static void bcm2838_gpio_set_pwm(void *opaque, int signal, int level)
{
    BCM2838GpioState *s = opaque;
    bool *levels;
    unsigned int channel;
    bool old_level;

    if (signal < 0 || signal >= 4) {
        return;
    }
    levels = signal < 2 ? s->pwm_level : s->pwm1_level;
    channel = signal % 2;
    old_level = levels[channel];
    levels[channel] = level;
    if (old_level == levels[channel]) {
        return;
    }
    for (unsigned int index = 0; index < BCM2838_GPIO_NUM; index++) {
        if (gpio_pwm_signal(s, index) == signal) {
            gpio_update_output(s, index);
            gpio_update_event(s, index, old_level);
        }
    }
}

static void bcm2838_gpio_set(void *opaque, int index, int level)
{
    BCM2838GpioState *s = opaque;
    uint32_t *input;
    uint32_t *mask;
    unsigned int bit;
    bool old_level;

    if (index < 0 || index >= BCM2838_GPIO_NUM) {
        return;
    }
    old_level = gpio_level(s, index);
    input = index < 32 ? &s->input0 : &s->input1;
    mask = index < 32 ? &s->input_mask0 : &s->input_mask1;
    bit = index < 32 ? index : index - 32;
    if (level < 0) {
        *mask = deposit32(*mask, bit, 1, 0);
    } else {
        *input = deposit32(*input, bit, 1, !!level);
        *mask = deposit32(*mask, bit, 1, 1);
    }
    gpio_update_event(s, index, old_level);
    if (index == 3 && s->powermgt) {
        bcm2835_powermgt_gpio3_input(s->powermgt, level);
    }
}

int bcm2838_gpio_get_external_input(BCM2838GpioState *s,
                                    unsigned int index)
{
    if (index >= BCM2838_GPIO_NUM ||
        !gpio_word_bit(s->input_mask0, s->input_mask1, index)) {
        return -1;
    }
    return gpio_word_bit(s->input0, s->input1, index);
}

int bcm2838_gpio_get_eeprom_nwp(BCM2838GpioState *s)
{
    return s->bridge_eeprom_nwp;
}

int bcm2838_gpio_get_sd_overcurrent(BCM2838GpioState *s)
{
    return s->bridge_sd_overcurrent;
}

static void bcm2838_gpio_apply_hat_map_now(BCM2838GpioState *s,
                                           const uint8_t map[30])
{
    s->hat_map_applied = true;
    s->hat_drive = map[0] & 0x0f;
    s->hat_slew = (map[0] >> 4) & 3;
    s->hat_hysteresis = (map[0] >> 6) & 3;
    s->hat_back_power = map[1] & 3;
    s->hat_used_mask = 0;
    for (unsigned int index = 0; index < 28; index++) {
        uint8_t setting = map[index + 2];
        unsigned int pull = (setting >> 5) & 3;
        bool old_level;

        if (!(setting & 0x80)) {
            continue;
        }
        s->hat_used_mask |= BIT(index);
        old_level = gpio_level(s, index);
        s->fsel[index] = setting & 7;
        if (pull) {
            pull = pull == 3 ? 0 : pull;
            s->pup_cntrl_reg[index / 16] =
                deposit32(s->pup_cntrl_reg[index / 16],
                          (index % 16) * 2, 2, pull);
        }
        gpio_update_output(s, index);
        gpio_update_event(s, index, old_level);
    }
}

void bcm2838_gpio_apply_hat_map(BCM2838GpioState *s,
                                const uint8_t map[30])
{
    if (!map) {
        s->hat_map_configured = false;
        s->hat_map_applied = false;
        memset(s->hat_map, 0, sizeof(s->hat_map));
        s->hat_drive = 0;
        s->hat_slew = 0;
        s->hat_hysteresis = 0;
        s->hat_back_power = 0;
        s->hat_used_mask = 0;
        return;
    }
    memcpy(s->hat_map, map, sizeof(s->hat_map));
    s->hat_map_configured = true;
    bcm2838_gpio_apply_hat_map_now(s, s->hat_map);
}

bool bcm2838_gpio_hat_map_applied(BCM2838GpioState *s)
{
    return s->hat_map_applied;
}

uint32_t bcm2838_gpio_hat_used_mask(BCM2838GpioState *s)
{
    return s->hat_used_mask;
}

uint8_t bcm2838_gpio_hat_drive(BCM2838GpioState *s)
{
    return s->hat_drive;
}

uint8_t bcm2838_gpio_hat_slew(BCM2838GpioState *s)
{
    return s->hat_slew;
}

uint8_t bcm2838_gpio_hat_hysteresis(BCM2838GpioState *s)
{
    return s->hat_hysteresis;
}

uint8_t bcm2838_gpio_hat_back_power(BCM2838GpioState *s)
{
    return s->hat_back_power;
}

static void gpset(BCM2838GpioState *s, uint32_t val, uint8_t start,
                  uint8_t count, uint32_t *lev)
{
    uint32_t valid = count == 32 ? UINT32_MAX : MAKE_64BIT_MASK(0, count);
    uint32_t changes;
    uint32_t cur = 1;
    int i;

    val &= valid;
    changes = val & ~*lev;

    *lev |= val;
    for (i = 0; i < count; i++) {
        if ((changes & cur) && gpfsel_is_out(s, start + i)) {
            qemu_set_irq(s->out[start + i], 1);
            gpio_update_event(s, start + i, false);
        }
        cur <<= 1;
    }
}

static void gpclr(BCM2838GpioState *s, uint32_t val, uint8_t start,
                  uint8_t count, uint32_t *lev)
{
    uint32_t valid = count == 32 ? UINT32_MAX : MAKE_64BIT_MASK(0, count);
    uint32_t changes;
    uint32_t cur = 1;
    int i;

    val &= valid;
    changes = val & *lev;

    *lev &= ~val;
    for (i = 0; i < count; i++) {
        if ((changes & cur) && gpfsel_is_out(s, start + i)) {
            qemu_set_irq(s->out[start + i], 0);
            gpio_update_event(s, start + i, true);
        }
        cur <<= 1;
    }
}

static uint64_t bcm2838_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2838GpioState *s = (BCM2838GpioState *)opaque;
    uint64_t value = 0;

    switch (offset) {
    case GPFSEL0:
    case GPFSEL1:
    case GPFSEL2:
    case GPFSEL3:
    case GPFSEL4:
    case GPFSEL5:
        value = gpfsel_get(s, offset / BYTES_IN_WORD);
        break;
    case GPSET0:
    case GPSET1:
    case GPCLR0:
    case GPCLR1:
        /* Write Only */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Attempt reading from write only"
                      " register. 0x%"PRIx64" will be returned."
                      " Address 0x%"HWADDR_PRIx", size %u\n",
                      TYPE_BCM2838_GPIO, __func__, value, offset, size);
        break;
    case GPLEV0:
        value = gpio_levels(s, 0, 32);
        break;
    case GPLEV1:
        value = gpio_levels(s, 32, BCM2838_GPIO_NUM - 32);
        break;
    case GPEDS0:
        value = s->event_status[0];
        break;
    case GPEDS1:
        value = s->event_status[1];
        break;
    case GPREN0:
        value = s->rising_enable[0];
        break;
    case GPREN1:
        value = s->rising_enable[1];
        break;
    case GPFEN0:
        value = s->falling_enable[0];
        break;
    case GPFEN1:
        value = s->falling_enable[1];
        break;
    case GPHEN0:
        value = s->high_enable[0];
        break;
    case GPHEN1:
        value = s->high_enable[1];
        break;
    case GPLEN0:
        value = s->low_enable[0];
        break;
    case GPLEN1:
        value = s->low_enable[1];
        break;
    case GPAREN0:
        value = s->async_rising_enable[0];
        break;
    case GPAREN1:
        value = s->async_rising_enable[1];
        break;
    case GPAFEN0:
        value = s->async_falling_enable[0];
        break;
    case GPAFEN1:
        value = s->async_falling_enable[1];
        break;
    case GPIO_PUP_PDN_CNTRL_REG0:
    case GPIO_PUP_PDN_CNTRL_REG1:
    case GPIO_PUP_PDN_CNTRL_REG2:
    case GPIO_PUP_PDN_CNTRL_REG3:
        value = s->pup_cntrl_reg[(offset - GPIO_PUP_PDN_CNTRL_REG0)
                                 / sizeof(s->pup_cntrl_reg[0])];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: bad offset %"HWADDR_PRIx"\n",
                      TYPE_BCM2838_GPIO, __func__, offset);
        break;
    }

    return value;
}

static void bcm2838_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    BCM2838GpioState *s = (BCM2838GpioState *)opaque;

    switch (offset) {
    case GPFSEL0:
    case GPFSEL1:
    case GPFSEL2:
    case GPFSEL3:
    case GPFSEL4:
    case GPFSEL5:
        gpfsel_set(s, offset / BYTES_IN_WORD, value);
        break;
    case GPSET0:
        gpset(s, value, 0, 32, &s->lev0);
        break;
    case GPSET1:
        gpset(s, value, 32, BCM2838_GPIO_NUM - 32, &s->lev1);
        break;
    case GPCLR0:
        gpclr(s, value, 0, 32, &s->lev0);
        break;
    case GPCLR1:
        gpclr(s, value, 32, BCM2838_GPIO_NUM - 32, &s->lev1);
        break;
    case GPLEV0:
    case GPLEV1:
        /* Read Only */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Attempt writing 0x%"PRIx64""
                      " to read only register. Ignored."
                      " Address 0x%"HWADDR_PRIx", size %u\n",
                      TYPE_BCM2838_GPIO, __func__, value, offset, size);
        break;
    case GPEDS0:
        s->event_status[0] &= ~value;
        gpio_update_level_events(s, 0);
        break;
    case GPEDS1:
        s->event_status[1] &= ~(value & gpio_bank_mask(1));
        gpio_update_level_events(s, 1);
        break;
    case GPREN0:
        s->rising_enable[0] = value;
        break;
    case GPREN1:
        s->rising_enable[1] = value & gpio_bank_mask(1);
        break;
    case GPFEN0:
        s->falling_enable[0] = value;
        break;
    case GPFEN1:
        s->falling_enable[1] = value & gpio_bank_mask(1);
        break;
    case GPHEN0:
        s->high_enable[0] = value;
        gpio_update_level_events(s, 0);
        break;
    case GPHEN1:
        s->high_enable[1] = value & gpio_bank_mask(1);
        gpio_update_level_events(s, 1);
        break;
    case GPLEN0:
        s->low_enable[0] = value;
        gpio_update_level_events(s, 0);
        break;
    case GPLEN1:
        s->low_enable[1] = value & gpio_bank_mask(1);
        gpio_update_level_events(s, 1);
        break;
    case GPAREN0:
        s->async_rising_enable[0] = value;
        break;
    case GPAREN1:
        s->async_rising_enable[1] = value & gpio_bank_mask(1);
        break;
    case GPAFEN0:
        s->async_falling_enable[0] = value;
        break;
    case GPAFEN1:
        s->async_falling_enable[1] = value & gpio_bank_mask(1);
        break;
    case GPIO_PUP_PDN_CNTRL_REG0:
    case GPIO_PUP_PDN_CNTRL_REG1:
    case GPIO_PUP_PDN_CNTRL_REG2:
    case GPIO_PUP_PDN_CNTRL_REG3:
    {
        unsigned int reg = (offset - GPIO_PUP_PDN_CNTRL_REG0) /
                           sizeof(s->pup_cntrl_reg[0]);
        bool old_level[16];
        unsigned int i;

        for (i = 0; i < 16 && reg * 16 + i < BCM2838_GPIO_NUM; i++) {
            old_level[i] = gpio_level(s, reg * 16 + i);
        }
        s->pup_cntrl_reg[reg] = value;
        for (i = 0; i < 16 && reg * 16 + i < BCM2838_GPIO_NUM; i++) {
            gpio_update_event(s, reg * 16 + i, old_level[i]);
        }
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: bad offset %"HWADDR_PRIx"\n",
                  TYPE_BCM2838_GPIO, __func__, offset);
    }
}

static void bcm2838_gpio_reset(DeviceState *dev)
{
    BCM2838GpioState *s = BCM2838_GPIO(dev);

    memset(s->fsel, 0, sizeof(s->fsel));

    s->sd_fsel = 0;

    /* SDHCI is selected by default */
    sdbus_reparent_card(&s->sdbus, s->sdbus_sdhci);

    s->lev0 = 0;
    s->lev1 = 0;
    /* External drivers remain connected while the controller resets. */
    memset(s->event_status, 0, sizeof(s->event_status));
    memset(s->rising_enable, 0, sizeof(s->rising_enable));
    memset(s->falling_enable, 0, sizeof(s->falling_enable));
    memset(s->high_enable, 0, sizeof(s->high_enable));
    memset(s->low_enable, 0, sizeof(s->low_enable));
    memset(s->async_rising_enable, 0, sizeof(s->async_rising_enable));
    memset(s->async_falling_enable, 0, sizeof(s->async_falling_enable));

    memset(s->fsel, 0, sizeof(s->fsel));

    s->pup_cntrl_reg[0] = RESET_VAL_CNTRL_REG0;
    s->pup_cntrl_reg[1] = RESET_VAL_CNTRL_REG1;
    s->pup_cntrl_reg[2] = RESET_VAL_CNTRL_REG2;
    s->pup_cntrl_reg[3] = RESET_VAL_CNTRL_REG3;
    s->hat_map_applied = false;
    s->hat_drive = 0;
    s->hat_slew = 0;
    s->hat_hysteresis = 0;
    s->hat_back_power = 0;
    s->hat_used_mask = 0;
    if (s->hat_map_configured) {
        bcm2838_gpio_apply_hat_map_now(s, s->hat_map);
    }
    gpio_update_outputs(s);
    gpio_update_irqs(s);
    gpio_bridge_write(s, "RESET\n");
}

static const MemoryRegionOps bcm2838_gpio_ops = {
    .read = bcm2838_gpio_read,
    .write = bcm2838_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int bcm2838_gpio_post_load(void *opaque, int version_id)
{
    BCM2838GpioState *s = opaque;

    gpio_update_outputs(s);
    gpio_update_irqs(s);
    /*
     * A repeated banner is an idempotent full-state synchronization request:
     * gpio_proxy responds with GET ALL, then reapplies physical inputs.
     */
    gpio_bridge_hello(s);
    return 0;
}

static const VMStateDescription vmstate_bcm2838_gpio = {
    .name = "bcm2838_gpio",
    .version_id = 8,
    .minimum_version_id = 1,
    .post_load = bcm2838_gpio_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fsel, BCM2838GpioState, BCM2838_GPIO_NUM),
        VMSTATE_UINT32(lev0, BCM2838GpioState),
        VMSTATE_UINT32(lev1, BCM2838GpioState),
        VMSTATE_UINT32_V(input0, BCM2838GpioState, 2),
        VMSTATE_UINT32_V(input1, BCM2838GpioState, 2),
        VMSTATE_UINT32_V(input_mask0, BCM2838GpioState, 2),
        VMSTATE_UINT32_V(input_mask1, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY_V(event_status, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT32_ARRAY_V(rising_enable, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT32_ARRAY_V(falling_enable, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT32_ARRAY_V(high_enable, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT32_ARRAY_V(low_enable, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT32_ARRAY_V(async_rising_enable, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT32_ARRAY_V(async_falling_enable, BCM2838GpioState,
                               BCM2838_GPIO_BANKS, 3),
        VMSTATE_UINT8(sd_fsel, BCM2838GpioState),
        VMSTATE_INT8_V(bridge_eeprom_nwp, BCM2838GpioState, 4),
        VMSTATE_INT8_V(bridge_sd_overcurrent, BCM2838GpioState, 6),
        VMSTATE_BOOL_V(hat_map_applied, BCM2838GpioState, 5),
        VMSTATE_BOOL_V(hat_map_configured, BCM2838GpioState, 5),
        VMSTATE_UINT8_ARRAY_V(hat_map, BCM2838GpioState, 30, 5),
        VMSTATE_UINT8_V(hat_drive, BCM2838GpioState, 5),
        VMSTATE_UINT8_V(hat_slew, BCM2838GpioState, 5),
        VMSTATE_UINT8_V(hat_hysteresis, BCM2838GpioState, 5),
        VMSTATE_UINT8_V(hat_back_power, BCM2838GpioState, 5),
        VMSTATE_UINT32_V(hat_used_mask, BCM2838GpioState, 5),
        VMSTATE_UINT32_ARRAY(pup_cntrl_reg, BCM2838GpioState,
                             GPIO_PUP_PDN_CNTRL_NUM),
        VMSTATE_BOOL_ARRAY_V(pwm_level, BCM2838GpioState, 2, 7),
        VMSTATE_BOOL_ARRAY_V(pwm1_level, BCM2838GpioState, 2, 8),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_gpio_init(Object *obj)
{
    BCM2838GpioState *s = BCM2838_GPIO(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->bridge_eeprom_nwp = -1;
    s->bridge_sd_overcurrent = -1;
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(s), "sd-bus");

    memory_region_init_io(&s->iomem, obj, &bcm2838_gpio_ops, s,
                          "bcm2838_gpio", BCM2838_GPIO_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out(dev, s->out, BCM2838_GPIO_NUM);
    qdev_init_gpio_out_named(dev, s->out_enable, "pin-output-enable",
                             BCM2838_GPIO_NUM);
    qdev_init_gpio_in_named(dev, bcm2838_gpio_set, "pin-input",
                            BCM2838_GPIO_NUM);
    qdev_init_gpio_in_named(dev, bcm2838_gpio_set_pwm, "pwm-input", 4);
    for (unsigned int i = 0; i < BCM2838_GPIO_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void bcm2838_gpio_realize(DeviceState *dev, Error **errp)
{
    BCM2838GpioState *s = BCM2838_GPIO(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "sdbus-sdhci", &error_abort);
    s->sdbus_sdhci = SD_BUS(obj);

    obj = object_property_get_link(OBJECT(dev), "sdbus-sdhost", &error_abort);
    s->sdbus_sdhost = SD_BUS(obj);
    obj = object_property_get_link(OBJECT(dev), "powermgt", &error_abort);
    s->powermgt = BCM2835_POWERMGT(obj);

    if (qemu_chr_fe_backend_connected(&s->bridge_chr)) {
        qemu_chr_fe_set_handlers(&s->bridge_chr, gpio_bridge_can_receive,
                                 gpio_bridge_receive, gpio_bridge_event, NULL,
                                 s, NULL, true);
    }
}

static const Property bcm2838_gpio_properties[] = {
    DEFINE_PROP_CHR("chardev", BCM2838GpioState, bridge_chr),
};

static void bcm2838_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_bcm2838_gpio;
    dc->realize = &bcm2838_gpio_realize;
    device_class_set_props(dc, bcm2838_gpio_properties);
    device_class_set_legacy_reset(dc, bcm2838_gpio_reset);
}

static const TypeInfo bcm2838_gpio_info = {
    .name          = TYPE_BCM2838_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838GpioState),
    .instance_init = bcm2838_gpio_init,
    .class_init    = bcm2838_gpio_class_init,
};

static void bcm2838_gpio_register_types(void)
{
    type_register_static(&bcm2838_gpio_info);
}

type_init(bcm2838_gpio_register_types)
