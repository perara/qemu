/*
 * SD Memory Card emulation as defined in the "SD Memory Card Physical
 * layer specification, Version 2.00."
 *
 * eMMC emulation defined in "JEDEC Standard No. 84-A43"
 *
 * Copyright (c) 2006 Andrzej Zaborowski  <balrog@zabor.org>
 * Copyright (c) 2007 CodeSourcery
 * Copyright (c) 2018 Philippe Mathieu-Daudé
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "system/block-backend.h"
#include "hw/sd/sd.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitmap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "qemu/module.h"
#include "sdmmc-internal.h"
#include "trace.h"
#include "crypto/hmac.h"
#include "net/net.h"

//#define DEBUG_SD 1

#define SDSC_MAX_CAPACITY   (2 * GiB)

#define INVALID_ADDRESS     UINT32_MAX

typedef enum {
    sd_r0 = 0,    /* no response */
    sd_r1,        /* normal response command */
    spi_r2,       /* STATUS */
    sd_r2_i,      /* CID register */
    sd_r2_s,      /* CSD register */
    sd_r3,        /* OCR register */
    sd_r6 = 6,    /* Published RCA response */
    sd_r7,        /* Operating voltage */
    sd_r1b = -1,
    sd_illegal = -2,
} sd_rsp_type_t;

typedef enum {
    sd_spi,
    sd_bc,     /* broadcast -- no response */
    sd_bcr,    /* broadcast with response */
    sd_ac,     /* addressed -- no data transfer */
    sd_adtc,   /* addressed with data transfer */
} sd_cmd_type_t;

enum SDCardModes {
    sd_inactive,
    sd_card_identification_mode,
    sd_data_transfer_mode,
};

enum SDCardStates {
    sd_waitirq_state        = -2, /* emmc */
    sd_inactive_state       = -1,

    sd_idle_state           = 0,
    sd_ready_state          = 1,
    sd_identification_state = 2,
    sd_standby_state        = 3,
    sd_transfer_state       = 4,
    sd_sendingdata_state    = 5,
    sd_receivingdata_state  = 6,
    sd_programming_state    = 7,
    sd_disconnect_state     = 8,
    sd_bus_test_state       = 9,  /* emmc */
    sd_sleep_state          = 10, /* emmc */
    sd_io_state             = 15  /* sd */
};

#define SDMMC_CMD_MAX 64

typedef sd_rsp_type_t (*sd_cmd_handler)(SDState *sd, SDRequest req);

typedef struct SDProto {
    const char *name;
    struct {
        const unsigned class;
        const sd_cmd_type_t type;
        const char *name;
        sd_cmd_handler handler;
    } cmd[SDMMC_CMD_MAX], acmd[SDMMC_CMD_MAX];
} SDProto;

#define RPMB_STUFF_LEN      196
#define RPMB_KEY_MAC_LEN    32
#define RPMB_DATA_LEN       256     /* one RPMB block is half a sector */
#define RPMB_NONCE_LEN      16
#define RPMB_HASH_LEN       284

typedef struct QEMU_PACKED {
    uint8_t stuff_bytes[RPMB_STUFF_LEN];
    uint8_t key_mac[RPMB_KEY_MAC_LEN];
    uint8_t data[RPMB_DATA_LEN];
    uint8_t nonce[RPMB_NONCE_LEN];
    uint32_t write_counter;
    uint16_t address;
    uint16_t block_count;
    uint16_t result;
    uint16_t req_resp;
} RPMBDataFrame;

QEMU_BUILD_BUG_MSG(sizeof(RPMBDataFrame) != 512,
                   "invalid RPMBDataFrame size");

typedef struct EMMCCacheEntry {
    uint64_t addr;
    uint64_t key;
    uint8_t partition;
    uint8_t data[512];
} EMMCCacheEntry;

typedef struct EMMCProgramEntry {
    uint64_t addr;
    uint8_t partition;
    uint8_t data[512];
} EMMCProgramEntry;

struct SDState {
    DeviceState parent_obj;

    /* SD Memory Card Registers */
    uint32_t ocr;
    uint8_t scr[8];
    uint8_t cid[16];
    uint8_t csd[16];
    uint16_t rca;
    uint32_t card_status;
    uint8_t sd_status[64];
    union {
        uint8_t ext_csd[512];
        struct {
            uint8_t ext_csd_rw[192]; /* Modes segment */
            uint8_t ext_csd_ro[320]; /* Properties segment */
        };
    };

    /* Static properties */

    uint8_t spec_version;
    uint64_t boot_part_size;
    uint64_t rpmb_part_size;
    uint64_t cache_size;
    uint32_t cache_capacity;
    uint32_t cache_count;
    bool cache_power_loss_on_reset;
    uint64_t cache_flush_sector_delay_us;
    uint64_t cache_flush_deadline_us;
    uint64_t cache_flush_completed_sectors;
    bool cache_flush_active;
    EMMCCacheEntry *cache_entries;
    GHashTable *cache_index;
    QEMUTimer *cache_flush_timer;
    uint64_t program_sector_delay_us;
    uint64_t program_deadline_us;
    uint64_t program_completed_sectors;
    int32_t program_count;
    bool program_active;
    EMMCProgramEntry *program_entries;
    QEMUTimer *program_timer;
    uint64_t erase_group_delay_us;
    uint64_t erase_next;
    uint64_t erase_last;
    uint64_t erase_deadline_us;
    uint64_t erase_completed_groups;
    uint8_t erase_partition;
    bool erase_active;
    QEMUTimer *erase_timer;
    BlockBackend *blk;
    BlockBackend *boot_blk;
    BlockBackend *rpmb_blk;
    uint8_t boot_config;

    const SDProto *proto;

    /* Runtime changeables */

    int32_t state;    /* current card state, one of SDCardStates */
    uint32_t vhs;
    bool wp_switch;
    unsigned long *wp_group_bmap;
    int32_t wp_group_bits;
    uint64_t size;
    uint32_t blk_len;
    uint32_t multi_blk_cnt;
    bool reliable_write;
    uint32_t erase_start;
    uint32_t erase_end;
    uint8_t pwd[16];
    uint32_t pwd_len;
    uint8_t function_group[6];
    uint8_t current_cmd;
    const char *last_cmd_name;
    /* True if we will handle the next command as an ACMD. Note that this does
     * *not* track the APP_CMD status bit!
     */
    bool expecting_acmd;
    uint32_t blk_written;

    uint64_t data_start;
    uint32_t data_offset;
    size_t data_size;
    uint8_t data[512];
    struct {
        uint32_t write_counter;
        uint8_t key[RPMB_KEY_MAC_LEN];
        uint8_t key_set;
        /*
         * RPMBDataFrame is packed to match the wire format, so it would
         * otherwise be placed straight after key_set, at an odd offset.
         * vmstate takes the address of write_counter and address inside
         * it, and the migration helpers dereference those as aligned
         * types; on a host that requires alignment that is a fault
         * rather than a slow access.  Only the placement of the frame
         * changes, not its layout or size, so neither the RPMB wire
         * format nor the migration stream is affected.
         */
        QEMU_ALIGNED(4) RPMBDataFrame result;
    } rpmb;
    QEMUTimer *ocr_power_timer;
    uint8_t dat_lines;
    bool cmd_line;
    char *preset_auth_key;
    char *preset_cid;
    uint8_t configured_cid[16];

    /* CYW43455 SDIO transport state. */
    uint8_t cyw_cccr[0x100];
    uint8_t cyw_fbr[0x300];
    uint8_t cyw_func1_regs[0x20];
    uint8_t cyw_transfer[32768];
    uint8_t cyw_rx_queue[32768];
    uint32_t cyw_transfer_size;
    uint32_t cyw_transfer_offset;
    uint32_t cyw_transfer_address;
    uint32_t cyw_ram_size;
    uint8_t *cyw_ram;
    uint16_t cyw_rca;
    uint16_t cyw_block_size[3];
    uint8_t cyw_function;
    uint8_t cyw_fail_command;
    bool cyw_selected;
    bool cyw_transfer_write;
    bool cyw_transfer_increment;
    uint64_t cyw_command_count;
    uint64_t cyw_fail_after;
    uint64_t cyw_command_failures;
    uint64_t cyw_firmware_bytes;
    uint64_t cyw_tx_bytes;
    uint64_t cyw_rx_bytes;
    uint32_t cyw_core_ioctl[4];
    uint32_t cyw_core_reset[4];
    uint32_t cyw_armcr4_bankidx;
    uint32_t cyw_reset_vector;
    uint32_t cyw_nvram_size;
    uint32_t cyw_shared_address;
    uint32_t cyw_intstatus;
    uint32_t cyw_hostintmask;
    uint32_t cyw_tosbmailbox;
    uint32_t cyw_tohostmailbox;
    uint32_t cyw_tosbmailboxdata;
    uint32_t cyw_tohostmailboxdata;
    uint32_t cyw_rx_queue_size;
    uint32_t cyw_rx_queue_offset;
    uint64_t cyw_start_failures;
    uint64_t cyw_control_requests;
    uint64_t cyw_control_rejections;
    uint64_t cyw_data_tx_packets;
    uint64_t cyw_data_rx_packets;
    uint8_t cyw_packet_drop_direction;
    uint64_t cyw_packet_drop_after;
    uint32_t cyw_packet_drop_count;
    uint64_t cyw_packet_drop_packets_seen;
    uint32_t cyw_packet_drops_injected;
    uint8_t cyw_rx_sequence;
    uint8_t cyw_tx_sequence_max;
    bool cyw_reset_vector_valid;
    bool cyw_firmware_started;
    bool cyw_sdio_irq;
    bool cyw_link_event_pending;
    bool cyw_link_event_sent;
    NICConf cyw_nic_conf;
    NICState *cyw_nic;
};

enum {
    CYW_PACKET_DROP_NONE,
    CYW_PACKET_DROP_TX,
    CYW_PACKET_DROP_RX,
    CYW_PACKET_DROP_BOTH,
};

static void sd_realize(DeviceState *dev, Error **errp);

static const SDProto sd_proto_spi;
static const SDProto sd_proto_emmc;

static bool sd_emmc_cache_flush(SDState *sd);
static void sd_emmc_cache_drop(SDState *sd);
static void sd_emmc_cache_flush_timer(void *opaque);
static void sd_emmc_program_drop(SDState *sd);
static void sd_emmc_program_timer(void *opaque);
static void sd_emmc_erase_timer(void *opaque);
static bool cyw_sdio_inject_packet_drop(SDState *sd, uint8_t direction);

static bool sd_is_spi(SDState *sd)
{
    return sd->proto == &sd_proto_spi;
}

static bool sd_is_emmc(SDState *sd)
{
    return sd->proto == &sd_proto_emmc;
}

static const char *sd_version_str(enum SDPhySpecificationVersion version)
{
    static const char *sdphy_version[] = {
        [SD_PHY_SPECv2_00_VERS]     = "v2.00",
        [SD_PHY_SPECv3_01_VERS]     = "v3.01",
    };
    if (version >= ARRAY_SIZE(sdphy_version)) {
        return "unsupported version";
    }
    return sdphy_version[version];
}

static const char *sd_mode_name(enum SDCardModes mode)
{
    static const char *mode_name[] = {
        [sd_inactive]                   = "inactive",
        [sd_card_identification_mode]   = "identification",
        [sd_data_transfer_mode]         = "transfer",
    };
    assert(mode < ARRAY_SIZE(mode_name));
    return mode_name[mode];
}

static const char *sd_state_name(enum SDCardStates state)
{
    static const char *state_name[] = {
        [sd_idle_state]             = "idle",
        [sd_ready_state]            = "ready",
        [sd_identification_state]   = "identification",
        [sd_standby_state]          = "standby",
        [sd_transfer_state]         = "transfer",
        [sd_sendingdata_state]      = "sendingdata",
        [sd_bus_test_state]         = "bus-test",
        [sd_receivingdata_state]    = "receivingdata",
        [sd_programming_state]      = "programming",
        [sd_disconnect_state]       = "disconnect",
        [sd_sleep_state]            = "sleep",
        [sd_io_state]               = "i/o"
    };
    if (state == sd_inactive_state) {
        return "inactive";
    }
    if (state == sd_waitirq_state) {
        return "wait-irq";
    }
    assert(state < ARRAY_SIZE(state_name));
    return state_name[state];
}

static const char *sd_response_name(sd_rsp_type_t rsp)
{
    static const char *response_name[] = {
        [sd_r0]     = "RESP#0 (no response)",
        [sd_r1]     = "RESP#1 (normal cmd)",
        [spi_r2]    = "RESP#2 (STATUS reg)",
        [sd_r2_i]   = "RESP#2 (CID reg)",
        [sd_r2_s]   = "RESP#2 (CSD reg)",
        [sd_r3]     = "RESP#3 (OCR reg)",
        [sd_r6]     = "RESP#6 (RCA)",
        [sd_r7]     = "RESP#7 (operating voltage)",
    };
    if (rsp == sd_illegal) {
        return "ILLEGAL RESP";
    }
    if (rsp == sd_r1b) {
        rsp = sd_r1;
    }
    assert(rsp < ARRAY_SIZE(response_name));
    return response_name[rsp];
}

static const char *sd_cmd_name(SDState *sd, uint8_t cmd)
{
    static const char *cmd_abbrev[SDMMC_CMD_MAX] = {
        [18]    = "READ_MULTIPLE_BLOCK",
                                            [25]    = "WRITE_MULTIPLE_BLOCK",
    };
    const SDProto *sdp = sd->proto;

    if (sdp->cmd[cmd].handler) {
        assert(!cmd_abbrev[cmd]);
        return sdp->cmd[cmd].name;
    }
    return cmd_abbrev[cmd] ? cmd_abbrev[cmd] : "UNKNOWN_CMD";
}

static const char *sd_acmd_name(SDState *sd, uint8_t cmd)
{
    const SDProto *sdp = sd->proto;

    if (sdp->acmd[cmd].handler) {
        return sdp->acmd[cmd].name;
    }

    return "UNKNOWN_ACMD";
}

static uint8_t sd_get_dat_lines(SDState *sd)
{
    return sd->dat_lines;
}

static bool sd_get_cmd_line(SDState *sd)
{
    return sd->cmd_line;
}

static void sd_set_voltage(SDState *sd, uint16_t millivolts)
{
    trace_sdcard_set_voltage(millivolts);

    switch (millivolts) {
    case 3001 ... 3600: /* SD_VOLTAGE_3_3V */
    case 2001 ... 3000: /* SD_VOLTAGE_3_0V */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "SD card voltage not supported: %.3fV",
                      millivolts / 1000.f);
    }
}

static enum SDCardModes sd_mode(SDState *sd)
{
    switch (sd->state) {
    case sd_inactive_state:
        return sd_inactive;
    case sd_idle_state:
    case sd_ready_state:
    case sd_identification_state:
        return sd_card_identification_mode;
    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        return sd_data_transfer_mode;
    default:
        g_assert_not_reached();
    }
}

static uint8_t sd_crc7(const void *message, size_t width)
{
    int i, bit;
    uint8_t shift_reg = 0x00;
    const uint8_t *msg = (const uint8_t *)message;

    for (i = 0; i < width; i ++, msg ++)
        for (bit = 7; bit >= 0; bit --) {
            shift_reg <<= 1;
            if ((shift_reg >> 7) ^ ((*msg >> bit) & 1))
                shift_reg ^= 0x89;
        }

    return shift_reg;
}

/* Operation Conditions register */

#define OCR_POWER_DELAY_NS      500000 /* 0.5ms */

FIELD(OCR, VDD_VOLTAGE_WINDOW,          0, 24)
FIELD(OCR, VDD_VOLTAGE_WIN_LO,          0,  8)
FIELD(OCR, DUAL_VOLTAGE_CARD,           7,  1)
FIELD(OCR, VDD_VOLTAGE_WIN_HI,          8, 16)
FIELD(OCR, ACCEPT_SWITCH_1V8,          24,  1) /* Only UHS-I */
FIELD(OCR, UHS_II_CARD,                29,  1) /* Only UHS-II */
FIELD(OCR, CARD_CAPACITY,              30,  1) /* 0:SDSC, 1:SDHC/SDXC */
FIELD(OCR, CARD_POWER_UP,              31,  1)

#define ACMD41_ENQUIRY_MASK     0x00ffffff
#define ACMD41_R3_MASK          (R_OCR_VDD_VOLTAGE_WIN_HI_MASK \
                               | R_OCR_ACCEPT_SWITCH_1V8_MASK \
                               | R_OCR_UHS_II_CARD_MASK \
                               | R_OCR_CARD_CAPACITY_MASK \
                               | R_OCR_CARD_POWER_UP_MASK)

static void sd_ocr_powerup(void *opaque)
{
    SDState *sd = opaque;

    trace_sdcard_powerup();
    assert(!FIELD_EX32(sd->ocr, OCR, CARD_POWER_UP));

    /* card power-up OK */
    sd->ocr = FIELD_DP32(sd->ocr, OCR, CARD_POWER_UP, 1);

    if (sd->size > SDSC_MAX_CAPACITY) {
        sd->ocr = FIELD_DP32(sd->ocr, OCR, CARD_CAPACITY, 1);
    }
}

static void sd_set_ocr(SDState *sd)
{
    /* All voltages OK */
    sd->ocr = R_OCR_VDD_VOLTAGE_WIN_HI_MASK;

    if (sd_is_spi(sd)) {
        /*
         * We don't need to emulate power up sequence in SPI-mode.
         * Thus, the card's power up status bit should be set to 1 when reset.
         * The card's capacity status bit should also be set if SD card size
         * is larger than 2GB for SDHC support.
         */
        sd_ocr_powerup(sd);
    }
}

/* SD Configuration register */

static void sd_set_scr(SDState *sd)
{
    sd->scr[0] = 0 << 4;        /* SCR structure version 1.0 */
    sd->scr[0] |= 2;            /* Spec Version 2.00 or Version 3.0X */
    sd->scr[1] = (2 << 4)       /* SDSC Card (Security Version 1.01) */
                 | 0b0101;      /* 1-bit or 4-bit width bus modes */
    sd->scr[2] = 0x00;          /* Extended Security is not supported. */
    if (sd->spec_version >= SD_PHY_SPECv3_01_VERS) {
        sd->scr[2] |= 1 << 7;   /* Spec Version 3.0X */
    }
    sd->scr[3] = 0x00;
    /* reserved for manufacturer usage */
    sd->scr[4] = 0x00;
    sd->scr[5] = 0x00;
    sd->scr[6] = 0x00;
    sd->scr[7] = 0x00;
}

/* Card IDentification register */

#define MID     0xaa
#define OID     "XY"
#define PNM     "QEMU!"
#define PRV     0x01
#define MDT_YR  2006
#define MDT_MON 2

static void sd_set_cid(SDState *sd)
{
    sd->cid[0] = MID;       /* Fake card manufacturer ID (MID) */
    sd->cid[1] = OID[0];    /* OEM/Application ID (OID) */
    sd->cid[2] = OID[1];
    sd->cid[3] = PNM[0];    /* Fake product name (PNM) */
    sd->cid[4] = PNM[1];
    sd->cid[5] = PNM[2];
    sd->cid[6] = PNM[3];
    sd->cid[7] = PNM[4];
    sd->cid[8] = PRV;       /* Fake product revision (PRV) */
    stl_be_p(&sd->cid[9], 0xdeadbeef); /* Fake serial number (PSN) */
    sd->cid[13] = 0x00 |    /* Manufacture date (MDT) */
        ((MDT_YR - 2000) / 10);
    sd->cid[14] = ((MDT_YR % 10) << 4) | MDT_MON;
    sd->cid[15] = (sd_crc7(sd->cid, 15) << 1) | 1;
}

static void emmc_set_cid(SDState *sd)
{
    if (sd->preset_cid) {
        memcpy(sd->cid, sd->configured_cid, sizeof(sd->cid));
        return;
    }

    sd->cid[0] = MID;       /* Fake card manufacturer ID (MID) */
    sd->cid[1] = 0b01;      /* CBX: soldered BGA */
    sd->cid[2] = OID[0];    /* OEM/Application ID (OID) */
    sd->cid[3] = PNM[0];    /* Fake product name (PNM) */
    sd->cid[4] = PNM[1];
    sd->cid[5] = PNM[2];
    sd->cid[6] = PNM[3];
    sd->cid[7] = PNM[4];
    sd->cid[8] = PNM[4];
    sd->cid[9] = PRV;       /* Fake product revision (PRV) */
    stl_be_p(&sd->cid[10], 0xdeadbeef); /* Fake serial number (PSN) */
    sd->cid[14] = (MDT_MON << 4) | (MDT_YR - 1997); /* Manufacture date (MDT) */
    sd->cid[15] = (sd_crc7(sd->cid, 15) << 1) | 1;
}

/* Card-Specific Data register */

#define HWBLOCK_SHIFT   9        /* 512 bytes */
#define SECTOR_SHIFT    5        /* 16 kilobytes */
#define WPGROUP_SHIFT   7        /* 2 megs */
#define CMULT_SHIFT     9        /* 512 times HWBLOCK_SIZE */
#define WPGROUP_SIZE    (1 << (HWBLOCK_SHIFT + SECTOR_SHIFT + WPGROUP_SHIFT))
#define EMMC_HC_ERASE_GROUP_BYTES (512 * KiB)

static const uint8_t sd_csd_rw_mask[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xfe,
};

static void emmc_set_ext_csd(SDState *sd, uint64_t size)
{
    uint32_t sectcount = size >> HWBLOCK_SHIFT;

    memset(sd->ext_csd, 0, sizeof(sd->ext_csd)); /* FIXME only RW at reset */

    /* Properties segment (RO) */
    sd->ext_csd[EXT_CSD_S_CMD_SET] = 0b1; /* supported command sets */
    sd->ext_csd[EXT_CSD_BOOT_INFO] = 0x0; /* Boot information */
                                     /* Boot partition size. 128KB unit */
    sd->ext_csd[EXT_CSD_BOOT_MULT] = sd->boot_part_size / (128 * KiB);
    sd->ext_csd[EXT_CSD_ACC_SIZE] = 0x1; /* Access size */
    sd->ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] = 0x01; /* HC Erase unit size */
    sd->ext_csd[EXT_CSD_ERASE_TIMEOUT_MULT] = 0x01; /* HC erase timeout */
    sd->ext_csd[EXT_CSD_ERASED_MEM_CONT] = 0x01; /* erased bytes read as 0xff */
    sd->ext_csd[EXT_CSD_REL_WR_SEC_C] = 0x1; /* Reliable write sector count */
    sd->ext_csd[EXT_CSD_WR_REL_PARAM] = EXT_CSD_WR_REL_PARAM_EN;
    sd->ext_csd[EXT_CSD_HC_WP_GRP_SIZE] = 0x01; /* HC write protect group size */
    sd->ext_csd[EXT_CSD_S_C_VCC] = 0x01; /* Sleep current VCC  */
    sd->ext_csd[EXT_CSD_S_C_VCCQ] = 0x01; /* Sleep current VCCQ */
    sd->ext_csd[EXT_CSD_S_A_TIMEOUT] = 0x01; /* Sleep/Awake timeout */
    stl_le_p(&sd->ext_csd[EXT_CSD_SEC_CNT], sectcount); /* Sector count */
    sd->ext_csd[210] = 0x46; /* Min write perf for 8bit@52Mhz */
    sd->ext_csd[209] = 0x46; /* Min read perf for 8bit@52Mhz  */
    sd->ext_csd[208] = 0x46; /* Min write perf for 4bit@52Mhz */
    sd->ext_csd[207] = 0x46; /* Min read perf for 4bit@52Mhz */
    sd->ext_csd[206] = 0x46; /* Min write perf for 4bit@26Mhz */
    sd->ext_csd[205] = 0x46; /* Min read perf for 4bit@26Mhz */
    sd->ext_csd[EXT_CSD_CARD_TYPE] = 0b11;
    sd->ext_csd[EXT_CSD_STRUCTURE] = 2;
    sd->ext_csd[EXT_CSD_REV] = 5;
    sd->ext_csd[EXT_CSD_RPMB_MULT] = sd->rpmb_part_size / (128 * KiB);
    sd->ext_csd[EXT_CSD_PARTITION_SUPPORT] = 0b111;
    stl_le_p(&sd->ext_csd[EXT_CSD_CACHE_SIZE], sd->cache_size / KiB);

    /* Mode segment (RW) */
    sd->ext_csd[EXT_CSD_PART_CONFIG] = sd->boot_config;
}

static void emmc_set_csd(SDState *sd, uint64_t size)
{
    int hwblock_shift = HWBLOCK_SHIFT;
    uint32_t sectsize = (1 << (SECTOR_SHIFT + 1)) - 1;
    uint32_t wpsize = (1 << (WPGROUP_SHIFT + 1)) - 1;

    sd->csd[0] = (3 << 6) | (4 << 2); /* Spec v4.3 with EXT_CSD */
    sd->csd[1] = (1 << 3) | 6; /* Asynchronous data access time: 1ms */
    sd->csd[2] = 0x00;
    sd->csd[3] = (1 << 3) | 3;; /* Maximum bus clock frequency: 100MHz */
    sd->csd[4] = 0x0f;
    if (size <= 2 * GiB) {
        /* use 1k blocks */
        uint32_t csize1k = (size >> (CMULT_SHIFT + 10)) - 1;
        sd->csd[5] = 0x5a;
        sd->csd[6] = 0x80 | ((csize1k >> 10) & 0xf);
        sd->csd[7] = (csize1k >> 2) & 0xff;
    } else { /* >= 2GB : size stored in ext CSD, block addressing */
        sd->csd[5] = 0x59;
        sd->csd[6] = 0x8f;
        sd->csd[7] = 0xff;
        sd->ocr = FIELD_DP32(sd->ocr, OCR, CARD_CAPACITY, 1);
    }
    sd->csd[8] = 0xff;
    sd->csd[9] = 0xfc |     /* Max. write current */
        ((CMULT_SHIFT - 2) >> 1);
    sd->csd[10] = 0x40 |    /* Erase sector size */
        (((CMULT_SHIFT - 2) << 7) & 0x80) | (sectsize >> 1);
    sd->csd[11] = 0x00 |    /* Write protect group size */
        ((sectsize << 7) & 0x80) | wpsize;
    sd->csd[12] = 0x90 |    /* Write speed factor */
        (hwblock_shift >> 2);
    sd->csd[13] = 0x20 |    /* Max. write data block length */
        ((hwblock_shift << 6) & 0xc0);
    sd->csd[14] = 0x00;
    sd->csd[15] = (sd_crc7(sd->csd, 15) << 1) | 1;
    emmc_set_ext_csd(sd, size);
}

static void sd_set_csd(SDState *sd, uint64_t size)
{
    int hwblock_shift = HWBLOCK_SHIFT;
    uint32_t csize;
    uint32_t sectsize = (1 << (SECTOR_SHIFT + 1)) - 1;
    uint32_t wpsize = (1 << (WPGROUP_SHIFT + 1)) - 1;

    /* To indicate 2 GiB card, BLOCK_LEN shall be 1024 bytes */
    if (size == SDSC_MAX_CAPACITY) {
        hwblock_shift += 1;
    }
    csize = (size >> (CMULT_SHIFT + hwblock_shift)) - 1;

    if (size <= SDSC_MAX_CAPACITY) { /* Standard Capacity SD */
        sd->csd[0] = 0x00;      /* CSD structure */
        sd->csd[1] = 0x26;      /* Data read access-time-1 */
        sd->csd[2] = 0x00;      /* Data read access-time-2 */
        sd->csd[3] = 0x32;      /* Max. data transfer rate: 25 MHz */
        sd->csd[4] = 0x5f;      /* Card Command Classes */
        sd->csd[5] = 0x50 |     /* Max. read data block length */
            hwblock_shift;
        sd->csd[6] = 0xe0 |     /* Partial block for read allowed */
            ((csize >> 10) & 0x03);
        sd->csd[7] = 0x00 |     /* Device size */
            ((csize >> 2) & 0xff);
        sd->csd[8] = 0x3f |     /* Max. read current */
            ((csize << 6) & 0xc0);
        sd->csd[9] = 0xfc |     /* Max. write current */
            ((CMULT_SHIFT - 2) >> 1);
        sd->csd[10] = 0x40 |    /* Erase sector size */
            (((CMULT_SHIFT - 2) << 7) & 0x80) | (sectsize >> 1);
        sd->csd[11] = 0x00 |    /* Write protect group size */
            ((sectsize << 7) & 0x80) | wpsize;
        sd->csd[12] = 0x90 |    /* Write speed factor */
            (hwblock_shift >> 2);
        sd->csd[13] = 0x20 |    /* Max. write data block length */
            ((hwblock_shift << 6) & 0xc0);
        sd->csd[14] = 0x00;     /* File format group */
    } else {                    /* SDHC */
        size /= 512 * KiB;
        size -= 1;
        sd->csd[0] = 0x40;
        sd->csd[1] = 0x0e;
        sd->csd[2] = 0x00;
        sd->csd[3] = 0x32;
        sd->csd[4] = 0x5b;
        sd->csd[5] = 0x59;
        sd->csd[6] = 0x00;
        st24_be_p(&sd->csd[7], size);
        sd->csd[10] = 0x7f;
        sd->csd[11] = 0x80;
        sd->csd[12] = 0x0a;
        sd->csd[13] = 0x40;
        sd->csd[14] = 0x00;
    }
    sd->csd[15] = (sd_crc7(sd->csd, 15) << 1) | 1;
}

/* Relative Card Address register */

static void sd_set_rca(SDState *sd, uint16_t value)
{
    trace_sdcard_set_rca(value);
    sd->rca = value;
}

static uint16_t sd_req_get_rca(SDState *s, SDRequest req)
{
    switch (s->proto->cmd[req.cmd].type) {
    case sd_ac:
    case sd_adtc:
        return req.arg >> 16;
    case sd_spi:
    default:
        g_assert_not_reached();
    }
}

static bool sd_req_rca_same(SDState *s, SDRequest req)
{
    return sd_req_get_rca(s, req) == s->rca;
}

/* Card Status register */

FIELD(CSR, AKE_SEQ_ERROR,               3,  1)
FIELD(CSR, APP_CMD,                     5,  1)
FIELD(CSR, FX_EVENT,                    6,  1)
FIELD(CSR, SWITCH_ERROR,                7,  1)
FIELD(CSR, READY_FOR_DATA,              8,  1)
FIELD(CSR, CURRENT_STATE,               9,  4)
FIELD(CSR, ERASE_RESET,                13,  1)
FIELD(CSR, CARD_ECC_DISABLED,          14,  1)
FIELD(CSR, WP_ERASE_SKIP,              15,  1)
FIELD(CSR, CSD_OVERWRITE,              16,  1)
FIELD(CSR, DEFERRED_RESPONSE,          17,  1)
FIELD(CSR, ERROR,                      19,  1)
FIELD(CSR, CC_ERROR,                   20,  1)
FIELD(CSR, CARD_ECC_FAILED,            21,  1)
FIELD(CSR, ILLEGAL_COMMAND,            22,  1)
FIELD(CSR, COM_CRC_ERROR,              23,  1)
FIELD(CSR, LOCK_UNLOCK_FAILED,         24,  1)
FIELD(CSR, CARD_IS_LOCKED,             25,  1)
FIELD(CSR, WP_VIOLATION,               26,  1)
FIELD(CSR, ERASE_PARAM,                27,  1)
FIELD(CSR, ERASE_SEQ_ERROR,            28,  1)
FIELD(CSR, BLOCK_LEN_ERROR,            29,  1)
FIELD(CSR, ADDRESS_ERROR,              30,  1)
FIELD(CSR, OUT_OF_RANGE,               31,  1)

/* Card status bits, split by clear condition:
 * A : According to the card current state
 * B : Always related to the previous command
 * C : Cleared by read
 */
#define CARD_STATUS_A           (R_CSR_READY_FOR_DATA_MASK \
                               | R_CSR_CARD_ECC_DISABLED_MASK \
                               | R_CSR_CARD_IS_LOCKED_MASK)
#define CARD_STATUS_B           (R_CSR_CURRENT_STATE_MASK \
                               | R_CSR_ILLEGAL_COMMAND_MASK \
                               | R_CSR_COM_CRC_ERROR_MASK)
#define CARD_STATUS_C           (R_CSR_AKE_SEQ_ERROR_MASK \
                               | R_CSR_APP_CMD_MASK \
                               | R_CSR_ERASE_RESET_MASK \
                               | R_CSR_WP_ERASE_SKIP_MASK \
                               | R_CSR_CSD_OVERWRITE_MASK \
                               | R_CSR_ERROR_MASK \
                               | R_CSR_CC_ERROR_MASK \
                               | R_CSR_CARD_ECC_FAILED_MASK \
                               | R_CSR_LOCK_UNLOCK_FAILED_MASK \
                               | R_CSR_WP_VIOLATION_MASK \
                               | R_CSR_ERASE_PARAM_MASK \
                               | R_CSR_ERASE_SEQ_ERROR_MASK \
                               | R_CSR_BLOCK_LEN_ERROR_MASK \
                               | R_CSR_ADDRESS_ERROR_MASK \
                               | R_CSR_OUT_OF_RANGE_MASK)

static void sd_set_cardstatus(SDState *sd)
{
    sd->card_status = READY_FOR_DATA;
}

static void sd_set_sdstatus(SDState *sd)
{
    memset(sd->sd_status, 0, 64);
}

static const uint8_t sd_tuning_block_pattern4[64] = {
    /*
     * See: Physical Layer Simplified Specification Version 3.01,
     * Table 4-2.
     */
    0xff, 0x0f, 0xff, 0x00,     0x0f, 0xfc, 0xc3, 0xcc,
    0xc3, 0x3c, 0xcc, 0xff,     0xfe, 0xff, 0xfe, 0xef,
    0xff, 0xdf, 0xff, 0xdd,     0xff, 0xfb, 0xff, 0xfb,
    0xbf, 0xff, 0x7f, 0xff,     0x77, 0xf7, 0xbd, 0xef,
    0xff, 0xf0, 0xff, 0xf0,     0x0f, 0xfc, 0xcc, 0x3c,
    0xcc, 0x33, 0xcc, 0xcf,     0xff, 0xef, 0xff, 0xee,
    0xff, 0xfd, 0xff, 0xfd,     0xdf, 0xff, 0xbf, 0xff,
    0xbb, 0xff, 0xf7, 0xff,     0xf7, 0x7f, 0x7b, 0xde
};

static int sd_req_crc_validate(SDRequest *req)
{
    uint8_t buffer[5];
    buffer[0] = 0x40 | req->cmd;
    stl_be_p(&buffer[1], req->arg);
    return 0;
    return sd_crc7(buffer, 5) != req->crc;  /* TODO */
}

static size_t sd_response_size(SDState *sd, sd_rsp_type_t rtype)
{
    switch (rtype) {
    case sd_r1:
    case sd_r1b:
        return sd_is_spi(sd) ? 1 : 4;

    case spi_r2:
        assert(sd_is_spi(sd));
        return 2;

    case sd_r2_i:
    case sd_r2_s:
        assert(!sd_is_spi(sd));
        return 16;

    case sd_r3:
    case sd_r7:
        return sd_is_spi(sd) ? 5 : 4;

    case sd_r6:
        assert(!sd_is_spi(sd));
        return 4;

    case sd_r0:
    case sd_illegal:
        return sd_is_spi(sd) ? 1 : 0;

    default:
        g_assert_not_reached();
    }
}

static void sd_response_r1_make(SDState *sd, uint8_t *response)
{
    if (sd_is_spi(sd)) {
        response[0] = sd->state == sd_idle_state;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ERASE_RESET) << 1;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ILLEGAL_COMMAND) << 2;
        response[0] |= FIELD_EX32(sd->card_status, CSR, COM_CRC_ERROR) << 3;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ERASE_SEQ_ERROR) << 4;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ADDRESS_ERROR) << 5;
        response[0] |= FIELD_EX32(sd->card_status, CSR, BLOCK_LEN_ERROR) << 6;
        response[0] |= 0 << 7;
    } else {
        stl_be_p(response, sd->card_status);
    }

    /* Clear the "clear on read" status bits */
    sd->card_status &= ~CARD_STATUS_C;
}

static void spi_response_r2_make(SDState *sd, uint8_t *resp)
{
    /* Prepend R1 */
    sd_response_r1_make(sd, resp);

    resp[1]  = FIELD_EX32(sd->card_status, CSR, CARD_IS_LOCKED) << 0;
    resp[1] |= (FIELD_EX32(sd->card_status, CSR, LOCK_UNLOCK_FAILED)
                || FIELD_EX32(sd->card_status, CSR, WP_ERASE_SKIP)) << 1;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, ERROR) << 2;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, CC_ERROR) << 3;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, CARD_ECC_FAILED) << 4;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, WP_VIOLATION) << 5;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, ERASE_PARAM) << 6;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, OUT_OF_RANGE) << 7;
}

static void sd_response_r3_make(SDState *sd, uint8_t *response)
{
    if (sd_is_spi(sd)) {
        /* Prepend R1 */
        sd_response_r1_make(sd, response);
        response++;
    }
    stl_be_p(response, sd->ocr & ACMD41_R3_MASK);
}

static void sd_response_r6_make(SDState *sd, uint8_t *response)
{
    uint16_t status;

    status = ((sd->card_status >> 8) & 0xc000) |
             ((sd->card_status >> 6) & 0x2000) |
              (sd->card_status & 0x1fff);
    sd->card_status &= ~(CARD_STATUS_C & 0xc81fff);
    stw_be_p(response + 0, sd->rca);
    stw_be_p(response + 2, status);
}

static void sd_response_r7_make(SDState *sd, uint8_t *response)
{
    if (sd_is_spi(sd)) {
        /* Prepend R1 */
        sd_response_r1_make(sd, response);
        response++;
    }
    stl_be_p(response, sd->vhs);
}

static uint32_t sd_blk_len(SDState *sd)
{
    if (FIELD_EX32(sd->ocr, OCR, CARD_CAPACITY)) {
        return 1 << HWBLOCK_SHIFT;
    }
    return sd->blk_len;
}

/*
 * The legacy layout places boot0, boot1, RPMB, then the user area in one
 * backend.  Separate partition backends keep the user-area image at offset
 * zero, which allows an unmodified image produced by Raspberry Pi Imager to
 * be attached directly.
 */
static bool sd_has_separate_partitions(SDState *sd)
{
    return sd->boot_blk || sd->rpmb_blk;
}

static BlockBackend *sd_part_backend_for_access(SDState *sd,
                                                unsigned partition_access,
                                                uint64_t *offset)
{
    *offset = 0;
    if (!sd_is_emmc(sd)) {
        return sd->blk;
    }

    if (sd_has_separate_partitions(sd)) {
        switch (partition_access) {
        case EXT_CSD_PART_CONFIG_ACC_DEFAULT:
            return sd->blk;
        case EXT_CSD_PART_CONFIG_ACC_BOOT1:
            return sd->boot_blk;
        case EXT_CSD_PART_CONFIG_ACC_BOOT2:
            *offset = sd->boot_part_size;
            return sd->boot_blk;
        case EXT_CSD_PART_CONFIG_ACC_RPMB:
            return sd->rpmb_blk;
        default:
            g_assert_not_reached();
        }
    }

    switch (partition_access) {
    case EXT_CSD_PART_CONFIG_ACC_DEFAULT:
        *offset = sd->boot_part_size * 2 + sd->rpmb_part_size;
        break;
    case EXT_CSD_PART_CONFIG_ACC_BOOT1:
        break;
    case EXT_CSD_PART_CONFIG_ACC_BOOT2:
        *offset = sd->boot_part_size;
        break;
    case EXT_CSD_PART_CONFIG_ACC_RPMB:
        *offset = sd->boot_part_size * 2;
        break;
    default:
        g_assert_not_reached();
    }
    return sd->blk;
}

static unsigned sd_current_partition(SDState *sd)
{
    return sd_is_emmc(sd) ?
        sd->ext_csd[EXT_CSD_PART_CONFIG] & EXT_CSD_PART_CONFIG_ACC_MASK :
        EXT_CSD_PART_CONFIG_ACC_DEFAULT;
}

static BlockBackend *sd_part_backend(SDState *sd, uint64_t *offset)
{
    return sd_part_backend_for_access(sd, sd_current_partition(sd), offset);
}

static uint64_t sd_req_get_address(SDState *sd, SDRequest req)
{
    uint64_t addr;

    if (FIELD_EX32(sd->ocr, OCR, CARD_CAPACITY)) {
        addr = (uint64_t) req.arg << HWBLOCK_SHIFT;
    } else {
        addr = req.arg;
    }
    trace_sdcard_req_addr(req.arg, addr);
    return addr;
}

static inline uint64_t sd_addr_to_wpnum(uint64_t addr)
{
    return addr >> (HWBLOCK_SHIFT + SECTOR_SHIFT + WPGROUP_SHIFT);
}

static void sd_reset(DeviceState *dev)
{
    SDState *sd = SDMMC_COMMON(dev);
    SDCardClass *sc = SDMMC_COMMON_GET_CLASS(sd);
    uint64_t size;
    uint64_t sect;

    trace_sdcard_reset();
    timer_del(sd->cache_flush_timer);
    timer_del(sd->program_timer);
    timer_del(sd->erase_timer);
    sd->cache_flush_active = false;
    sd->cache_flush_deadline_us = 0;
    sd->cache_flush_completed_sectors = 0;
    sd->program_active = false;
    sd->program_deadline_us = 0;
    sd->program_completed_sectors = 0;
    sd_emmc_program_drop(sd);
    sd->erase_active = false;
    sd->erase_next = UINT64_MAX;
    sd->erase_last = 0;
    sd->erase_deadline_us = 0;
    sd->erase_completed_groups = 0;
    sd->erase_partition = 0;
    if (sd_is_emmc(sd) && sd->cache_count) {
        if (sd->cache_power_loss_on_reset) {
            sd_emmc_cache_drop(sd);
        } else if (!sd_emmc_cache_flush(sd)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "eMMC cache flush failed during reset\n");
        }
    }
    if (sd->blk) {
        blk_get_geometry(sd->blk, &sect);
    } else {
        sect = 0;
    }
    size = sect << HWBLOCK_SHIFT;
    if (sd_is_emmc(sd) && !sd_has_separate_partitions(sd)) {
        size -= sd->boot_part_size * 2 + sd->rpmb_part_size;
    }

    sect = sd_addr_to_wpnum(size) + 1;

    sd->state = sd_idle_state;

    /* card registers */
    sd->rca = sd_is_emmc(sd) ? 0x0001 : 0x0000;
    sd->size = size;
    sd_set_ocr(sd);
    sd_set_scr(sd);
    sc->set_cid(sd);
    sc->set_csd(sd, size);
    sd_set_cardstatus(sd);
    sd_set_sdstatus(sd);

    g_free(sd->wp_group_bmap);
    sd->wp_switch = sd->blk ? !blk_is_writable(sd->blk) : false;
    sd->wp_group_bits = sect;
    sd->wp_group_bmap = bitmap_new(sd->wp_group_bits);
    memset(sd->function_group, 0, sizeof(sd->function_group));
    sd->erase_start = INVALID_ADDRESS;
    sd->erase_end = INVALID_ADDRESS;
    sd->blk_len = 0x200;
    sd->pwd_len = 0;
    sd->expecting_acmd = false;
    sd->dat_lines = 0xf;
    sd->cmd_line = true;
    sd->multi_blk_cnt = 0;
    sd->reliable_write = false;
}

static bool sd_get_inserted(SDState *sd)
{
    return sd->blk && blk_is_inserted(sd->blk);
}

static bool sd_get_readonly(SDState *sd)
{
    return sd->wp_switch;
}

static void sd_cardchange(void *opaque, bool load, Error **errp)
{
    SDState *sd = opaque;
    DeviceState *dev = DEVICE(sd);
    SDBus *sdbus;
    bool inserted = sd_get_inserted(sd);
    bool readonly = sd_get_readonly(sd);

    if (inserted) {
        trace_sdcard_inserted(readonly);
        sd_reset(dev);
    } else {
        trace_sdcard_ejected();
    }

    sdbus = SD_BUS(qdev_get_parent_bus(dev));
    sdbus_set_inserted(sdbus, inserted);
    if (inserted) {
        sdbus_set_readonly(sdbus, readonly);
    }
}

static const BlockDevOps sd_block_ops = {
    .change_media_cb = sd_cardchange,
};

static const BlockDevOps emmc_block_ops = {
};

static bool sd_ocr_vmstate_needed(void *opaque)
{
    SDState *sd = opaque;

    /* Include the OCR state (and timer) if it is not yet powered up */
    return !FIELD_EX32(sd->ocr, OCR, CARD_POWER_UP);
}

static const VMStateDescription sd_ocr_vmstate = {
    .name = "sd-card/ocr-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sd_ocr_vmstate_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ocr, SDState),
        VMSTATE_TIMER_PTR(ocr_power_timer, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_rpmb(void *opaque)
{
    SDState *sd = opaque;

    return sd->rpmb_part_size > 0;
}

static const VMStateDescription emmc_rpmb_vmstate = {
    .name = "sd-card/rpmb-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_rpmb,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(rpmb.result.key_mac, SDState, RPMB_KEY_MAC_LEN),
        VMSTATE_UINT8_ARRAY(rpmb.result.data, SDState, RPMB_DATA_LEN),
        VMSTATE_UINT8_ARRAY(rpmb.result.nonce, SDState, RPMB_NONCE_LEN),
        VMSTATE_UINT32(rpmb.result.write_counter, SDState),
        VMSTATE_UINT16(rpmb.result.address, SDState),
        VMSTATE_UINT16(rpmb.result.block_count, SDState),
        VMSTATE_UINT16(rpmb.result.result, SDState),
        VMSTATE_UINT16(rpmb.result.req_resp, SDState),
        VMSTATE_UINT32(rpmb.write_counter, SDState),
        VMSTATE_UINT8_ARRAY(rpmb.key, SDState, 32),
        VMSTATE_UINT8(rpmb.key_set, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_emmc(void *opaque)
{
    SDState *sd = opaque;

    return sd_is_emmc(sd);
}

static const VMStateDescription emmc_extcsd_vmstate = {
    .name = "sd-card/ext_csd_modes-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_emmc,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ext_csd_rw, SDState, 192),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription emmc_cache_entry_vmstate = {
    .name = "sd-card/emmc-cache-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(addr, EMMCCacheEntry),
        VMSTATE_UINT8(partition, EMMCCacheEntry),
        VMSTATE_UINT8_ARRAY(data, EMMCCacheEntry, 512),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_emmc_cache(void *opaque)
{
    SDState *sd = opaque;

    return sd_is_emmc(sd) &&
           (sd->cache_count || sd->cache_flush_active);
}

static void sd_emmc_cache_rebuild_index(SDState *sd)
{
    g_hash_table_remove_all(sd->cache_index);
    for (uint32_t i = 0; i < sd->cache_count; i++) {
        EMMCCacheEntry *entry = &sd->cache_entries[i];

        entry->key = (entry->addr >> HWBLOCK_SHIFT) |
                     ((uint64_t)entry->partition << 56);
        g_hash_table_insert(sd->cache_index, &entry->key,
                            GUINT_TO_POINTER(i + 1));
    }
}

/*
 * The entry arrays are decoded straight after their counts, so a count that
 * post_load would have rejected has already been used to size the decode.
 * Validate both counts as they are read instead, while keeping the wire
 * format identical to the plain integer fields they replace.
 */
static int get_emmc_cache_count(QEMUFile *f, void *pv, size_t size,
                                const VMStateField *field)
{
    SDState *sd = container_of(pv, SDState, cache_count);
    uint32_t value = qemu_get_be32(f);

    if (value > sd->cache_capacity) {
        return -EINVAL;
    }
    sd->cache_count = value;
    return 0;
}

static int put_emmc_cache_count(QEMUFile *f, void *pv, size_t size,
                                const VMStateField *field,
                                JSONWriter *vmdesc)
{
    qemu_put_be32(f, *(uint32_t *)pv);
    return 0;
}

static const VMStateInfo vmstate_info_emmc_cache_count = {
    .name = "sd-card/emmc-cache-count",
    .get = get_emmc_cache_count,
    .put = put_emmc_cache_count,
};

static int get_emmc_program_count(QEMUFile *f, void *pv, size_t size,
                                  const VMStateField *field)
{
    int32_t value = qemu_get_be32(f);

    if (value < 0 || value > UINT16_MAX) {
        return -EINVAL;
    }
    *(int32_t *)pv = value;
    return 0;
}

static int put_emmc_program_count(QEMUFile *f, void *pv, size_t size,
                                  const VMStateField *field,
                                  JSONWriter *vmdesc)
{
    qemu_put_be32(f, *(int32_t *)pv);
    return 0;
}

static const VMStateInfo vmstate_info_emmc_program_count = {
    .name = "sd-card/emmc-program-count",
    .get = get_emmc_program_count,
    .put = put_emmc_program_count,
};

static int emmc_cache_post_load(void *opaque, int version_id)
{
    SDState *sd = opaque;

    if (version_id < 2) {
        sd->cache_flush_active = false;
        sd->cache_flush_deadline_us = 0;
        sd->cache_flush_completed_sectors = 0;
        timer_del(sd->cache_flush_timer);
    }
    if (!sd->cache_index || sd->cache_count > sd->cache_capacity) {
        return -EINVAL;
    }
    if (sd->cache_flush_active &&
        (!sd->cache_count || !sd->cache_flush_sector_delay_us ||
         !timer_pending(sd->cache_flush_timer))) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < sd->cache_count; i++) {
        if (sd->cache_entries[i].partition >
            EXT_CSD_PART_CONFIG_ACC_RPMB) {
            return -EINVAL;
        }
    }
    sd_emmc_cache_rebuild_index(sd);
    return 0;
}

static const VMStateDescription emmc_cache_vmstate = {
    .name = "sd-card/emmc-cache-state",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_emmc_cache,
    .post_load = emmc_cache_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(cache_size, SDState),
        {
            .name = "cache_count",
            .version_id = 0,
            .size = sizeof(uint32_t),
            .info = &vmstate_info_emmc_cache_count,
            .flags = VMS_SINGLE,
            .offset = offsetof(SDState, cache_count),
        },
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(
            cache_entries, SDState, cache_count,
            emmc_cache_entry_vmstate, EMMCCacheEntry),
        VMSTATE_UINT64_EQUAL_V(cache_flush_sector_delay_us, SDState, 2),
        VMSTATE_UINT64_V(cache_flush_deadline_us, SDState, 2),
        VMSTATE_UINT64_V(cache_flush_completed_sectors, SDState, 2),
        VMSTATE_BOOL_V(cache_flush_active, SDState, 2),
        VMSTATE_TIMER_PTR_V(cache_flush_timer, SDState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription emmc_program_entry_vmstate = {
    .name = "sd-card/emmc-program-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(addr, EMMCProgramEntry),
        VMSTATE_UINT8(partition, EMMCProgramEntry),
        VMSTATE_UINT8_ARRAY(data, EMMCProgramEntry, 512),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_emmc_program(void *opaque)
{
    SDState *sd = opaque;

    return sd_is_emmc(sd) && (sd->program_count || sd->program_active);
}

static int emmc_program_post_load(void *opaque, int version_id)
{
    SDState *sd = opaque;

    if (sd->program_count < 0 || sd->program_count > UINT16_MAX ||
        (sd->program_active &&
         (!sd->program_count || !sd->program_sector_delay_us ||
          !timer_pending(sd->program_timer)))) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < sd->program_count; i++) {
        if (sd->program_entries[i].partition >
            EXT_CSD_PART_CONFIG_ACC_RPMB) {
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription emmc_program_vmstate = {
    .name = "sd-card/emmc-program-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_emmc_program,
    .post_load = emmc_program_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(program_sector_delay_us, SDState),
        VMSTATE_UINT64(program_deadline_us, SDState),
        VMSTATE_UINT64(program_completed_sectors, SDState),
        {
            .name = "program_count",
            .version_id = 0,
            .size = sizeof(int32_t),
            .info = &vmstate_info_emmc_program_count,
            .flags = VMS_SINGLE,
            .offset = offsetof(SDState, program_count),
        },
        VMSTATE_STRUCT_VARRAY_ALLOC(
            program_entries, SDState, program_count, 1,
            emmc_program_entry_vmstate, EMMCProgramEntry),
        VMSTATE_BOOL(program_active, SDState),
        VMSTATE_TIMER_PTR(program_timer, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_emmc_erase(void *opaque)
{
    SDState *sd = opaque;

    return sd_is_emmc(sd) &&
           (sd->erase_active || sd->erase_next != UINT64_MAX);
}

static int emmc_erase_post_load(void *opaque, int version_id)
{
    SDState *sd = opaque;

    if (sd->erase_next == UINT64_MAX || sd->erase_next > sd->erase_last ||
        sd->erase_partition >= EXT_CSD_PART_CONFIG_ACC_RPMB ||
        (sd->erase_active &&
         (!sd->erase_group_delay_us ||
          !timer_pending(sd->erase_timer)))) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription emmc_erase_vmstate = {
    .name = "sd-card/emmc-erase-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_emmc_erase,
    .post_load = emmc_erase_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(erase_group_delay_us, SDState),
        VMSTATE_UINT64(erase_next, SDState),
        VMSTATE_UINT64(erase_last, SDState),
        VMSTATE_UINT64(erase_deadline_us, SDState),
        VMSTATE_UINT64(erase_completed_groups, SDState),
        VMSTATE_UINT8(erase_partition, SDState),
        VMSTATE_BOOL(erase_active, SDState),
        VMSTATE_TIMER_PTR(erase_timer, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static int sd_vmstate_pre_load(void *opaque)
{
    SDState *sd = opaque;

    /* If the OCR state is not included (prior versions, or not
     * needed), then the OCR must be set as powered up. If the OCR state
     * is included, this will be replaced by the state restore.
     */
    sd_ocr_powerup(sd);

    return 0;
}

static const VMStateDescription sd_vmstate = {
    .name = "sd-card",
    .version_id = 3,
    .minimum_version_id = 2,
    .pre_load = sd_vmstate_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UNUSED(4),
        VMSTATE_INT32(state, SDState),
        VMSTATE_UINT8_ARRAY(cid, SDState, 16),
        VMSTATE_UINT8_ARRAY(csd, SDState, 16),
        VMSTATE_UINT16(rca, SDState),
        VMSTATE_UINT32(card_status, SDState),
        VMSTATE_PARTIAL_BUFFER(sd_status, SDState, 1),
        VMSTATE_UINT32(vhs, SDState),
        VMSTATE_BITMAP(wp_group_bmap, SDState, 0, wp_group_bits),
        VMSTATE_UINT32(blk_len, SDState),
        VMSTATE_UINT32(multi_blk_cnt, SDState),
        VMSTATE_BOOL_V(reliable_write, SDState, 3),
        VMSTATE_UINT32(erase_start, SDState),
        VMSTATE_UINT32(erase_end, SDState),
        VMSTATE_UINT8_ARRAY(pwd, SDState, 16),
        VMSTATE_UINT32(pwd_len, SDState),
        VMSTATE_UINT8_ARRAY(function_group, SDState, 6),
        VMSTATE_UINT8(current_cmd, SDState),
        VMSTATE_BOOL(expecting_acmd, SDState),
        VMSTATE_UINT32(blk_written, SDState),
        VMSTATE_UINT64(data_start, SDState),
        VMSTATE_UINT32(data_offset, SDState),
        VMSTATE_UINT8_ARRAY(data, SDState, 512),
        VMSTATE_UNUSED_V(1, 512),
        VMSTATE_UNUSED(1),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &sd_ocr_vmstate,
        &emmc_extcsd_vmstate,
        &emmc_rpmb_vmstate,
        &emmc_cache_vmstate,
        &emmc_program_vmstate,
        &emmc_erase_vmstate,
        NULL
    },
};

static int sd_partition_pread(SDState *sd, uint64_t addr, uint64_t len,
                              void *buf)
{
    uint64_t offset;
    BlockBackend *blk = sd_part_backend(sd, &offset);

    return blk ? blk_pread(blk, addr + offset, len, buf, 0) : -ENOMEDIUM;
}

static int sd_partition_pwrite(SDState *sd, uint64_t addr, uint64_t len,
                               const void *buf)
{
    uint64_t offset;
    BlockBackend *blk = sd_part_backend(sd, &offset);

    return blk ? blk_pwrite(blk, addr + offset, len, buf, 0) : -ENOMEDIUM;
}

static void sd_emmc_cache_drop(SDState *sd)
{
    sd->cache_count = 0;
    if (sd->cache_index) {
        g_hash_table_remove_all(sd->cache_index);
    }
}

static bool sd_emmc_cache_commit_first(SDState *sd)
{
    EMMCCacheEntry *entry;
    uint64_t offset;
    BlockBackend *blk;

    if (!sd->cache_count) {
        return true;
    }
    entry = &sd->cache_entries[0];
    blk = sd_part_backend_for_access(sd, entry->partition, &offset);
    if (!blk ||
        blk_pwrite(blk, entry->addr + offset, sizeof(entry->data),
                   entry->data, 0) < 0 ||
        blk_flush(blk) < 0) {
        return false;
    }

    sd->cache_count--;
    if (sd->cache_count) {
        memmove(sd->cache_entries, sd->cache_entries + 1,
                sd->cache_count * sizeof(*sd->cache_entries));
    }
    sd_emmc_cache_rebuild_index(sd);
    return true;
}

static void sd_emmc_cache_flush_timer(void *opaque)
{
    SDState *sd = opaque;

    if (!sd->cache_flush_active) {
        return;
    }
    if (!sd_emmc_cache_commit_first(sd)) {
        sd->card_status |= R_CSR_ERROR_MASK;
        sd->cache_flush_active = false;
        sd->cache_flush_deadline_us = 0;
        sd->state = sd_transfer_state;
        return;
    }

    sd->cache_flush_completed_sectors++;
    if (!sd->cache_count) {
        sd->cache_flush_active = false;
        sd->cache_flush_deadline_us = 0;
        sd->state = sd_transfer_state;
        return;
    }

    sd->cache_flush_deadline_us += sd->cache_flush_sector_delay_us;
    timer_mod(sd->cache_flush_timer, sd->cache_flush_deadline_us);
}

static bool sd_emmc_cache_flush(SDState *sd)
{
    uint32_t completed = 0;
    bool flush_user = false;
    bool flush_boot = false;
    bool flush_rpmb = false;

    if (!sd->cache_count) {
        uint64_t offset;
        BlockBackend *blk = sd_part_backend(sd, &offset);

        flush_user = blk == sd->blk;
        flush_boot = blk == sd->boot_blk;
        flush_rpmb = blk == sd->rpmb_blk;
    }

    for (; completed < sd->cache_count; completed++) {
        EMMCCacheEntry *entry = &sd->cache_entries[completed];
        uint64_t offset;
        BlockBackend *blk = sd_part_backend_for_access(
            sd, entry->partition, &offset);

        if (!blk ||
            blk_pwrite(blk, entry->addr + offset, sizeof(entry->data),
                       entry->data, 0) < 0) {
            break;
        }
        flush_user |= blk == sd->blk;
        flush_boot |= blk == sd->boot_blk;
        flush_rpmb |= blk == sd->rpmb_blk;
    }

    if (completed != sd->cache_count) {
        bool completed_durable =
            (!flush_user || blk_flush(sd->blk) == 0) &&
            (!flush_boot || blk_flush(sd->boot_blk) == 0) &&
            (!flush_rpmb || blk_flush(sd->rpmb_blk) == 0);

        if (completed && completed_durable) {
            memmove(sd->cache_entries, sd->cache_entries + completed,
                    (sd->cache_count - completed) *
                    sizeof(*sd->cache_entries));
            sd->cache_count -= completed;
            sd_emmc_cache_rebuild_index(sd);
        }
        return false;
    }

    if ((flush_user && blk_flush(sd->blk) < 0) ||
        (flush_boot && blk_flush(sd->boot_blk) < 0) ||
        (flush_rpmb && blk_flush(sd->rpmb_blk) < 0)) {
        return false;
    }

    sd_emmc_cache_drop(sd);
    return true;
}

static void sd_emmc_program_drop(SDState *sd)
{
    g_clear_pointer(&sd->program_entries, g_free);
    sd->program_count = 0;
}

static EMMCProgramEntry *sd_emmc_program_lookup(SDState *sd, uint64_t addr,
                                                unsigned int partition)
{
    for (uint32_t i = 0; i < sd->program_count; i++) {
        EMMCProgramEntry *entry = &sd->program_entries[i];

        if (entry->addr == addr && entry->partition == partition) {
            return entry;
        }
    }
    return NULL;
}

static void sd_emmc_program_timer(void *opaque)
{
    SDState *sd = opaque;
    EMMCProgramEntry *entry;
    uint64_t offset;
    BlockBackend *blk;

    if (!sd->program_active || !sd->program_count) {
        return;
    }
    entry = &sd->program_entries[0];
    blk = sd_part_backend_for_access(sd, entry->partition, &offset);
    if (!blk ||
        blk_pwrite(blk, entry->addr + offset, sizeof(entry->data),
                   entry->data, 0) < 0 ||
        blk_flush(blk) < 0) {
        sd->card_status |= R_CSR_ERROR_MASK;
        sd->program_active = false;
        sd->program_deadline_us = 0;
        if (sd->state == sd_programming_state) {
            sd->state = sd_transfer_state;
        }
        return;
    }

    sd->program_count--;
    if (sd->program_count) {
        memmove(sd->program_entries, sd->program_entries + 1,
                sd->program_count * sizeof(*sd->program_entries));
    } else {
        g_clear_pointer(&sd->program_entries, g_free);
    }
    sd->program_completed_sectors++;
    if (!sd->program_count) {
        sd->program_active = false;
        sd->program_deadline_us = 0;
        if (sd->state == sd_programming_state) {
            sd->state = sd_transfer_state;
        }
        return;
    }

    sd->program_deadline_us += sd->program_sector_delay_us;
    timer_mod(sd->program_timer, sd->program_deadline_us);
}

static bool sd_emmc_program_write(SDState *sd, uint64_t addr, uint32_t len,
                                  const void *buf)
{
    unsigned int partition;
    EMMCProgramEntry *entry;
    bool new_transaction;

    if (!sd_is_emmc(sd) || !sd->program_sector_delay_us ||
        len != sizeof(entry->data) ||
        !QEMU_IS_ALIGNED(addr, sizeof(entry->data))) {
        return false;
    }
    partition = sd_current_partition(sd);
    if (partition == EXT_CSD_PART_CONFIG_ACC_RPMB) {
        return false;
    }

    entry = sd_emmc_program_lookup(sd, addr, partition);
    if (!entry) {
        if (sd->program_count == UINT16_MAX) {
            sd->card_status |= R_CSR_ERROR_MASK;
            return true;
        }
        new_transaction = !sd->program_count;
        sd->program_entries = g_renew(
            EMMCProgramEntry, sd->program_entries, sd->program_count + 1);
        entry = &sd->program_entries[sd->program_count++];
        entry->addr = addr;
        entry->partition = partition;
        if (new_transaction) {
            sd->program_completed_sectors = 0;
        }
    }
    memcpy(entry->data, buf, sizeof(entry->data));

    if (!sd->program_active) {
        sd->program_active = true;
        sd->program_deadline_us =
            qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
            sd->program_sector_delay_us;
        timer_mod(sd->program_timer, sd->program_deadline_us);
    }
    return true;
}

static bool sd_emmc_cache_enabled(SDState *sd)
{
    return sd_is_emmc(sd) && sd->cache_capacity &&
           (sd->ext_csd[EXT_CSD_CACHE_CTRL] & 1);
}

static EMMCCacheEntry *sd_emmc_cache_lookup(SDState *sd, uint64_t addr,
                                            unsigned partition)
{
    uint64_t key = (addr >> HWBLOCK_SHIFT) |
                   ((uint64_t)partition << 56);
    gpointer value;

    if (!sd->cache_index) {
        return NULL;
    }
    value = g_hash_table_lookup(sd->cache_index, &key);
    return value ? &sd->cache_entries[GPOINTER_TO_UINT(value) - 1] : NULL;
}

static bool sd_emmc_cache_write(SDState *sd, uint64_t addr, uint32_t len,
                                const void *buf)
{
    unsigned partition = sd_current_partition(sd);
    EMMCCacheEntry *entry;

    if (sd->reliable_write || !sd_emmc_cache_enabled(sd) ||
        len != sizeof(entry->data) ||
        !QEMU_IS_ALIGNED(addr, sizeof(entry->data)) ||
        partition == EXT_CSD_PART_CONFIG_ACC_RPMB) {
        return false;
    }

    entry = sd_emmc_cache_lookup(sd, addr, partition);
    if (!entry) {
        if (sd->cache_count == sd->cache_capacity &&
            !sd_emmc_cache_flush(sd)) {
            sd->card_status |= R_CSR_ERROR_MASK;
            return true;
        }
        entry = &sd->cache_entries[sd->cache_count++];
        entry->addr = addr;
        entry->partition = partition;
        entry->key = (addr >> HWBLOCK_SHIFT) |
                     ((uint64_t)partition << 56);
        g_hash_table_insert(sd->cache_index, &entry->key,
                            GUINT_TO_POINTER(sd->cache_count));
    }
    memcpy(entry->data, buf, sizeof(entry->data));
    return true;
}

static void sd_blk_read(SDState *sd, uint64_t addr, uint32_t len)
{
    EMMCCacheEntry *entry;
    EMMCProgramEntry *program_entry;

    trace_sdcard_read_block(addr, len);
    if (sd_partition_pread(sd, addr, len, sd->data) < 0) {
        fprintf(stderr, "sd_blk_read: read error on host side\n");
    }
    entry = sd_emmc_cache_lookup(sd, addr, sd_current_partition(sd));
    if (entry && len <= sizeof(entry->data)) {
        memcpy(sd->data, entry->data, len);
    }
    program_entry = sd_emmc_program_lookup(
        sd, addr, sd_current_partition(sd));
    if (program_entry && len <= sizeof(program_entry->data)) {
        memcpy(sd->data, program_entry->data, len);
    }
}

static void sd_blk_write(SDState *sd, uint64_t addr, uint32_t len)
{
    uint64_t offset;
    BlockBackend *blk;

    trace_sdcard_write_block(addr, len);
    if (sd_emmc_cache_write(sd, addr, len, sd->data)) {
        return;
    }
    if (sd_emmc_program_write(sd, addr, len, sd->data)) {
        return;
    }
    if (sd_partition_pwrite(sd, addr, len, sd->data) < 0) {
        fprintf(stderr, "sd_blk_write: write error on host side\n");
        return;
    }
    if (sd->reliable_write) {
        blk = sd_part_backend(sd, &offset);
        if (!blk || blk_flush(blk) < 0) {
            sd->card_status |= R_CSR_ERROR_MASK;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "eMMC reliable write flush failed\n");
        }
    }
}

static bool rpmb_calc_hmac(SDState *sd, const RPMBDataFrame *frame,
                           unsigned int num_blocks, uint8_t *mac)
{
    g_autoptr(QCryptoHmac) hmac = NULL;
    size_t mac_len = RPMB_KEY_MAC_LEN;
    bool success = true;
    Error *err = NULL;
    uint64_t offset;

    hmac = qcrypto_hmac_new(QCRYPTO_HASH_ALGO_SHA256, sd->rpmb.key,
                            RPMB_KEY_MAC_LEN, &err);
    if (!hmac) {
        error_report_err(err);
        return false;
    }

    /*
     * This implies a read request because we only support single-block write
     * requests so far.
     */
    if (num_blocks > 1) {
        /*
         * Unfortunately, the underlying crypto libraries do not allow us to
         * migrate an active QCryptoHmac state. Therefore, we have to calculate
         * the HMAC in one run. To avoid buffering a complete read sequence in
         * SDState, reconstruct all frames except for the last one.
         */
        void *buf = sd->data;

        assert(RPMB_HASH_LEN <= sizeof(sd->data));

        /*
         * We will hash everything from data field to the end of RPMBDataFrame.
         */
        memcpy((uint8_t *)buf + RPMB_DATA_LEN,
               (uint8_t *)frame + offsetof(RPMBDataFrame, nonce),
               RPMB_HASH_LEN - RPMB_DATA_LEN);

        offset = lduw_be_p(&frame->address) * RPMB_DATA_LEN;
        do {
            if (sd_partition_pread(sd, offset, RPMB_DATA_LEN, buf) < 0) {
                error_report("sd_blk_read: read error on host side");
                success = false;
                break;
            }
            if (qcrypto_hmac_bytes(hmac, buf, RPMB_HASH_LEN, NULL, NULL,
                                   &err) < 0) {
                error_report_err(err);
                success = false;
                break;
            }
            offset += RPMB_DATA_LEN;
        } while (--num_blocks > 1);
    }

    if (success &&
        qcrypto_hmac_bytes(hmac, frame->data, RPMB_HASH_LEN, &mac,
                           &mac_len, &err) < 0) {
        error_report_err(err);
        success = false;
    }
    assert(!success || mac_len == RPMB_KEY_MAC_LEN);

    return success;
}

static void emmc_rpmb_blk_read(SDState *sd, uint64_t addr, uint32_t len)
{
    uint16_t resp = lduw_be_p(&sd->rpmb.result.req_resp);
    uint16_t result = lduw_be_p(&sd->rpmb.result.result);
    unsigned int curr_block = 0;

    if ((result & ~RPMB_RESULT_COUTER_EXPIRED) == RPMB_RESULT_OK &&
        resp == RPMB_RESP(RPMB_REQ_AUTH_DATA_READ)) {
        curr_block = lduw_be_p(&sd->rpmb.result.address);
        if (sd->rpmb.result.block_count == 0) {
            stw_be_p(&sd->rpmb.result.block_count, sd->multi_blk_cnt);
        } else {
            curr_block += lduw_be_p(&sd->rpmb.result.block_count);
            curr_block -= sd->multi_blk_cnt;
        }
        addr = curr_block * RPMB_DATA_LEN;
        if (sd_partition_pread(sd, addr, RPMB_DATA_LEN,
                               sd->rpmb.result.data) < 0) {
            error_report("sd_blk_read: read error on host side");
            memset(sd->rpmb.result.data, 0, sizeof(sd->rpmb.result.data));
            stw_be_p(&sd->rpmb.result.result,
                     RPMB_RESULT_READ_FAILURE
                     | (result & RPMB_RESULT_COUTER_EXPIRED));
        }
        if (sd->multi_blk_cnt == 1 &&
            !rpmb_calc_hmac(sd, &sd->rpmb.result,
                            lduw_be_p(&sd->rpmb.result.block_count),
                            sd->rpmb.result.key_mac)) {
            memset(sd->rpmb.result.data, 0, sizeof(sd->rpmb.result.data));
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_AUTH_FAILURE);
        }
    } else if (!rpmb_calc_hmac(sd, &sd->rpmb.result, 1,
                               sd->rpmb.result.key_mac)) {
        memset(sd->rpmb.result.data, 0, sizeof(sd->rpmb.result.data));
        stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_AUTH_FAILURE);
    }
    memcpy(sd->data, &sd->rpmb.result, sizeof(sd->rpmb.result));

    trace_sdcard_rpmb_read_block(resp, curr_block,
                                 lduw_be_p(&sd->rpmb.result.result));
}

static void emmc_rpmb_blk_write(SDState *sd, uint64_t addr, uint32_t len)
{
    RPMBDataFrame *frame = (RPMBDataFrame *)sd->data;
    uint16_t req = lduw_be_p(&frame->req_resp);
    uint8_t mac[RPMB_KEY_MAC_LEN];

    if (req == RPMB_REQ_READ_RESULT) {
        /* just return the current result register */
        goto exit;
    }
    memset(&sd->rpmb.result, 0, sizeof(sd->rpmb.result));
    memcpy(sd->rpmb.result.nonce, frame->nonce, sizeof(sd->rpmb.result.nonce));
    stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_OK);
    stw_be_p(&sd->rpmb.result.req_resp, RPMB_RESP(req));

    if (!sd->rpmb.key_set && req != RPMB_REQ_PROGRAM_AUTH_KEY) {
        stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_NO_AUTH_KEY);
        goto exit;
    }

    switch (req) {
    case RPMB_REQ_PROGRAM_AUTH_KEY:
        if (sd->rpmb.key_set) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_WRITE_FAILURE);
            break;
        }
        memcpy(sd->rpmb.key, frame->key_mac, sizeof(sd->rpmb.key));
        sd->rpmb.key_set = 1;
        break;
    case RPMB_REQ_READ_WRITE_COUNTER:
        stl_be_p(&sd->rpmb.result.write_counter, sd->rpmb.write_counter);
        break;
    case RPMB_REQ_AUTH_DATA_WRITE:
        /* We only support single-block writes so far */
        if (sd->multi_blk_cnt != 1) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_GENERAL_FAILURE);
            break;
        }
        if (sd->rpmb.write_counter == 0xffffffff) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_WRITE_FAILURE);
            break;
        }
        if (!rpmb_calc_hmac(sd, frame, 1, mac) ||
            memcmp(frame->key_mac, mac, RPMB_KEY_MAC_LEN) != 0) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_AUTH_FAILURE);
            break;
        }
        if (ldl_be_p(&frame->write_counter) != sd->rpmb.write_counter) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_COUNTER_FAILURE);
            break;
        }
        sd->rpmb.result.address = frame->address;
        addr = lduw_be_p(&frame->address) * RPMB_DATA_LEN;
        if (sd_partition_pwrite(sd, addr, RPMB_DATA_LEN, frame->data) < 0) {
            error_report("sd_blk_write: write error on host side");
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_WRITE_FAILURE);
        } else {
            sd->rpmb.write_counter++;
        }
        stl_be_p(&sd->rpmb.result.write_counter, sd->rpmb.write_counter);
        break;
    case RPMB_REQ_AUTH_DATA_READ:
        sd->rpmb.result.address = frame->address;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "RPMB request %d not implemented\n", req);
        stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_GENERAL_FAILURE);
        break;
    }
exit:
    if (sd->rpmb.write_counter == 0xffffffff) {
        stw_be_p(&sd->rpmb.result.result,
                 lduw_be_p(&sd->rpmb.result.result) | RPMB_RESULT_COUTER_EXPIRED);
    }
    trace_sdcard_rpmb_write_block(req, lduw_be_p(&sd->rpmb.result.result));
}

static void sd_emmc_cache_remove_range(SDState *sd, unsigned int partition,
                                       uint64_t start, uint64_t end)
{
    uint32_t keep = 0;

    for (uint32_t i = 0; i < sd->cache_count; i++) {
        EMMCCacheEntry *entry = &sd->cache_entries[i];
        bool overlaps = entry->partition == partition &&
                        entry->addr < end &&
                        entry->addr + sizeof(entry->data) > start;

        if (!overlaps) {
            if (keep != i) {
                sd->cache_entries[keep] = *entry;
            }
            keep++;
        }
    }
    if (keep != sd->cache_count) {
        sd->cache_count = keep;
        sd_emmc_cache_rebuild_index(sd);
    }
}

static bool sd_emmc_erase_current_group(SDState *sd)
{
    g_autofree uint8_t *erased = NULL;
    uint64_t offset;
    uint64_t length;
    BlockBackend *blk;

    if (sd->erase_next >= sd->erase_last) {
        return true;
    }
    length = MIN(EMMC_HC_ERASE_GROUP_BYTES,
                 sd->erase_last - sd->erase_next);
    erased = g_malloc(length);
    memset(erased, 0xff, length);
    blk = sd_part_backend_for_access(sd, sd->erase_partition, &offset);
    if (!blk ||
        blk_pwrite(blk, sd->erase_next + offset, length, erased, 0) < 0 ||
        blk_flush(blk) < 0) {
        return false;
    }

    sd->erase_next += length;
    sd->erase_completed_groups++;
    return true;
}

static void sd_emmc_erase_timer(void *opaque)
{
    SDState *sd = opaque;

    if (!sd->erase_active) {
        return;
    }
    if (!sd_emmc_erase_current_group(sd)) {
        sd->card_status |= R_CSR_ERROR_MASK;
        sd->erase_active = false;
        sd->erase_deadline_us = 0;
        sd->state = sd_transfer_state;
        return;
    }
    if (sd->erase_next >= sd->erase_last) {
        sd->erase_active = false;
        sd->erase_next = UINT64_MAX;
        sd->erase_last = 0;
        sd->erase_deadline_us = 0;
        sd->state = sd_transfer_state;
        return;
    }

    sd->erase_deadline_us += sd->erase_group_delay_us;
    timer_mod(sd->erase_timer, sd->erase_deadline_us);
}

static bool sd_erase(SDState *sd)
{
    uint64_t erase_start = sd->erase_start;
    uint64_t erase_end = sd->erase_end;
    unsigned int partition;
    bool sdsc = true;
    uint64_t wpnum;
    uint64_t erase_addr;
    int erase_len = 1 << HWBLOCK_SHIFT;

    trace_sdcard_erase(sd->erase_start, sd->erase_end);
    if (sd->erase_start == INVALID_ADDRESS ||
        sd->erase_end == INVALID_ADDRESS ||
        sd->erase_start > sd->erase_end) {
        sd->card_status |= ERASE_SEQ_ERROR;
        sd->erase_start = INVALID_ADDRESS;
        sd->erase_end = INVALID_ADDRESS;
        return false;
    }

    if (FIELD_EX32(sd->ocr, OCR, CARD_CAPACITY)) {
        /* High capacity memory card: erase units are 512 byte blocks */
        erase_start <<= HWBLOCK_SHIFT;
        erase_end <<= HWBLOCK_SHIFT;
        sdsc = false;
    }

    if (erase_start >= sd->size || erase_end >= sd->size) {
        sd->card_status |= OUT_OF_RANGE;
        sd->erase_start = INVALID_ADDRESS;
        sd->erase_end = INVALID_ADDRESS;
        return false;
    }

    sd->erase_start = INVALID_ADDRESS;
    sd->erase_end = INVALID_ADDRESS;
    sd->csd[14] |= 0x40;

    if (sd_is_emmc(sd)) {
        partition = sd_current_partition(sd);
        if (partition == EXT_CSD_PART_CONFIG_ACC_RPMB ||
            sd->program_count) {
            sd->card_status |= ERASE_SEQ_ERROR;
            return false;
        }

        sd_emmc_cache_remove_range(sd, partition, erase_start,
                                   erase_end + erase_len);
        sd->erase_next = erase_start;
        sd->erase_last = erase_end + erase_len;
        sd->erase_partition = partition;
        sd->erase_completed_groups = 0;
        if (sd->erase_group_delay_us) {
            sd->erase_active = true;
            sd->erase_deadline_us =
                qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                sd->erase_group_delay_us;
            timer_mod(sd->erase_timer, sd->erase_deadline_us);
            return true;
        }

        while (sd->erase_next < sd->erase_last) {
            if (!sd_emmc_erase_current_group(sd)) {
                sd->card_status |= R_CSR_ERROR_MASK;
                return false;
            }
        }
        sd->erase_next = UINT64_MAX;
        sd->erase_last = 0;
        return false;
    }

    memset(sd->data, 0xff, erase_len);
    for (erase_addr = erase_start; erase_addr <= erase_end;
         erase_addr += erase_len) {
        if (sdsc) {
            /* Only SDSC cards support write protect groups */
            wpnum = sd_addr_to_wpnum(erase_addr);
            assert(wpnum < sd->wp_group_bits);
            if (test_bit(wpnum, sd->wp_group_bmap)) {
                sd->card_status |= WP_ERASE_SKIP;
                continue;
            }
        }
        sd_blk_write(sd, erase_addr, erase_len);
    }
    return false;
}

static uint32_t sd_wpbits(SDState *sd, uint64_t addr)
{
    uint32_t i, wpnum;
    uint32_t ret = 0;

    wpnum = sd_addr_to_wpnum(addr);

    for (i = 0; i < 32; i++, wpnum++, addr += WPGROUP_SIZE) {
        if (addr >= sd->size) {
            /*
             * If the addresses of the last groups are outside the valid range,
             * then the corresponding write protection bits shall be set to 0.
             */
            continue;
        }
        assert(wpnum < sd->wp_group_bits);
        if (test_bit(wpnum, sd->wp_group_bmap)) {
            ret |= (1 << i);
        }
    }

    return ret;
}

enum ExtCsdAccessMode {
    EXT_CSD_ACCESS_MODE_COMMAND_SET = 0,
    EXT_CSD_ACCESS_MODE_SET_BITS    = 1,
    EXT_CSD_ACCESS_MODE_CLEAR_BITS  = 2,
    EXT_CSD_ACCESS_MODE_WRITE_BYTE  = 3
};

static void emmc_function_switch(SDState *sd, uint32_t arg)
{
    uint8_t access = extract32(arg, 24, 2);
    uint8_t index = extract32(arg, 16, 8);
    uint8_t value = extract32(arg, 8, 8);
    uint8_t b = sd->ext_csd[index];

    trace_sdcard_switch(access, index, value, extract32(arg, 0, 2));

    if (index >= 192) {
        qemu_log_mask(LOG_GUEST_ERROR, "MMC switching illegal offset\n");
        sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
        return;
    }

    if (index == EXT_CSD_FLUSH_CACHE) {
        if (access != EXT_CSD_ACCESS_MODE_WRITE_BYTE || value != 1) {
            sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
        } else if (sd->cache_flush_sector_delay_us && sd->cache_count) {
            sd->cache_flush_active = true;
            sd->cache_flush_completed_sectors = 0;
            sd->cache_flush_deadline_us =
                qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                sd->cache_flush_sector_delay_us;
            timer_mod(sd->cache_flush_timer,
                      sd->cache_flush_deadline_us);
        } else if (!sd_emmc_cache_flush(sd)) {
            sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
        }
        sd->ext_csd[index] = 0;
        return;
    }

    switch (access) {
    case EXT_CSD_ACCESS_MODE_COMMAND_SET:
        qemu_log_mask(LOG_UNIMP, "MMC Command set switching not supported\n");
        return;
    case EXT_CSD_ACCESS_MODE_SET_BITS:
        b |= value;
        break;
    case EXT_CSD_ACCESS_MODE_CLEAR_BITS:
        b &= ~value;
        break;
    case EXT_CSD_ACCESS_MODE_WRITE_BYTE:
        b = value;
        break;
    }

    if (index == EXT_CSD_CACHE_CTRL) {
        if ((b & ~1) || ((b & 1) && !sd->cache_capacity)) {
            sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
            return;
        }
        if (!(b & 1) && sd->cache_count && !sd_emmc_cache_flush(sd)) {
            sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
            return;
        }
    }

    if (index == EXT_CSD_PART_CONFIG) {
        uint8_t part = b & EXT_CSD_PART_CONFIG_ACC_MASK;

        if (((part == EXT_CSD_PART_CONFIG_ACC_BOOT1 ||
              part == EXT_CSD_PART_CONFIG_ACC_BOOT2) && !sd->boot_part_size) ||
            (part == EXT_CSD_PART_CONFIG_ACC_RPMB && !sd->rpmb_part_size)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "MMC switching to illegal partition\n");
            sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
            return;
        }
    }

    trace_sdcard_ext_csd_update(index, sd->ext_csd[index], b);
    sd->ext_csd[index] = b;
}

static void sd_function_switch(SDState *sd, uint32_t arg)
{
    int i, mode, new_func;
    mode = !!(arg & 0x80000000);

    sd->data[0] = 0x00;     /* Maximum current consumption */
    sd->data[1] = 0x01;
    sd->data[2] = 0x80;     /* Supported group 6 functions */
    sd->data[3] = 0x01;
    sd->data[4] = 0x80;     /* Supported group 5 functions */
    sd->data[5] = 0x01;
    sd->data[6] = 0x80;     /* Supported group 4 functions */
    sd->data[7] = 0x01;
    sd->data[8] = 0x80;     /* Supported group 3 functions */
    sd->data[9] = 0x01;
    sd->data[10] = 0x80;    /* Supported group 2 functions */
    sd->data[11] = 0x43;
    sd->data[12] = 0x80;    /* Supported group 1 functions */
    sd->data[13] = 0x03;

    memset(&sd->data[14], 0, 3);
    for (i = 0; i < 6; i ++) {
        new_func = (arg >> (i * 4)) & 0x0f;
        if (mode && new_func != 0x0f)
            sd->function_group[i] = new_func;
        sd->data[16 - (i >> 1)] |= new_func << ((i % 2) * 4);
    }
    memset(&sd->data[17], 0, 47);
}

static inline bool sd_wp_addr(SDState *sd, uint64_t addr)
{
    return test_bit(sd_addr_to_wpnum(addr), sd->wp_group_bmap);
}

static void sd_lock_command(SDState *sd)
{
    int erase, lock, clr_pwd, set_pwd, pwd_len;
    erase = !!(sd->data[0] & 0x08);
    lock = sd->data[0] & 0x04;
    clr_pwd = sd->data[0] & 0x02;
    set_pwd = sd->data[0] & 0x01;

    if (sd->blk_len > 1)
        pwd_len = sd->data[1];
    else
        pwd_len = 0;

    if (lock) {
        trace_sdcard_lock();
    } else {
        trace_sdcard_unlock();
    }
    if (erase) {
        if (!(sd->card_status & CARD_IS_LOCKED) || sd->blk_len > 1 ||
                        set_pwd || clr_pwd || lock || sd->wp_switch ||
                        (sd->csd[14] & 0x20)) {
            sd->card_status |= LOCK_UNLOCK_FAILED;
            return;
        }
        bitmap_zero(sd->wp_group_bmap, sd->wp_group_bits);
        sd->csd[14] &= ~0x10;
        sd->card_status &= ~CARD_IS_LOCKED;
        sd->pwd_len = 0;
        /* Erasing the entire card here! */
        fprintf(stderr, "SD: Card force-erased by CMD42\n");
        return;
    }

    if (sd->blk_len < 2 + pwd_len ||
                    pwd_len <= sd->pwd_len ||
                    pwd_len > sd->pwd_len + 16) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    if (sd->pwd_len && memcmp(sd->pwd, sd->data + 2, sd->pwd_len)) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    pwd_len -= sd->pwd_len;
    if ((pwd_len && !set_pwd) ||
                    (clr_pwd && (set_pwd || lock)) ||
                    (lock && !sd->pwd_len && !set_pwd) ||
                    (!set_pwd && !clr_pwd &&
                     (((sd->card_status & CARD_IS_LOCKED) && lock) ||
                      (!(sd->card_status & CARD_IS_LOCKED) && !lock)))) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    if (set_pwd) {
        memcpy(sd->pwd, sd->data + 2 + sd->pwd_len, pwd_len);
        sd->pwd_len = pwd_len;
    }

    if (clr_pwd) {
        sd->pwd_len = 0;
    }

    if (lock)
        sd->card_status |= CARD_IS_LOCKED;
    else
        sd->card_status &= ~CARD_IS_LOCKED;
}

static bool address_in_range(SDState *sd, const char *desc,
                             uint64_t addr, uint32_t length)
{
    if (addr + length > sd->size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s offset %"PRIu64" > card %"PRIu64" [%%%u]\n",
                      desc, addr, sd->size, length);
        sd->card_status |= ADDRESS_ERROR;
        return false;
    }
    return true;
}

static sd_rsp_type_t sd_invalid_state_for_cmd(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: CMD%i in a wrong state: %s (spec %s)\n",
                  sd->proto->name, req.cmd, sd_state_name(sd->state),
                  sd_version_str(sd->spec_version));

    return sd_illegal;
}

static sd_rsp_type_t sd_invalid_mode_for_cmd(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: CMD%i in a wrong mode: %s (spec %s)\n",
                  sd->proto->name, req.cmd, sd_mode_name(sd_mode(sd)),
                  sd_version_str(sd->spec_version));

    return sd_illegal;
}

static sd_rsp_type_t sd_cmd_illegal(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Unknown CMD%i for spec %s\n",
                  sd->proto->name, req.cmd,
                  sd_version_str(sd->spec_version));

    return sd_illegal;
}

/* Commands that are recognised but not yet implemented. */
static sd_rsp_type_t sd_cmd_unimplemented(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_UNIMP, "%s: CMD%i not implemented\n",
                  sd->proto->name, req.cmd);

    return sd_illegal;
}

static sd_rsp_type_t sd_cmd_optional(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_UNIMP, "%s: Optional CMD%i not implemented\n",
                  sd->proto->name, req.cmd);

    return sd_illegal;
}

/* Configure fields for following sd_generic_write_data() calls */
static sd_rsp_type_t sd_cmd_to_receivingdata(SDState *sd, SDRequest req,
                                             uint64_t start, size_t size)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->state = sd_receivingdata_state;
    sd->data_start = start;
    sd->data_offset = 0;
    /* sd->data[] used as receive buffer */
    sd->data_size = size ?: sizeof(sd->data);
    return sd_r1;
}

/* Configure fields for following sd_generic_read_data() calls */
static sd_rsp_type_t sd_cmd_to_sendingdata(SDState *sd, SDRequest req,
                                           uint64_t start,
                                           const void *data, size_t size)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    sd->state = sd_sendingdata_state;
    sd->data_start = start;
    sd->data_offset = 0;
    if (data) {
        assert(size > 0 && size <= sizeof(sd->data));
        memcpy(sd->data, data, size);
    }
    if (size) {
        sd->data_size = size;
    }
    return sd_r1;
}

/* CMD0 */
static sd_rsp_type_t sd_cmd_GO_IDLE_STATE(SDState *sd, SDRequest req)
{
    if (sd->state == sd_sleep_state) {
        switch (req.arg) {
        case 0x00000000:
        case 0xf0f0f0f0:
            break;
        default:
            return sd_r0;
        }
    }
    if (sd->state != sd_inactive_state) {
        sd->state = sd_idle_state;
        sd_reset(DEVICE(sd));
    }

    return sd_is_spi(sd) ? sd_r1 : sd_r0;
}

/* CMD2 */
static sd_rsp_type_t sd_cmd_ALL_SEND_CID(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_ready_state:
        sd->state = sd_identification_state;
        return sd_r2_i;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD3 */
static sd_rsp_type_t sd_cmd_SEND_RELATIVE_ADDR(SDState *sd, SDRequest req)
{
    uint16_t random_rca;

    switch (sd->state) {
    case sd_identification_state:
    case sd_standby_state:
        sd->state = sd_standby_state;
        qemu_guest_getrandom_nofail(&random_rca, sizeof(random_rca));
        sd_set_rca(sd, random_rca);
        return sd_r6;

    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

static sd_rsp_type_t emmc_cmd_SET_RELATIVE_ADDR(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_identification_state:
    case sd_standby_state:
        sd->state = sd_standby_state;
        sd_set_rca(sd, req.arg >> 16);
        return sd_r1;

    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD5 */
static sd_rsp_type_t emmc_cmd_sleep_awake(SDState *sd, SDRequest req)
{
    bool do_sleep = extract32(req.arg, 15, 1);

    switch (sd->state) {
    case sd_sleep_state:
        if (!do_sleep) {
            /* Awake */
            sd->state = sd_standby_state;
        }
        return sd_r1b;

    case sd_standby_state:
        if (do_sleep) {
            sd->state = sd_sleep_state;
        }
        return sd_r1b;

    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD6 */
static sd_rsp_type_t sd_cmd_SWITCH_FUNCTION(SDState *sd, SDRequest req)
{
    if (sd_mode(sd) != sd_data_transfer_mode) {
        return sd_invalid_mode_for_cmd(sd, req);
    }
    if (sd_is_spi(sd)) {
        if (sd->state == sd_idle_state) {
            return sd_invalid_state_for_cmd(sd, req);
        }
    } else {
        if (sd->state != sd_transfer_state) {
            return sd_invalid_state_for_cmd(sd, req);
        }
    }

    sd_function_switch(sd, req.arg);
    return sd_cmd_to_sendingdata(sd, req, 0, NULL, 64);
}

static sd_rsp_type_t emmc_cmd_SWITCH(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_transfer_state:
        sd->state = sd_programming_state;
        emmc_function_switch(sd, req.arg);
        if (!sd->cache_flush_active) {
            sd->state = sd_transfer_state;
        }
        return sd_r1b;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD7 */
static sd_rsp_type_t sd_cmd_DE_SELECT_CARD(SDState *sd, SDRequest req)
{
    bool same_rca = sd_req_rca_same(sd, req);

    switch (sd->state) {
    case sd_standby_state:
        if (!same_rca) {
            return sd_r0;
        }
        sd->state = sd_transfer_state;
        return sd_r1b;

    case sd_transfer_state:
    case sd_sendingdata_state:
        if (same_rca) {
            break;
        }
        sd->state = sd_standby_state;
        return sd_r1b;

    case sd_disconnect_state:
        if (!same_rca) {
            return sd_r0;
        }
        sd->state = sd_programming_state;
        return sd_r1b;

    case sd_programming_state:
        if (same_rca) {
            break;
        }
        sd->state = sd_disconnect_state;
        return sd_r1b;

    default:
        break;
    }
    return sd_invalid_state_for_cmd(sd, req);
}

/* CMD8 */
static sd_rsp_type_t sd_cmd_SEND_IF_COND(SDState *sd, SDRequest req)
{
    if (sd->state != sd_idle_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->vhs = 0;

    /* No response if not exactly one VHS bit is set.  */
    if (!(req.arg >> 8) || (req.arg >> (ctz32(req.arg & ~0xff) + 1))) {
        return sd_is_spi(sd) ? sd_r7 : sd_r0;
    }

    /* Accept.  */
    sd->vhs = req.arg;
    return sd_r7;
}

/* CMD8 */
static sd_rsp_type_t emmc_cmd_SEND_EXT_CSD(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    return sd_cmd_to_sendingdata(sd, req, sd_req_get_address(sd, req),
                                 sd->ext_csd, sizeof(sd->ext_csd));
}

static sd_rsp_type_t spi_cmd_SEND_CxD(SDState *sd, SDRequest req,
                                      const void *data, size_t size)
{
    /*
     * XXX as of v10.1.0-rc1 command is reached in sd_idle_state,
     * so disable this check.
    if (sd->state != sd_standby_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    */

    /*
     * Since SPI returns CSD and CID on the DAT lines,
     * switch to sd_transfer_state.
     */
    sd->state = sd_transfer_state;

    return sd_cmd_to_sendingdata(sd, req, 0, data, size);
}

/* CMD9 */
static sd_rsp_type_t spi_cmd_SEND_CSD(SDState *sd, SDRequest req)
{
    return spi_cmd_SEND_CxD(sd, req, sd->csd, sizeof(sd->csd));
}

static sd_rsp_type_t sd_cmd_SEND_CSD(SDState *sd, SDRequest req)
{
    if (sd->state != sd_standby_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    return sd_req_rca_same(sd, req) ? sd_r2_s : sd_r0;
}

/* CMD10 */
static sd_rsp_type_t spi_cmd_SEND_CID(SDState *sd, SDRequest req)
{
    return spi_cmd_SEND_CxD(sd, req, sd->cid, sizeof(sd->cid));
}

static sd_rsp_type_t sd_cmd_SEND_CID(SDState *sd, SDRequest req)
{
    if (sd->state != sd_standby_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    return sd_req_rca_same(sd, req) ? sd_r2_i : sd_r0;
}

/* CMD12 */
static sd_rsp_type_t sd_cmd_STOP_TRANSMISSION(SDState *sd, SDRequest req)
{
    sd->multi_blk_cnt = 0;
    sd->reliable_write = false;

    switch (sd->state) {
    case sd_sendingdata_state:
        sd->state = sd_transfer_state;
        return sd_r1b;
    case sd_receivingdata_state:
        sd->state = sd_programming_state;
        /* Bzzzzzzztt .... Operation complete.  */
        if (!sd->program_count) {
            sd->state = sd_transfer_state;
        }
        return sd_r1;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD13 */
static sd_rsp_type_t sd_cmd_SEND_STATUS(SDState *sd, SDRequest req)
{
    if (sd_mode(sd) != sd_data_transfer_mode) {
        return sd_invalid_mode_for_cmd(sd, req);
    }

    switch (sd->state) {
    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        break;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }

    if (sd_is_spi(sd)) {
        return spi_r2;
    }

    return sd_req_rca_same(sd, req) ? sd_r1 : sd_r0;
}

/* CMD15 */
static sd_rsp_type_t sd_cmd_GO_INACTIVE_STATE(SDState *sd, SDRequest req)
{
    if (sd_mode(sd) != sd_data_transfer_mode) {
        return sd_invalid_mode_for_cmd(sd, req);
    }
    switch (sd->state) {
    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        break;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
    if (sd_req_rca_same(sd, req)) {
        sd->state = sd_inactive_state;
    }

    return sd_r0;
}

/* CMD16 */
static sd_rsp_type_t sd_cmd_SET_BLOCKLEN(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    if (req.arg > (1 << HWBLOCK_SHIFT)) {
        sd->card_status |= BLOCK_LEN_ERROR;
    } else {
        trace_sdcard_set_blocklen(req.arg);
        sd->blk_len = req.arg;
    }

    return sd_r1;
}

/* CMD17 */
static sd_rsp_type_t sd_cmd_READ_SINGLE_BLOCK(SDState *sd, SDRequest req)
{
    uint64_t addr;

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, "READ_SINGLE_BLOCK", addr, sd->blk_len)) {
        return sd_r1;
    }

    sd_blk_read(sd, addr, sd->blk_len);
    return sd_cmd_to_sendingdata(sd, req, addr, NULL, sd->blk_len);
}

/* CMD19 */
static sd_rsp_type_t sd_cmd_SEND_TUNING_BLOCK(SDState *sd, SDRequest req)
{
    if (sd->spec_version < SD_PHY_SPECv3_01_VERS) {
        return sd_cmd_illegal(sd, req);
    }

    return sd_cmd_to_sendingdata(sd, req, 0,
                                 sd_tuning_block_pattern4,
                                 sizeof(sd_tuning_block_pattern4));
}

/* CMD23 */
static sd_rsp_type_t sd_cmd_SET_BLOCK_COUNT(SDState *sd, SDRequest req)
{
    if (sd->spec_version < SD_PHY_SPECv3_01_VERS) {
        return sd_cmd_illegal(sd, req);
    }

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    sd->multi_blk_cnt = req.arg;
    sd->reliable_write = false;
    if (sd_is_emmc(sd)) {
        sd->multi_blk_cnt &= 0xffff;
        sd->reliable_write = sd->multi_blk_cnt && (req.arg & BIT(31));
    }
    trace_sdcard_set_block_count(sd->multi_blk_cnt);

    return sd_r1;
}

/* CMD24 */
static sd_rsp_type_t sd_cmd_WRITE_SINGLE_BLOCK(SDState *sd, SDRequest req)
{
    uint64_t addr;

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, "WRITE_SINGLE_BLOCK", addr, sd->blk_len)) {
        return sd_r1;
    }

    if (sd->size <= SDSC_MAX_CAPACITY) {
        if (sd_wp_addr(sd, addr)) {
            sd->card_status |= WP_VIOLATION;
        }
    }
    if (sd->csd[14] & 0x30) {
        sd->card_status |= WP_VIOLATION;
    }

    sd->blk_written = 0;
    return sd_cmd_to_receivingdata(sd, req, addr, sd->blk_len);
}

/* CMD26 */
static sd_rsp_type_t emmc_cmd_PROGRAM_CID(SDState *sd, SDRequest req)
{
    return sd_cmd_to_receivingdata(sd, req, 0, sizeof(sd->cid));
}

/* CMD27 */
static sd_rsp_type_t sd_cmd_PROGRAM_CSD(SDState *sd, SDRequest req)
{
    return sd_cmd_to_receivingdata(sd, req, 0, sizeof(sd->csd));
}

static sd_rsp_type_t sd_cmd_SET_CLR_WRITE_PROT(SDState *sd, SDRequest req,
                                               bool is_write)
{
    uint64_t addr;

    if (sd->size > SDSC_MAX_CAPACITY) {
        return sd_illegal;
    }

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, is_write ? "SET_WRITE_PROT" : "CLR_WRITE_PROT",
                          addr, 1)) {
        return sd_r1b;
    }

    sd->state = sd_programming_state;
    if (is_write) {
        set_bit(sd_addr_to_wpnum(addr), sd->wp_group_bmap);
    } else {
        clear_bit(sd_addr_to_wpnum(addr), sd->wp_group_bmap);
    }
    /* Bzzzzzzztt .... Operation complete.  */
    sd->state = sd_transfer_state;
    return sd_r1;
}

/* CMD28 */
static sd_rsp_type_t sd_cmd_SET_WRITE_PROT(SDState *sd, SDRequest req)
{
    return sd_cmd_SET_CLR_WRITE_PROT(sd, req, true);
}

/* CMD29 */
static sd_rsp_type_t sd_cmd_CLR_WRITE_PROT(SDState *sd, SDRequest req)
{
    return sd_cmd_SET_CLR_WRITE_PROT(sd, req, false);
}

/* CMD30 */
static sd_rsp_type_t sd_cmd_SEND_WRITE_PROT(SDState *sd, SDRequest req)
{
    uint64_t addr;
    uint32_t data;

    if (sd->size > SDSC_MAX_CAPACITY) {
        return sd_illegal;
    }

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, "SEND_WRITE_PROT", addr, sd->blk_len)) {
        return sd_r1;
    }

    data = sd_wpbits(sd, req.arg);
    return sd_cmd_to_sendingdata(sd, req, addr, &data, sizeof(data));
}

/* CMD32 */
static sd_rsp_type_t sd_cmd_ERASE_WR_BLK_START(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->erase_start = req.arg;
    return sd_r1;
}

/* CMD33 */
static sd_rsp_type_t sd_cmd_ERASE_WR_BLK_END(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->erase_end = req.arg;
    return sd_r1;
}

/* CMD38 */
static sd_rsp_type_t sd_cmd_ERASE(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    if (sd->csd[14] & 0x30) {
        sd->card_status |= WP_VIOLATION;
        return sd_r1b;
    }

    sd->state = sd_programming_state;
    if (sd_erase(sd)) {
        return sd_r1b;
    }
    /* Bzzzzzzztt .... Operation complete.  */
    sd->state = sd_transfer_state;
    return sd_r1b;
}

/* CMD42 */
static sd_rsp_type_t sd_cmd_LOCK_UNLOCK(SDState *sd, SDRequest req)
{
    return sd_cmd_to_receivingdata(sd, req, 0, 0);
}

/* CMD55 */
static sd_rsp_type_t sd_cmd_APP_CMD(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_ready_state:
    case sd_identification_state:
    case sd_inactive_state:
    case sd_sleep_state:
        return sd_invalid_state_for_cmd(sd, req);
    case sd_idle_state:
        if (!sd_is_spi(sd) && sd_req_get_rca(sd, req) != 0x0000) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "SD: illegal RCA 0x%04x for APP_CMD\n", req.cmd);
        }
        /* fall-through */
    default:
        break;
    }
    if (!sd_is_spi(sd) && !sd_req_rca_same(sd, req)) {
        return sd_r0;
    }
    sd->expecting_acmd = true;
    sd->card_status |= APP_CMD;

    return sd_r1;
}

/* CMD56 */
static sd_rsp_type_t sd_cmd_GEN_CMD(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    /* Vendor specific command: our model is RAZ/WI */
    if (req.arg & 1) {
        memset(sd->data, 0, sizeof(sd->data));
        return sd_cmd_to_sendingdata(sd, req, 0, NULL, 0);
    } else {
        return sd_cmd_to_receivingdata(sd, req, 0, 0);
    }
}

/* CMD58 */
static sd_rsp_type_t spi_cmd_READ_OCR(SDState *sd, SDRequest req)
{
    return sd_r3;
}

/* CMD59 */
static sd_rsp_type_t spi_cmd_CRC_ON_OFF(SDState *sd, SDRequest req)
{
    return sd_r1;
}

/* ACMD6 */
static sd_rsp_type_t sd_acmd_SET_BUS_WIDTH(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    sd->sd_status[0] &= 0x3f;
    sd->sd_status[0] |= (req.arg & 0x03) << 6;
    return sd_r1;
}

/* ACMD13 */
static sd_rsp_type_t sd_acmd_SD_STATUS(SDState *sd, SDRequest req)
{
    sd_rsp_type_t rsp;

    rsp = sd_cmd_to_sendingdata(sd, req, 0,
                                sd->sd_status, sizeof(sd->sd_status));
    if (sd_is_spi(sd) && rsp != sd_illegal) {
        return spi_r2;
    }
    return rsp;
}

/* ACMD22 */
static sd_rsp_type_t sd_acmd_SEND_NUM_WR_BLOCKS(SDState *sd, SDRequest req)
{
    return sd_cmd_to_sendingdata(sd, req, 0,
                                 &sd->blk_written, sizeof(sd->blk_written));
}

/* ACMD23 */
static sd_rsp_type_t sd_acmd_SET_WR_BLK_ERASE_COUNT(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    return sd_r1;
}

/* ACMD41 */
static sd_rsp_type_t sd_cmd_SEND_OP_COND(SDState *sd, SDRequest req)
{
    if (sd->state != sd_idle_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    /*
     * If it's the first ACMD41 since reset, we need to decide
     * whether to power up. If this is not an enquiry ACMD41,
     * we immediately report power on and proceed below to the
     * ready state, but if it is, we set a timer to model a
     * delay for power up. This works around a bug in EDK2
     * UEFI, which sends an initial enquiry ACMD41, but
     * assumes that the card is in ready state as soon as it
     * sees the power up bit set.
     */
    if (!FIELD_EX32(sd->ocr, OCR, CARD_POWER_UP)) {
        if ((req.arg & ACMD41_ENQUIRY_MASK) != 0) {
            timer_del(sd->ocr_power_timer);
            sd_ocr_powerup(sd);
        } else {
            trace_sdcard_inquiry_cmd41();
            if (!timer_pending(sd->ocr_power_timer)) {
                timer_mod_ns(sd->ocr_power_timer,
                             (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                              + OCR_POWER_DELAY_NS));
            }
        }
    }

    if (sd_is_spi(sd)) {
        sd->state = sd_ready_state;
        return sd_r1;
    } else {
        if (FIELD_EX32(sd->ocr & req.arg, OCR, VDD_VOLTAGE_WINDOW)) {
            /*
             * We accept any voltage.  10000 V is nothing.
             *
             * Once we're powered up, we advance straight to ready state
             * unless it's an enquiry ACMD41 (bits 23:0 == 0).
             */
            sd->state = sd_ready_state;
        }
        return sd_r3;
    }
}

/* ACMD42 */
static sd_rsp_type_t sd_acmd_SET_CLR_CARD_DETECT(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    /* Bringing in the 50KOhm pull-up resistor... Done.  */
    return sd_r1;
}

/* ACMD51 */
static sd_rsp_type_t sd_acmd_SEND_SCR(SDState *sd, SDRequest req)
{
    return sd_cmd_to_sendingdata(sd, req, 0, sd->scr, sizeof(sd->scr));
}

static sd_rsp_type_t sd_normal_command(SDState *sd, SDRequest req)
{
    uint64_t addr;

    sd->last_cmd_name = sd_cmd_name(sd, req.cmd);
    /* CMD55 precedes an ACMD, so we are not interested in tracing it.
     * However there is no ACMD55, so we want to trace this particular case.
     */
    if (req.cmd != 55 || sd->expecting_acmd) {
        trace_sdcard_normal_command(sd->proto->name,
                                    sd->last_cmd_name, req.cmd,
                                    req.arg,
                                    sd_mode_name(sd_mode(sd)),
                                    sd_state_name(sd->state));
    }

    /* Not interpreting this as an app command */
    sd->card_status &= ~APP_CMD;

    /* CMD23 (set block count) must be immediately followed by CMD18 or CMD25
     * if not, its effects are cancelled */
    if (sd->multi_blk_cnt != 0 && !(req.cmd == 18 || req.cmd == 25)) {
        sd->multi_blk_cnt = 0;
        sd->reliable_write = false;
    }

    if (sd->proto->cmd[req.cmd].class == 6 && FIELD_EX32(sd->ocr, OCR,
                                                         CARD_CAPACITY)) {
        /* Only Standard Capacity cards support class 6 commands */
        return sd_illegal;
    }

    if (sd->proto->cmd[req.cmd].handler) {
        return sd->proto->cmd[req.cmd].handler(sd, req);
    }

    switch (req.cmd) {
    /* Block read commands (Class 2) */
    case 18:  /* CMD18:  READ_MULTIPLE_BLOCK */
        addr = sd_req_get_address(sd, req);
        switch (sd->state) {
        case sd_transfer_state:

            if (!address_in_range(sd, "READ_BLOCK", addr, sd->blk_len)) {
                return sd_r1;
            }

            sd->state = sd_sendingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    /* Block write commands (Class 4) */
    case 25:  /* CMD25:  WRITE_MULTIPLE_BLOCK */
        addr = sd_req_get_address(sd, req);
        switch (sd->state) {
        case sd_transfer_state:

            if (!address_in_range(sd, "WRITE_BLOCK", addr, sd->blk_len)) {
                return sd_r1;
            }

            sd->state = sd_receivingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            sd->blk_written = 0;

            if (sd->size <= SDSC_MAX_CAPACITY) {
                if (sd_wp_addr(sd, sd->data_start)) {
                    sd->card_status |= WP_VIOLATION;
                }
            }
            if (sd->csd[14] & 0x30) {
                sd->card_status |= WP_VIOLATION;
            }
            return sd_r1;

        default:
            break;
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "SD: Unknown CMD%i\n", req.cmd);
        return sd_illegal;
    }

    return sd_invalid_state_for_cmd(sd, req);
}

static sd_rsp_type_t sd_app_command(SDState *sd,
                                    SDRequest req)
{
    sd->last_cmd_name = sd_acmd_name(sd, req.cmd);
    trace_sdcard_app_command(sd->proto->name, sd->last_cmd_name,
                             req.cmd, req.arg,
                             sd_mode_name(sd_mode(sd)),
                             sd_state_name(sd->state));
    sd->card_status |= APP_CMD;

    if (sd->proto->acmd[req.cmd].handler) {
        return sd->proto->acmd[req.cmd].handler(sd, req);
    }

    switch (req.cmd) {
    case 18:    /* Reserved for SD security applications */
    case 25:
    case 26:
    case 38:
    case 43 ... 49:
        /* Refer to the "SD Specifications Part3 Security Specification" for
         * information about the SD Security Features.
         */
        qemu_log_mask(LOG_UNIMP, "SD: CMD%i Security not implemented\n",
                      req.cmd);
        return sd_illegal;

    default:
        /* Fall back to standard commands.  */
        return sd_normal_command(sd, req);
    }

    qemu_log_mask(LOG_GUEST_ERROR, "SD: ACMD%i in a wrong state\n", req.cmd);
    return sd_illegal;
}

static bool cmd_valid_while_locked(SDState *sd, unsigned cmd)
{
    unsigned cmd_class;

    /* Valid commands in locked state:
     * basic class (0)
     * lock card class (7)
     * CMD16
     * implicitly, the ACMD prefix CMD55
     * ACMD41 and ACMD42
     * Anything else provokes an "illegal command" response.
     */
    if (sd->expecting_acmd) {
        return cmd == 41 || cmd == 42;
    }
    if (cmd == 16 || cmd == 55) {
        return true;
    }
    if (!sd->proto->cmd[cmd].handler) {
        return false;
    }
    cmd_class = sd->proto->cmd[cmd].class;

    return cmd_class == 0 || cmd_class == 7;
}

static size_t sd_do_command(SDState *sd, SDRequest *req,
                            uint8_t *response, size_t respsz)
{
    int last_state;
    sd_rsp_type_t rtype;
    int rsplen;

    if (!sd->blk || !blk_is_inserted(sd->blk)) {
        return 0;
    }

    if (sd->state == sd_inactive_state) {
        rtype = sd_illegal;
        goto send_response;
    }

    if (sd_req_crc_validate(req)) {
        sd->card_status |= COM_CRC_ERROR;
        rtype = sd_illegal;
        goto send_response;
    }

    if (req->cmd >= SDMMC_CMD_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "SD: incorrect command 0x%02x\n",
                      req->cmd);
        req->cmd &= 0x3f;
    }

    if (sd->state == sd_sleep_state && req->cmd) {
        qemu_log_mask(LOG_GUEST_ERROR, "SD: Card is sleeping\n");
        rtype = sd_r0;
        goto send_response;
    }

    if (sd->card_status & CARD_IS_LOCKED) {
        if (!cmd_valid_while_locked(sd, req->cmd)) {
            sd->card_status |= ILLEGAL_COMMAND;
            sd->expecting_acmd = false;
            qemu_log_mask(LOG_GUEST_ERROR, "SD: Card is locked\n");
            rtype = sd_illegal;
            goto send_response;
        }
    }

    last_state = sd->state;

    if (sd->expecting_acmd) {
        sd->expecting_acmd = false;
        rtype = sd_app_command(sd, *req);
    } else {
        rtype = sd_normal_command(sd, *req);
    }

    if (rtype == sd_illegal) {
        sd->card_status |= ILLEGAL_COMMAND;
    } else {
        /* Valid command, we can update the 'state before command' bits.
         * (Do this now so they appear in r1 responses.)
         */
        sd->card_status = FIELD_DP32(sd->card_status, CSR,
                                     CURRENT_STATE, last_state);
    }

send_response:
    rsplen = sd_response_size(sd, rtype);
    assert(rsplen <= respsz);

    switch (rtype) {
    case sd_r1:
    case sd_r1b:
        sd_response_r1_make(sd, response);
        break;

    case spi_r2:
        spi_response_r2_make(sd, response);
        break;

    case sd_r2_i:
        memcpy(response, sd->cid, sizeof(sd->cid));
        break;

    case sd_r2_s:
        memcpy(response, sd->csd, sizeof(sd->csd));
        break;

    case sd_r3:
        sd_response_r3_make(sd, response);
        break;

    case sd_r6:
        sd_response_r6_make(sd, response);
        break;

    case sd_r7:
        sd_response_r7_make(sd, response);
        break;

    case sd_r0:
        /*
         * Invalid state transition, reset implementation
         * fields to avoid OOB abuse.
         */
        sd->data_start = 0;
        sd->data_offset = 0;
        /* fall-through */
    case sd_illegal:
        break;
    default:
        g_assert_not_reached();
    }
    trace_sdcard_response(sd_response_name(rtype), rsplen);

    if (rtype != sd_illegal) {
        /* Clear the "clear on valid command" status bits now we've
         * sent any response
         */
        sd->card_status &= ~CARD_STATUS_B;
    }

#ifdef DEBUG_SD
    qemu_hexdump(stderr, "Response", response, rsplen);
#endif

    sd->current_cmd = rtype == sd_illegal ? 0 : req->cmd;

    return rsplen;
}

/* Return true if buffer is consumed. Configured by sd_cmd_to_receivingdata() */
static bool sd_generic_write_data(SDState *sd, const void *buf, size_t *len)
{
    size_t to_write = MIN(sd->data_size - sd->data_offset, *len);

    memcpy(&sd->data[sd->data_offset], buf, to_write);
    sd->data_offset += to_write;
    *len = to_write;

    if (sd->data_offset >= sd->data_size) {
        sd->state = sd_transfer_state;
        return true;
    }
    return false;
}

/* Return true when buffer is consumed. Configured by sd_cmd_to_sendingdata() */
static bool sd_generic_read_data(SDState *sd, void *buf, size_t *len)
{
    size_t to_read = MIN(sd->data_size - sd->data_offset, *len);

    memcpy(buf, &sd->data[sd->data_offset], to_read);
    sd->data_offset += to_read;
    *len = to_read;

    if (sd->data_offset >= sd->data_size) {
        sd->state = sd_transfer_state;
        return true;
    }

    return false;
}

static void sdcard_write_data_dump(const char *proto, const char *cmd_desc,
                                   uint8_t cmd, uint32_t offset,
                                   const void *buf, size_t len)
{
    g_autoptr(GString) str = NULL;

    if (trace_event_get_state_backends(TRACE_SDCARD_WRITE_DATA)) {
        str = qemu_hexdump_line(NULL, buf, len, 8, 0);
        trace_sdcard_write_data(proto, cmd_desc, cmd, offset, str->str);
    }
}

static size_t sd_write_data(SDState *sd, const void *buf, size_t length)
{
    unsigned int partition_access;
    int i;
    const uint8_t *value = buf;

    if (!sd->blk || !blk_is_inserted(sd->blk)) {
        return length;
    }

    if (sd->state != sd_receivingdata_state) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: not in Receiving-Data state\n", __func__);
        return length;
    }

    if (sd->card_status & (ADDRESS_ERROR | WP_VIOLATION))
        return length;

    sdcard_write_data_dump(sd->proto->name,
                           sd->last_cmd_name,
                           sd->current_cmd, sd->data_offset, buf, length);
    switch (sd->current_cmd) {
    case 24:  /* CMD24:  WRITE_SINGLE_BLOCK */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            sd_blk_write(sd, sd->data_start, sd->data_offset);
            sd->blk_written ++;
            sd->csd[14] |= 0x40;
            /* Bzzzzzzztt .... Operation complete.  */
            if (!sd->program_count) {
                sd->state = sd_transfer_state;
            }
        }
        break;

    case 25:  /* CMD25:  WRITE_MULTIPLE_BLOCK */
        /*
         * Only read one byte at a time. We will be called again with the
         * remaining.
         */
        length = 1;

        if (sd->data_offset == 0) {
            /* Start of the block - let's check the address is valid */
            if (!address_in_range(sd, "WRITE_MULTIPLE_BLOCK",
                                  sd->data_start, sd->blk_len)) {
                break;
            }
            if (sd->size <= SDSC_MAX_CAPACITY) {
                if (sd_wp_addr(sd, sd->data_start)) {
                    sd->card_status |= WP_VIOLATION;
                    break;
                }
            }
        }
        sd->data[sd->data_offset++] = value[0];
        if (sd->data_offset >= sd->blk_len) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            partition_access = sd->ext_csd[EXT_CSD_PART_CONFIG]
                    & EXT_CSD_PART_CONFIG_ACC_MASK;
            if (partition_access == EXT_CSD_PART_CONFIG_ACC_RPMB) {
                emmc_rpmb_blk_write(sd, sd->data_start, sd->data_offset);
            } else {
                sd_blk_write(sd, sd->data_start, sd->data_offset);
            }
            sd->blk_written++;
            sd->data_start += sd->blk_len;
            sd->data_offset = 0;
            sd->csd[14] |= 0x40;

            /* Bzzzzzzztt .... Operation complete.  */
            if (sd->multi_blk_cnt != 0) {
                if (--sd->multi_blk_cnt == 0) {
                    /* Stop! */
                    sd->reliable_write = false;
                    if (!sd->program_count) {
                        sd->state = sd_transfer_state;
                    }
                    break;
                }
            }

            sd->state = sd_receivingdata_state;
        }
        break;

    case 26:  /* CMD26:  PROGRAM_CID */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            for (i = 0; i < sizeof(sd->cid); i ++)
                if ((sd->cid[i] | 0x00) != sd->data[i])
                    sd->card_status |= CID_CSD_OVERWRITE;

            if (!(sd->card_status & CID_CSD_OVERWRITE))
                for (i = 0; i < sizeof(sd->cid); i ++) {
                    sd->cid[i] |= 0x00;
                    sd->cid[i] &= sd->data[i];
                }
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 27:  /* CMD27:  PROGRAM_CSD */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            for (i = 0; i < sizeof(sd->csd); i ++)
                if ((sd->csd[i] | sd_csd_rw_mask[i]) !=
                    (sd->data[i] | sd_csd_rw_mask[i]))
                    sd->card_status |= CID_CSD_OVERWRITE;

            /* Copy flag (OTP) & Permanent write protect */
            if (sd->csd[14] & ~sd->data[14] & 0x60)
                sd->card_status |= CID_CSD_OVERWRITE;

            if (!(sd->card_status & CID_CSD_OVERWRITE))
                for (i = 0; i < sizeof(sd->csd); i ++) {
                    sd->csd[i] |= sd_csd_rw_mask[i];
                    sd->csd[i] &= sd->data[i];
                }
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 42:  /* CMD42:  LOCK_UNLOCK */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            sd_lock_command(sd);
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 56:  /* CMD56:  GEN_CMD */
        sd_generic_write_data(sd, buf, &length);
        break;

    default:
        g_assert_not_reached();
    }

    return length;
}

static size_t sd_read_data(SDState *sd, void *buf, size_t length)
{
    /* TODO: Append CRCs */
    const uint8_t dummy_byte = 0x00;
    unsigned int partition_access;
    uint32_t io_len;
    uint8_t *value = buf;

    if (!sd->blk || !blk_is_inserted(sd->blk)) {
        memset(buf, dummy_byte, length);
        return length;
    }

    if (sd->state != sd_sendingdata_state) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: not in Sending-Data state\n", __func__);
        memset(buf, dummy_byte, length);
        return length;
    }

    if (sd->card_status & (ADDRESS_ERROR | WP_VIOLATION)) {
        memset(buf, dummy_byte, length);
        return length;
    }

    io_len = sd_blk_len(sd);

    trace_sdcard_read_data(sd->proto->name,
                           sd->last_cmd_name, sd->current_cmd,
                           sd->data_offset, sd->data_size, io_len);
    switch (sd->current_cmd) {
    case 6:  /* CMD6:   SWITCH_FUNCTION */
    case 8:  /* CMD8:   SEND_EXT_CSD */
    case 9:  /* CMD9:   SEND_CSD */
    case 10: /* CMD10:  SEND_CID */
    case 13: /* ACMD13: SD_STATUS */
    case 17: /* CMD17:  READ_SINGLE_BLOCK */
    case 19: /* CMD19:  SEND_TUNING_BLOCK (SD) */
    case 22: /* ACMD22: SEND_NUM_WR_BLOCKS */
    case 30: /* CMD30:  SEND_WRITE_PROT */
    case 51: /* ACMD51: SEND_SCR */
    case 56: /* CMD56:  GEN_CMD */
        sd_generic_read_data(sd, buf, &length);
        break;

    case 18:  /* CMD18:  READ_MULTIPLE_BLOCK */
        /*
         * We will only read one byte at a time. We will be called again with
         * the remaining buffer.
         */
        length = 1;

        if (sd->data_offset == 0) {
            if (!address_in_range(sd, "READ_MULTIPLE_BLOCK",
                                  sd->data_start, io_len)) {
                *value = dummy_byte;
                return length;
            }
            partition_access = sd->ext_csd[EXT_CSD_PART_CONFIG]
                    & EXT_CSD_PART_CONFIG_ACC_MASK;
            if (partition_access == EXT_CSD_PART_CONFIG_ACC_RPMB) {
                emmc_rpmb_blk_read(sd, sd->data_start, io_len);
            } else {
                sd_blk_read(sd, sd->data_start, io_len);
            }
        }
        *value = sd->data[sd->data_offset++];

        if (sd->data_offset >= io_len) {
            sd->data_start += io_len;
            sd->data_offset = 0;

            if (sd->multi_blk_cnt != 0) {
                if (--sd->multi_blk_cnt == 0) {
                    /* Stop! */
                    sd->reliable_write = false;
                    sd->state = sd_transfer_state;
                    break;
                }
            }
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DAT read illegal for command %s\n",
                                       __func__, sd->last_cmd_name);
        memset(buf, dummy_byte, length);
    }

    return length;
}

static bool sd_receive_ready(SDState *sd)
{
    return sd->state == sd_receivingdata_state;
}

static bool sd_data_ready(SDState *sd)
{
    return sd->state == sd_sendingdata_state;
}

static const SDProto sd_proto_spi = {
    .name = "SPI",
    .cmd = {
        [0]  = {0,  sd_spi, "GO_IDLE_STATE", sd_cmd_GO_IDLE_STATE},
        [1]  = {0,  sd_spi, "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [5]  = {9,  sd_spi, "IO_SEND_OP_COND", sd_cmd_optional},
        [6]  = {10, sd_spi, "SWITCH_FUNCTION", sd_cmd_SWITCH_FUNCTION},
        [8]  = {0,  sd_spi, "SEND_IF_COND", sd_cmd_SEND_IF_COND},
        [9]  = {0,  sd_spi, "SEND_CSD", spi_cmd_SEND_CSD},
        [10] = {0,  sd_spi, "SEND_CID", spi_cmd_SEND_CID},
        [12] = {0,  sd_spi, "STOP_TRANSMISSION", sd_cmd_STOP_TRANSMISSION},
        [13] = {0,  sd_spi, "SEND_STATUS", sd_cmd_SEND_STATUS},
        [16] = {2,  sd_spi, "SET_BLOCKLEN", sd_cmd_SET_BLOCKLEN},
        [17] = {2,  sd_spi, "READ_SINGLE_BLOCK", sd_cmd_READ_SINGLE_BLOCK},
        [24] = {4,  sd_spi, "WRITE_SINGLE_BLOCK", sd_cmd_WRITE_SINGLE_BLOCK},
        [27] = {4,  sd_spi, "PROGRAM_CSD", sd_cmd_PROGRAM_CSD},
        [28] = {6,  sd_spi, "SET_WRITE_PROT", sd_cmd_SET_WRITE_PROT},
        [29] = {6,  sd_spi, "CLR_WRITE_PROT", sd_cmd_CLR_WRITE_PROT},
        [30] = {6,  sd_spi, "SEND_WRITE_PROT", sd_cmd_SEND_WRITE_PROT},
        [32] = {5,  sd_spi, "ERASE_WR_BLK_START", sd_cmd_ERASE_WR_BLK_START},
        [33] = {5,  sd_spi, "ERASE_WR_BLK_END", sd_cmd_ERASE_WR_BLK_END},
        [34] = {10, sd_spi, "READ_SEC_CMD", sd_cmd_optional},
        [35] = {10, sd_spi, "WRITE_SEC_CMD", sd_cmd_optional},
        [36] = {10, sd_spi, "SEND_PSI", sd_cmd_optional},
        [37] = {10, sd_spi, "CONTROL_ASSD_SYSTEM", sd_cmd_optional},
        [38] = {5,  sd_spi, "ERASE", sd_cmd_ERASE},
        [42] = {7,  sd_spi, "LOCK_UNLOCK", sd_cmd_LOCK_UNLOCK},
        [50] = {10, sd_spi, "DIRECT_SECURE_READ", sd_cmd_optional},
        [52] = {9,  sd_spi, "IO_RW_DIRECT", sd_cmd_optional},
        [53] = {9,  sd_spi, "IO_RW_EXTENDED", sd_cmd_optional},
        [55] = {8,  sd_spi, "APP_CMD", sd_cmd_APP_CMD},
        [56] = {8,  sd_spi, "GEN_CMD", sd_cmd_GEN_CMD},
        [57] = {10, sd_spi, "DIRECT_SECURE_WRITE", sd_cmd_optional},
        [58] = {0,  sd_spi, "READ_OCR", spi_cmd_READ_OCR},
        [59] = {0,  sd_spi, "CRC_ON_OFF", spi_cmd_CRC_ON_OFF},
    },
    .acmd = {
        [13] = {8,  sd_spi, "SD_STATUS", sd_acmd_SD_STATUS},
        [22] = {8,  sd_spi, "SEND_NUM_WR_BLOCKS", sd_acmd_SEND_NUM_WR_BLOCKS},
        [23] = {8,  sd_spi, "SET_WR_BLK_ERASE_COUNT", sd_acmd_SET_WR_BLK_ERASE_COUNT},
        [41] = {8,  sd_spi, "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [42] = {8,  sd_spi, "SET_CLR_CARD_DETECT", sd_acmd_SET_CLR_CARD_DETECT},
        [51] = {8,  sd_spi, "SEND_SCR", sd_acmd_SEND_SCR},
    },
};

static const SDProto sd_proto_sd = {
    .name = "SD",
    .cmd = {
        [0]  = {0,  sd_bc,   "GO_IDLE_STATE", sd_cmd_GO_IDLE_STATE},
        [2]  = {0,  sd_bcr,  "ALL_SEND_CID", sd_cmd_ALL_SEND_CID},
        [3]  = {0,  sd_bcr,  "SEND_RELATIVE_ADDR", sd_cmd_SEND_RELATIVE_ADDR},
        [4]  = {0,  sd_bc,   "SEND_DSR", sd_cmd_unimplemented},
        [5]  = {9,  sd_bc,   "IO_SEND_OP_COND", sd_cmd_optional},
        [6]  = {10, sd_adtc, "SWITCH_FUNCTION", sd_cmd_SWITCH_FUNCTION},
        [7]  = {0,  sd_ac,   "(DE)SELECT_CARD", sd_cmd_DE_SELECT_CARD},
        [8]  = {0,  sd_bcr,  "SEND_IF_COND", sd_cmd_SEND_IF_COND},
        [9]  = {0,  sd_ac,   "SEND_CSD", sd_cmd_SEND_CSD},
        [10] = {0,  sd_ac,   "SEND_CID", sd_cmd_SEND_CID},
        [11] = {0,  sd_ac,   "VOLTAGE_SWITCH", sd_cmd_optional},
        [12] = {0,  sd_ac,   "STOP_TRANSMISSION", sd_cmd_STOP_TRANSMISSION},
        [13] = {0,  sd_ac,   "SEND_STATUS", sd_cmd_SEND_STATUS},
        [15] = {0,  sd_ac,   "GO_INACTIVE_STATE", sd_cmd_GO_INACTIVE_STATE},
        [16] = {2,  sd_ac,   "SET_BLOCKLEN", sd_cmd_SET_BLOCKLEN},
        [17] = {2,  sd_adtc, "READ_SINGLE_BLOCK", sd_cmd_READ_SINGLE_BLOCK},
        [19] = {2,  sd_adtc, "SEND_TUNING_BLOCK", sd_cmd_SEND_TUNING_BLOCK},
        [20] = {2,  sd_ac,   "SPEED_CLASS_CONTROL", sd_cmd_optional},
        [23] = {2,  sd_ac,   "SET_BLOCK_COUNT", sd_cmd_SET_BLOCK_COUNT},
        [24] = {4,  sd_adtc, "WRITE_SINGLE_BLOCK", sd_cmd_WRITE_SINGLE_BLOCK},
        [27] = {4,  sd_adtc, "PROGRAM_CSD", sd_cmd_PROGRAM_CSD},
        [28] = {6,  sd_ac,   "SET_WRITE_PROT", sd_cmd_SET_WRITE_PROT},
        [29] = {6,  sd_ac,   "CLR_WRITE_PROT", sd_cmd_CLR_WRITE_PROT},
        [30] = {6,  sd_adtc, "SEND_WRITE_PROT", sd_cmd_SEND_WRITE_PROT},
        [32] = {5,  sd_ac,   "ERASE_WR_BLK_START", sd_cmd_ERASE_WR_BLK_START},
        [33] = {5,  sd_ac,   "ERASE_WR_BLK_END", sd_cmd_ERASE_WR_BLK_END},
        [34] = {10, sd_adtc, "READ_SEC_CMD", sd_cmd_optional},
        [35] = {10, sd_adtc, "WRITE_SEC_CMD", sd_cmd_optional},
        [36] = {10, sd_adtc, "SEND_PSI", sd_cmd_optional},
        [37] = {10, sd_ac,   "CONTROL_ASSD_SYSTEM", sd_cmd_optional},
        [38] = {5,  sd_ac,   "ERASE", sd_cmd_ERASE},
        [42] = {7,  sd_adtc, "LOCK_UNLOCK", sd_cmd_LOCK_UNLOCK},
        [43] = {1,  sd_ac,   "Q_MANAGEMENT", sd_cmd_optional},
        [44] = {1,  sd_ac,   "Q_TASK_INFO_A", sd_cmd_optional},
        [45] = {1,  sd_ac,   "Q_TASK_INFO_B", sd_cmd_optional},
        [46] = {1,  sd_adtc, "Q_RD_TASK", sd_cmd_optional},
        [47] = {1,  sd_adtc, "Q_WR_TASK", sd_cmd_optional},
        [48] = {1,  sd_adtc, "READ_EXTR_SINGLE", sd_cmd_optional},
        [49] = {1,  sd_adtc, "WRITE_EXTR_SINGLE", sd_cmd_optional},
        [50] = {10, sd_adtc, "DIRECT_SECURE_READ", sd_cmd_optional},
        [52] = {9,  sd_bc,   "IO_RW_DIRECT", sd_cmd_optional},
        [53] = {9,  sd_bc,   "IO_RW_EXTENDED", sd_cmd_optional},
        [55] = {8,  sd_ac,   "APP_CMD", sd_cmd_APP_CMD},
        [56] = {8,  sd_adtc, "GEN_CMD", sd_cmd_GEN_CMD},
        [57] = {10, sd_adtc, "DIRECT_SECURE_WRITE", sd_cmd_optional},
        [58] = {11, sd_adtc, "READ_EXTR_MULTI", sd_cmd_optional},
        [59] = {11, sd_adtc, "WRITE_EXTR_MULTI", sd_cmd_optional},
    },
    .acmd = {
        [6]  = {8,  sd_ac,   "SET_BUS_WIDTH", sd_acmd_SET_BUS_WIDTH},
        [13] = {8,  sd_adtc, "SD_STATUS", sd_acmd_SD_STATUS},
        [22] = {8,  sd_adtc, "SEND_NUM_WR_BLOCKS", sd_acmd_SEND_NUM_WR_BLOCKS},
        [23] = {8,  sd_ac,   "SET_WR_BLK_ERASE_COUNT", sd_acmd_SET_WR_BLK_ERASE_COUNT},
        [41] = {8,  sd_bcr,  "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [42] = {8,  sd_ac,   "SET_CLR_CARD_DETECT", sd_acmd_SET_CLR_CARD_DETECT},
        [51] = {8,  sd_adtc, "SEND_SCR", sd_acmd_SEND_SCR},
    },
};

static const SDProto sd_proto_emmc = {
    /* Only v4.3 is supported */
    .name = "eMMC",
    .cmd = {
        [0]  = {0,  sd_bc,   "GO_IDLE_STATE", sd_cmd_GO_IDLE_STATE},
        [1]  = {0,  sd_bcr,  "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [2]  = {0,  sd_bcr,  "ALL_SEND_CID", sd_cmd_ALL_SEND_CID},
        [3]  = {0,  sd_ac,   "SET_RELATIVE_ADDR", emmc_cmd_SET_RELATIVE_ADDR},
        [4]  = {0,  sd_bc,   "SEND_DSR", sd_cmd_unimplemented},
        [5]  = {0,  sd_ac,   "SLEEP/AWAKE", emmc_cmd_sleep_awake},
        [6]  = {10, sd_adtc, "SWITCH", emmc_cmd_SWITCH},
        [7]  = {0,  sd_ac,   "(DE)SELECT_CARD", sd_cmd_DE_SELECT_CARD},
        [8]  = {0,  sd_adtc, "SEND_EXT_CSD", emmc_cmd_SEND_EXT_CSD},
        [9]  = {0,  sd_ac,   "SEND_CSD", sd_cmd_SEND_CSD},
        [10] = {0,  sd_ac,   "SEND_CID", sd_cmd_SEND_CID},
        [11] = {1,  sd_adtc, "READ_DAT_UNTIL_STOP", sd_cmd_unimplemented},
        [12] = {0,  sd_ac,   "STOP_TRANSMISSION", sd_cmd_STOP_TRANSMISSION},
        [13] = {0,  sd_ac,   "SEND_STATUS", sd_cmd_SEND_STATUS},
        [14] = {0,  sd_adtc, "BUSTEST_R", sd_cmd_unimplemented},
        [15] = {0,  sd_ac,   "GO_INACTIVE_STATE", sd_cmd_GO_INACTIVE_STATE},
        [16] = {2,  sd_ac,   "SET_BLOCKLEN", sd_cmd_SET_BLOCKLEN},
        [17] = {2,  sd_adtc, "READ_SINGLE_BLOCK", sd_cmd_READ_SINGLE_BLOCK},
        [19] = {0,  sd_adtc, "BUSTEST_W", sd_cmd_unimplemented},
        [20] = {3,  sd_adtc, "WRITE_DAT_UNTIL_STOP", sd_cmd_unimplemented},
        [23] = {2,  sd_ac,   "SET_BLOCK_COUNT", sd_cmd_SET_BLOCK_COUNT},
        [24] = {4,  sd_adtc, "WRITE_SINGLE_BLOCK", sd_cmd_WRITE_SINGLE_BLOCK},
        [26] = {4,  sd_adtc, "PROGRAM_CID", emmc_cmd_PROGRAM_CID},
        [27] = {4,  sd_adtc, "PROGRAM_CSD", sd_cmd_PROGRAM_CSD},
        [28] = {6,  sd_ac,   "SET_WRITE_PROT", sd_cmd_SET_WRITE_PROT},
        [29] = {6,  sd_ac,   "CLR_WRITE_PROT", sd_cmd_CLR_WRITE_PROT},
        [30] = {6,  sd_adtc, "SEND_WRITE_PROT", sd_cmd_SEND_WRITE_PROT},
        [31] = {6,  sd_adtc, "SEND_WRITE_PROT_TYPE", sd_cmd_unimplemented},
        [35] = {5,  sd_ac,   "ERASE_WR_BLK_START", sd_cmd_ERASE_WR_BLK_START},
        [36] = {5,  sd_ac,   "ERASE_WR_BLK_END", sd_cmd_ERASE_WR_BLK_END},
        [38] = {5,  sd_ac,   "ERASE", sd_cmd_ERASE},
        [39] = {9,  sd_ac,   "FAST_IO", sd_cmd_unimplemented},
        [40] = {9,  sd_bcr,  "GO_IRQ_STATE", sd_cmd_unimplemented},
        [42] = {7,  sd_adtc, "LOCK_UNLOCK", sd_cmd_LOCK_UNLOCK},
        [49] = {0,  sd_adtc, "SET_TIME", sd_cmd_unimplemented},
        [55] = {8,  sd_ac,   "APP_CMD", sd_cmd_APP_CMD},
        [56] = {8,  sd_adtc, "GEN_CMD", sd_cmd_GEN_CMD},
    },
};

static void sd_instance_init(Object *obj)
{
    SDState *sd = SDMMC_COMMON(obj);
    SDCardClass *sc = SDMMC_COMMON_GET_CLASS(sd);

    sd->proto = sc->proto;
    sd->last_cmd_name = "UNSET";
    sd->ocr_power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sd_ocr_powerup, sd);
    sd->cache_flush_timer = timer_new_us(
        QEMU_CLOCK_VIRTUAL, sd_emmc_cache_flush_timer, sd);
    sd->program_timer = timer_new_us(
        QEMU_CLOCK_VIRTUAL, sd_emmc_program_timer, sd);
    sd->erase_timer = timer_new_us(
        QEMU_CLOCK_VIRTUAL, sd_emmc_erase_timer, sd);
}

static void sd_instance_finalize(Object *obj)
{
    SDState *sd = SDMMC_COMMON(obj);

    timer_free(sd->ocr_power_timer);
    timer_free(sd->cache_flush_timer);
    timer_free(sd->program_timer);
    timer_free(sd->erase_timer);
    g_clear_pointer(&sd->cache_index, g_hash_table_destroy);
    g_free(sd->cache_entries);
    g_free(sd->program_entries);
    g_free(sd->cyw_ram);
}

static void sd_blk_size_error(SDState *sd, int64_t blk_size,
                              int64_t blk_size_aligned, const char *rule,
                              Error **errp)
{
    const char *dev_type = sd_is_emmc(sd) ? "eMMC" : "SD card";
    char *blk_size_str;

    blk_size_str = size_to_str(blk_size);
    error_setg(errp, "Invalid %s size: %s", dev_type, blk_size_str);
    g_free(blk_size_str);

    blk_size_str = size_to_str(blk_size_aligned);
    error_append_hint(errp,
                      "%s size has to be %s, e.g. %s.\n"
                      "You can resize disk images with"
                      " 'qemu-img resize <imagefile> <new-size>'\n"
                      "(note that this will lose data if you make the"
                      " image smaller than it currently is).\n",
                      dev_type, rule, blk_size_str);
    g_free(blk_size_str);
}

static void sd_realize(DeviceState *dev, Error **errp)
{
    SDState *sd = SDMMC_COMMON(dev);
    int64_t blk_size = -ENOMEDIUM;
    int ret;

    switch (sd->spec_version) {
    case SD_PHY_SPECv2_00_VERS
     ... SD_PHY_SPECv3_01_VERS:
        break;
    default:
        error_setg(errp, "Invalid SD card Spec version: %u", sd->spec_version);
        return;
    }

    if (sd->blk) {
        if (!blk_supports_write_perm(sd->blk)) {
            error_setg(errp, "Cannot use read-only drive as SD card");
            return;
        }

        blk_size = blk_getlength(sd->blk);
    }
    if (blk_size >= 0) {
        if (sd_is_emmc(sd) && !sd_has_separate_partitions(sd)) {
            blk_size -= sd->boot_part_size * 2 + sd->rpmb_part_size;
        }
        if (blk_size > SDSC_MAX_CAPACITY) {
            if (sd_is_emmc(sd) &&
                !QEMU_IS_ALIGNED(blk_size, 1 << HWBLOCK_SHIFT)) {
                int64_t blk_size_aligned =
                    ((blk_size >> HWBLOCK_SHIFT) + 1) << HWBLOCK_SHIFT;
                sd_blk_size_error(sd, blk_size, blk_size_aligned,
                                  "multiples of 512", errp);
                return;
            } else if (!sd_is_emmc(sd) &&
                !QEMU_IS_ALIGNED(blk_size, 512 * KiB)) {
                int64_t blk_size_aligned = ((blk_size >> 19) + 1) << 19;
                sd_blk_size_error(sd, blk_size, blk_size_aligned,
                                  "multiples of 512K", errp);
                return;
            }
        } else if (blk_size > 0 && !is_power_of_2(blk_size)) {
            sd_blk_size_error(sd, blk_size, pow2ceil(blk_size), "a power of 2",
                              errp);
            return;
        } else if (blk_size < 0) {
            error_setg(errp, "eMMC image smaller than boot partitions");
            return;
        }
    }
    if (sd->blk) {
        ret = blk_set_perm(sd->blk,
                           BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                           BLK_PERM_ALL, errp);
        if (ret < 0) {
            return;
        }
        blk_set_dev_ops(sd->blk, sd_is_emmc(sd) ? &emmc_block_ops :
                                                   &sd_block_ops,
                        sd);
    }
    if (!QEMU_IS_ALIGNED(sd->boot_part_size, 128 * KiB) ||
        sd->boot_part_size > 255 * 128 * KiB) {
        g_autofree char *size_str = size_to_str(sd->boot_part_size);

        error_setg(errp, "Invalid boot partition size: %s", size_str);
        error_append_hint(errp,
                          "The boot partition size must be multiples of 128K"
                          "and not larger than 32640K.\n");
    }
    if (!QEMU_IS_ALIGNED(sd->rpmb_part_size, 128 * KiB) ||
        sd->rpmb_part_size > 128 * 128 * KiB) {
        char *size_str = size_to_str(sd->boot_part_size);

        error_setg(errp, "Invalid RPMB partition size: %s", size_str);
        g_free(size_str);
        error_append_hint(errp,
                          "The RPMB partition size must be multiples of 128K"
                          "and not larger than 16384K.\n");
    }
    if (sd_is_emmc(sd) && sd->preset_auth_key) {
        if (strlen(sd->preset_auth_key) != 64) {
            error_setg(errp,
                       "Authentication key must be 32 bytes long, "
                       "encoded hexadecimally");
            return;
        }

        char *pos = sd->preset_auth_key;
        unsigned int n;
        for (n = 0; n < RPMB_KEY_MAC_LEN; n++, pos += 2) {
            int chrs;
            if (sscanf(pos, "%02hhx%n", &sd->rpmb.key[n], &chrs) != 1 ||
                chrs != 2) {
                error_setg(errp,
                           "Authentication key contains invalid characters");
                return;
            }
        }
        sd->rpmb.key_set = 1;
    }
}

static void emmc_realize(DeviceState *dev, Error **errp)
{
    SDState *sd = SDMMC_COMMON(dev);
    Error *local_err = NULL;
    int64_t length;

    if (sd->cache_size &&
        (!QEMU_IS_ALIGNED(sd->cache_size, 512) ||
         sd->cache_size > 16 * MiB)) {
        error_setg(errp, "eMMC cache size must be a multiple of 512 bytes "
                   "and no larger than 16 MiB");
        return;
    }
    if (sd->cache_flush_sector_delay_us && !sd->cache_size) {
        error_setg(errp, "eMMC cache flush delay requires a nonzero "
                   "cache size");
        return;
    }
    if (sd->cache_flush_sector_delay_us > INT64_MAX / 2) {
        error_setg(errp, "eMMC cache flush sector delay is too large");
        return;
    }
    if (sd->program_sector_delay_us > INT64_MAX / 2) {
        error_setg(errp, "eMMC program sector delay is too large");
        return;
    }
    if (sd->erase_group_delay_us > INT64_MAX / 2) {
        error_setg(errp, "eMMC erase group delay is too large");
        return;
    }
    sd->cache_capacity = sd->cache_size / 512;
    if (sd->cache_capacity) {
        sd->cache_entries = g_new0(EMMCCacheEntry, sd->cache_capacity);
        sd->cache_index = g_hash_table_new(g_int64_hash, g_int64_equal);
    }

    if (sd->preset_cid) {
        const char *pos = sd->preset_cid;
        size_t cid_length = strlen(sd->preset_cid);
        uint8_t expected_crc;

        if (cid_length != 30 && cid_length != 32) {
            error_setg(errp, "eMMC CID must contain 15 or 16 bytes "
                       "encoded hexadecimally");
            return;
        }
        memset(sd->configured_cid, 0, sizeof(sd->configured_cid));
        for (unsigned int n = 0; n < cid_length / 2;
             n++, pos += 2) {
            int chrs;

            if (sscanf(pos, "%02hhx%n", &sd->configured_cid[n], &chrs) != 1
                || chrs != 2) {
                error_setg(errp, "eMMC CID contains invalid characters");
                return;
            }
        }
        expected_crc = (sd_crc7(sd->configured_cid, 15) << 1) | 1;
        if (cid_length == 30 || sd->configured_cid[15] == 0) {
            /* Linux sysfs normalizes the CRC/end byte to zero. */
            sd->configured_cid[15] = expected_crc;
        } else if (sd->configured_cid[15] != expected_crc) {
            error_setg(errp, "eMMC CID has an invalid CRC7 or end bit");
            return;
        }
    }

    if (sd_has_separate_partitions(sd)) {
        if (sd->boot_blk) {
            length = blk_getlength(sd->boot_blk);
            if (length <= 0 || !QEMU_IS_ALIGNED(length, 256 * KiB) ||
                length / 2 > 255 * 128 * KiB) {
                error_setg(errp, "eMMC boot partition backend size must be "
                           "twice a nonzero 128 KiB unit, with each "
                           "partition no larger than 32,640 KiB");
                return;
            }
            if (sd->boot_part_size &&
                length != 2 * sd->boot_part_size) {
                error_setg(errp, "eMMC boot partition backend size does not "
                           "match boot-partition-size");
                return;
            }
            sd->boot_part_size = length / 2;
        } else if (sd->boot_part_size) {
            error_setg(errp, "separate eMMC layout requires a boot partition "
                       "backend when boot-partition-size is nonzero");
            return;
        }

        if (sd->rpmb_blk) {
            length = blk_getlength(sd->rpmb_blk);
            if (length <= 0 || !QEMU_IS_ALIGNED(length, 128 * KiB) ||
                length > 128 * 128 * KiB) {
                error_setg(errp, "eMMC RPMB backend size must be a nonzero "
                           "128 KiB unit no larger than 16,384 KiB");
                return;
            }
            if (sd->rpmb_part_size && length != sd->rpmb_part_size) {
                error_setg(errp, "eMMC RPMB backend size does not match "
                           "rpmb-partition-size");
                return;
            }
            sd->rpmb_part_size = length;
        } else if (sd->rpmb_part_size) {
            error_setg(errp, "separate eMMC layout requires an RPMB backend "
                       "when rpmb-partition-size is nonzero");
            return;
        }
    }

    sd->spec_version = SD_PHY_SPECv3_01_VERS; /* Actually v4.5 */

    sd_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (sd->boot_blk) {
        if (!blk_supports_write_perm(sd->boot_blk)) {
            error_setg(errp, "cannot use read-only drive as eMMC boot "
                       "partition backend");
            return;
        }
        if (blk_set_perm(sd->boot_blk,
                         BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            error_prepend(errp, "cannot use eMMC boot partition backend: ");
            return;
        }
        blk_set_dev_ops(sd->boot_blk, &emmc_block_ops, sd);
    }
    if (sd->rpmb_blk) {
        if (!blk_supports_write_perm(sd->rpmb_blk)) {
            error_setg(errp, "cannot use read-only drive as eMMC RPMB "
                       "backend");
            return;
        }
        if (blk_set_perm(sd->rpmb_blk,
                         BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            error_prepend(errp, "cannot use eMMC RPMB backend: ");
            return;
        }
        blk_set_dev_ops(sd->rpmb_blk, &emmc_block_ops, sd);
    }
}

static const Property sdmmc_common_properties[] = {
    DEFINE_PROP_DRIVE("drive", SDState, blk),
};

static const Property sd_properties[] = {
    DEFINE_PROP_UINT8("spec_version", SDState,
                      spec_version, SD_PHY_SPECv3_01_VERS),
};

static const Property emmc_properties[] = {
    DEFINE_PROP_UINT64("boot-partition-size", SDState, boot_part_size, 0),
    DEFINE_PROP_UINT8("boot-config", SDState, boot_config, 0x0),
    DEFINE_PROP_UINT64("rpmb-partition-size", SDState, rpmb_part_size, 0),
    DEFINE_PROP_DRIVE("boot-partition-drive", SDState, boot_blk),
    DEFINE_PROP_DRIVE("rpmb-partition-drive", SDState, rpmb_blk),
    DEFINE_PROP_STRING("cid", SDState, preset_cid),
    DEFINE_PROP_STRING("auth-key", SDState, preset_auth_key),
    DEFINE_PROP_SIZE("cache-size", SDState, cache_size, 0),
    DEFINE_PROP_BOOL("cache-power-loss-on-reset", SDState,
                     cache_power_loss_on_reset, false),
    DEFINE_PROP_UINT64("cache-flush-sector-delay-us", SDState,
                       cache_flush_sector_delay_us, 0),
    DEFINE_PROP_UINT64("program-sector-delay-us", SDState,
                       program_sector_delay_us, 0),
    DEFINE_PROP_UINT64("erase-group-delay-us", SDState,
                       erase_group_delay_us, 0),
};

static void emmc_get_cache_dirty_sectors(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint32_t value = SDMMC_COMMON(obj)->cache_count;

    visit_type_uint32(v, name, &value, errp);
}

static bool emmc_get_cache_flush_active(Object *obj, Error **errp)
{
    return SDMMC_COMMON(obj)->cache_flush_active;
}

static void emmc_get_cache_flush_completed(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint64_t value = SDMMC_COMMON(obj)->cache_flush_completed_sectors;

    visit_type_uint64(v, name, &value, errp);
}

static void emmc_get_program_pending(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint32_t value = SDMMC_COMMON(obj)->program_count;

    visit_type_uint32(v, name, &value, errp);
}

static bool emmc_get_program_active(Object *obj, Error **errp)
{
    return SDMMC_COMMON(obj)->program_active;
}

static void emmc_get_program_completed(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint64_t value = SDMMC_COMMON(obj)->program_completed_sectors;

    visit_type_uint64(v, name, &value, errp);
}

static bool emmc_get_erase_active(Object *obj, Error **errp)
{
    return SDMMC_COMMON(obj)->erase_active;
}

static void emmc_get_erase_pending(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    SDState *sd = SDMMC_COMMON(obj);
    uint64_t value = 0;

    if (sd->erase_next != UINT64_MAX && sd->erase_next < sd->erase_last) {
        value = DIV_ROUND_UP(sd->erase_last - sd->erase_next,
                             EMMC_HC_ERASE_GROUP_BYTES);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void emmc_get_erase_completed(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint64_t value = SDMMC_COMMON(obj)->erase_completed_groups;

    visit_type_uint64(v, name, &value, errp);
}

#define CYW_SDIO_OCR                 0x00ff8000
#define CYW_SDIO_FUNCTIONS           2
#define CYW_SDIO_CCCR_IO_ENABLE      0x02
#define CYW_SDIO_CCCR_IO_READY       0x03
#define CYW_SDIO_CCCR_INT_ENABLE     0x04
#define CYW_SDIO_CCCR_INT_PENDING    0x05
#define CYW_SDIO_CCCR_IO_ABORT       0x06
#define CYW_SDIO_CCCR_BUS_IF         0x07
#define CYW_SDIO_CCCR_CAPS           0x08
#define CYW_SDIO_CCCR_SPEED          0x13
#define CYW_SDIO_CCCR_BRCM_CARDCAP   0xf0
#define CYW_SDIO_CCCR_BRCM_CARDCTRL  0xf1
#define CYW_SDIO_FUNC1_MISC_BASE     0x10000
#define CYW_SDIO_FUNC1_SBADDRLOW     0x0a
#define CYW_SDIO_FUNC1_SBADDRMID     0x0b
#define CYW_SDIO_FUNC1_SBADDRHIGH    0x0c
#define CYW_SDIO_FUNC1_CHIPCLKCSR    0x0e
#define CYW_SDIO_FUNC1_SLEEPCSR      0x1f
#define CYW_SDIO_SB_OFT_MASK         0x7fff
#define CYW_SDPCM_SHARED_SIZE        64
#define CYW_SDPCM_SHARED_VERSION     3
#define CYW_SDIO_CHIPCOMMON_BASE     0x18000000
#define CYW_SDIO_CHIP_ID             0x15294345
#define CYW_SDIO_EROM_BASE           0x18110000
#define CYW_SDIO_RAM_BASE            0x00198000
#define CYW_SDIO_DEFAULT_RAM_SIZE    0x000c8000
#define CYW_SDIO_CORE_CHIPCOMMON     0x800
#define CYW_SDIO_CORE_80211          0x812
#define CYW_SDIO_CORE_SDIO_DEV       0x829
#define CYW_SDIO_CORE_ARM_CR4        0x83e

#define CYW_DMP_COMPONENT(id)         (((id) << 8) | 0x1)
#define CYW_DMP_COMPONENT_INFO(rev)   (((rev) << 24) | BIT(19) | 0x1)
#define CYW_DMP_SLAVE_ADDRESS(base)   ((base) | 0x5)
#define CYW_DMP_SWRAP_ADDRESS(base)   ((base) | 0x85)

/*
 * The enumeration ROM is consumed by brcmf_chip_dmp_erom_scan().  Keep
 * ChipCommon first and describe one regular 4 KiB slave plus one 4 KiB
 * slave-wrapper region per core.
 */
static const uint32_t cyw_sdio_erom[] = {
    CYW_DMP_COMPONENT(CYW_SDIO_CORE_CHIPCOMMON),
    CYW_DMP_COMPONENT_INFO(54),
    CYW_DMP_SLAVE_ADDRESS(0x18000000),
    CYW_DMP_SWRAP_ADDRESS(0x18100000),

    CYW_DMP_COMPONENT(CYW_SDIO_CORE_SDIO_DEV),
    CYW_DMP_COMPONENT_INFO(12),
    CYW_DMP_SLAVE_ADDRESS(0x18002000),
    CYW_DMP_SWRAP_ADDRESS(0x18102000),

    CYW_DMP_COMPONENT(CYW_SDIO_CORE_80211),
    CYW_DMP_COMPONENT_INFO(65),
    CYW_DMP_SLAVE_ADDRESS(0x18001000),
    CYW_DMP_SWRAP_ADDRESS(0x18101000),

    CYW_DMP_COMPONENT(CYW_SDIO_CORE_ARM_CR4),
    CYW_DMP_COMPONENT_INFO(1),
    CYW_DMP_SLAVE_ADDRESS(0x18003000),
    CYW_DMP_SWRAP_ADDRESS(0x18103000),

    0xf,
};

static const uint32_t cyw_sdio_core_base[] = {
    0x18000000, 0x18002000, 0x18001000, 0x18003000,
};

static const uint32_t cyw_sdio_core_wrap[] = {
    0x18100000, 0x18102000, 0x18101000, 0x18103000,
};

#define CYW_BCMA_IOCTL                 0x408
#define CYW_BCMA_IOCTL_CLK             BIT(0)
#define CYW_BCMA_RESET_CTL             0x800
#define CYW_BCMA_RESET_ST              0x804
#define CYW_ARMCR4_CAP                 0x04
#define CYW_ARMCR4_BANKIDX             0x40
#define CYW_ARMCR4_BANKINFO            0x44
#define CYW_ARMCR4_BANK_COUNT          1
#define CYW_ARMCR4_BANKINFO_800K       0x63
#define CYW_ARMCR4_CORE_INDEX          3
#define CYW_SDIO_CORE_BASE             0x18002000
#define CYW_SDIO_INTSTATUS             0x20
#define CYW_SDIO_HOSTINTMASK           0x24
#define CYW_SDIO_TOSBMAILBOX           0x40
#define CYW_SDIO_TOHOSTMAILBOX         0x44
#define CYW_SDIO_TOSBMAILBOXDATA       0x48
#define CYW_SDIO_TOHOSTMAILBOXDATA     0x4c
#define CYW_SDIO_I_HMB_HOST_INT        BIT(7)
#define CYW_SDIO_I_HMB_FRAME_IND       BIT(6)
#define CYW_SDIO_HMB_DATA_FWREADY      BIT(3)
#define CYW_SDIO_HMB_DATA_VERSION_SHIFT 16
#define CYW_SDIO_PROTOCOL_VERSION      4
#define CYW_SDIO_SMB_INT_ACK           BIT(1)
#define CYW_SDPCM_HEADER_SIZE           12
#define CYW_SDPCM_HWEXT_SIZE             8
#define CYW_SDPCM_CONTROL_CHANNEL       0
#define CYW_SDPCM_EVENT_CHANNEL         1
#define CYW_SDPCM_DATA_CHANNEL          2
#define CYW_BCDC_DCMD_SIZE              16
#define CYW_BCDC_HEADER_SIZE            4
#define CYW_BCDC_PROTOCOL_VERSION       2
#define CYW_BCDC_WLC_GET_VERSION        1
#define CYW_BCDC_WLC_GET_BANDLIST       140
#define CYW_BCDC_WLC_GET_VAR            262
#define CYW_BCDC_WLC_SET_VAR            263
#define CYW_BCDC_IOCTL_VERSION           2

static const uint8_t cyw_sdio_cis_common[] = {
    0x20, 0x04, 0xd0, 0x02, 0xbf, 0xa9, /* Broadcom CYW43455 */
    0x21, 0x02, 0x0c, 0x00,             /* SDIO function */
    0x22, 0x04, 0x00, 0x00, 0x02, 0x32, /* 512-byte, 25 MHz */
    0xff,
};

static const uint8_t cyw_sdio_cis_func1[49] = {
    [0] = 0x21, [1] = 0x02, [2] = 0x0c,
    [4] = 0x22, [5] = 42, [6] = 0x01,
    [18] = 0x40, [19] = 0x00, /* 64-byte maximum block */
    [34] = 10,                /* 100 ms enable timeout */
    [48] = 0xff,
};

static const uint8_t cyw_sdio_cis_func2[49] = {
    [0] = 0x21, [1] = 0x02, [2] = 0x0c,
    [4] = 0x22, [5] = 42, [6] = 0x01,
    [18] = 0x00, [19] = 0x02, /* 512-byte maximum block */
    [34] = 10,                /* 100 ms enable timeout */
    [48] = 0xff,
};

static uint8_t cyw_sdio_cis_read(uint32_t address)
{
    if (address >= 0x1000 &&
        address < 0x1000 + sizeof(cyw_sdio_cis_common)) {
        return cyw_sdio_cis_common[address - 0x1000];
    }
    if (address >= 0x1100 &&
        address < 0x1100 + sizeof(cyw_sdio_cis_func1)) {
        return cyw_sdio_cis_func1[address - 0x1100];
    }
    if (address >= 0x1200 &&
        address < 0x1200 + sizeof(cyw_sdio_cis_func2)) {
        return cyw_sdio_cis_func2[address - 0x1200];
    }
    return 0xff;
}

static uint32_t cyw_sdio_backplane_address(SDState *sd, uint32_t address)
{
    uint32_t window = (uint32_t)sd->cyw_func1_regs[
        CYW_SDIO_FUNC1_SBADDRLOW] << 8;

    window |= (uint32_t)sd->cyw_func1_regs[
        CYW_SDIO_FUNC1_SBADDRMID] << 16;
    window |= (uint32_t)sd->cyw_func1_regs[
        CYW_SDIO_FUNC1_SBADDRHIGH] << 24;
    return window | (address & CYW_SDIO_SB_OFT_MASK);
}

static int cyw_sdio_wrapper_index(uint32_t address, uint32_t *offset)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(cyw_sdio_core_wrap); i++) {
        if (address >= cyw_sdio_core_wrap[i] &&
            address < cyw_sdio_core_wrap[i] + 0x1000) {
            *offset = address - cyw_sdio_core_wrap[i];
            return i;
        }
    }
    return -1;
}

static void cyw_sdio_update_irq(SDState *sd)
{
    bool enabled = (sd->cyw_cccr[CYW_SDIO_CCCR_INT_ENABLE] &
                    (BIT(0) | BIT(1))) == (BIT(0) | BIT(1));
    bool level = enabled && (sd->cyw_intstatus & sd->cyw_hostintmask);

    sd->cyw_cccr[CYW_SDIO_CCCR_INT_PENDING] = level ? BIT(1) : 0;
    if (sd->cyw_sdio_irq != level) {
        trace_cyw_sdio_irq(level, sd->cyw_intstatus,
                           sd->cyw_hostintmask,
                           sd->cyw_cccr[CYW_SDIO_CCCR_INT_ENABLE]);
        sd->cyw_sdio_irq = level;
        sdbus_set_sdio_irq(
            SD_BUS(qdev_get_parent_bus(DEVICE(sd))), level);
    }
}

static bool cyw_sdio_register_read(SDState *sd, uint32_t address,
                                   uint32_t *value)
{
    uint32_t offset;
    int index = cyw_sdio_wrapper_index(address, &offset);

    if (address == CYW_SDIO_CHIPCOMMON_BASE) {
        *value = CYW_SDIO_CHIP_ID;
        return true;
    }
    if (address == CYW_SDIO_CHIPCOMMON_BASE + 0xfc) {
        *value = CYW_SDIO_EROM_BASE;
        return true;
    }
    if (address >= CYW_SDIO_EROM_BASE &&
        address < CYW_SDIO_EROM_BASE + sizeof(cyw_sdio_erom)) {
        *value = cyw_sdio_erom[
            (address - CYW_SDIO_EROM_BASE) / sizeof(uint32_t)];
        return true;
    }
    if (index >= 0) {
        if (offset == CYW_BCMA_IOCTL) {
            *value = sd->cyw_core_ioctl[index];
            return true;
        }
        if (offset == CYW_BCMA_RESET_CTL) {
            *value = sd->cyw_core_reset[index];
            return true;
        }
        if (offset == CYW_BCMA_RESET_ST) {
            *value = 0;
            return true;
        }
    }
    if (address == cyw_sdio_core_base[3] + CYW_ARMCR4_CAP) {
        *value = CYW_ARMCR4_BANK_COUNT;
        return true;
    }
    if (address == cyw_sdio_core_base[3] + CYW_ARMCR4_BANKIDX) {
        *value = sd->cyw_armcr4_bankidx;
        return true;
    }
    if (address == cyw_sdio_core_base[3] + CYW_ARMCR4_BANKINFO) {
        *value = sd->cyw_armcr4_bankidx == 0 ?
                 CYW_ARMCR4_BANKINFO_800K : 0;
        return true;
    }
    if (address >= CYW_SDIO_CORE_BASE &&
        address < CYW_SDIO_CORE_BASE + 0x1000) {
        switch (address - CYW_SDIO_CORE_BASE) {
        case CYW_SDIO_INTSTATUS:
            *value = sd->cyw_intstatus;
            return true;
        case CYW_SDIO_HOSTINTMASK:
            *value = sd->cyw_hostintmask;
            return true;
        case CYW_SDIO_TOSBMAILBOX:
            *value = sd->cyw_tosbmailbox;
            return true;
        case CYW_SDIO_TOHOSTMAILBOX:
            *value = sd->cyw_tohostmailbox;
            return true;
        case CYW_SDIO_TOSBMAILBOXDATA:
            *value = sd->cyw_tosbmailboxdata;
            return true;
        case CYW_SDIO_TOHOSTMAILBOXDATA:
            *value = sd->cyw_tohostmailboxdata;
            return true;
        }
    }
    return false;
}

static uint8_t cyw_sdio_backplane_read(SDState *sd, uint32_t address)
{
    uint32_t value;
    uint32_t aligned = address & ~3;

    if (cyw_sdio_register_read(sd, aligned, &value)) {
        return (value >> ((address & 3) * 8)) & 0xff;
    }
    if (address < sizeof(sd->cyw_reset_vector)) {
        return (sd->cyw_reset_vector >> (address * 8)) & 0xff;
    }
    if (address >= CYW_SDIO_RAM_BASE &&
        address < CYW_SDIO_RAM_BASE + sd->cyw_ram_size) {
        return sd->cyw_ram[address - CYW_SDIO_RAM_BASE];
    }
    return 0;
}

static bool cyw_sdio_nvram_valid(SDState *sd, uint32_t *size)
{
    uint32_t token = ldl_le_p(sd->cyw_ram + sd->cyw_ram_size -
                             sizeof(token));
    uint16_t words = token;

    if (!words || (uint16_t)(token >> 16) != (uint16_t)~words ||
        (uint32_t)words * sizeof(uint32_t) > sd->cyw_ram_size) {
        return false;
    }
    *size = (uint32_t)words * sizeof(uint32_t);
    return true;
}

static void cyw_sdio_armcr4_release(SDState *sd)
{
    uint32_t nvram_size;
    uint32_t shared_offset;

    sd->cyw_firmware_started = false;
    sd->cyw_nvram_size = 0;
    sd->cyw_shared_address = 0;
    if (!sd->cyw_reset_vector_valid || !sd->cyw_firmware_bytes ||
        !cyw_sdio_nvram_valid(sd, &nvram_size)) {
        sd->cyw_start_failures++;
        return;
    }
    sd->cyw_nvram_size = nvram_size;
    shared_offset = ROUND_DOWN(sd->cyw_ram_size - sizeof(uint32_t) -
                               nvram_size - CYW_SDPCM_SHARED_SIZE,
                               sizeof(uint32_t));
    memset(sd->cyw_ram + shared_offset, 0, CYW_SDPCM_SHARED_SIZE);
    stl_le_p(sd->cyw_ram + shared_offset, CYW_SDPCM_SHARED_VERSION);
    sd->cyw_shared_address = CYW_SDIO_RAM_BASE + shared_offset;
    stl_le_p(sd->cyw_ram + sd->cyw_ram_size - sizeof(uint32_t),
             sd->cyw_shared_address);
    sd->cyw_firmware_started = true;
    qemu_flush_queued_packets(qemu_get_queue(sd->cyw_nic));
}

static void cyw_sdio_firmware_ready(SDState *sd)
{
    if (!sd->cyw_firmware_started) {
        return;
    }
    sd->cyw_tohostmailboxdata =
        (CYW_SDIO_PROTOCOL_VERSION << CYW_SDIO_HMB_DATA_VERSION_SHIFT) |
        CYW_SDIO_HMB_DATA_FWREADY;
    sd->cyw_intstatus |= CYW_SDIO_I_HMB_HOST_INT;
    cyw_sdio_update_irq(sd);
}

static void cyw_sdio_bcdc_response(SDState *sd, uint8_t *dcmd,
                                    size_t available)
{
    static const char firmware_version[] =
        "QEMU Raspberry Pi CYW43455 7.45.241";
    static const char capabilities[] = "sta";
    uint8_t *payload = dcmd + CYW_BCDC_DCMD_SIZE;
    size_t payload_size = available - CYW_BCDC_DCMD_SIZE;
    uint32_t command = ldl_le_p(dcmd);
    uint32_t flags = ldl_le_p(dcmd + 8);
    const char *iovar = (const char *)payload;
    size_t iovar_size = strnlen(iovar, payload_size);
    const void *response = NULL;
    size_t response_size = 0;

    stl_le_p(dcmd + 12, 0);
    if ((command == CYW_BCDC_WLC_GET_VAR ||
         command == CYW_BCDC_WLC_SET_VAR) &&
        iovar_size < payload_size) {
        trace_cyw_sdio_iovar(iovar, command == CYW_BCDC_WLC_SET_VAR);
        if (command == CYW_BCDC_WLC_SET_VAR &&
            !strcmp(iovar, "allmulti") &&
            sd->cyw_nic_conf.peers.ncs[0] &&
            !sd->cyw_link_event_sent) {
            sd->cyw_link_event_pending = true;
            sd->cyw_link_event_sent = true;
        }
    }
    if (flags & BIT(1) || command == CYW_BCDC_WLC_SET_VAR) {
        return;
    }
    if (command == CYW_BCDC_WLC_GET_VERSION) {
        memset(payload, 0, payload_size);
        if (payload_size >= sizeof(uint32_t)) {
            stl_le_p(payload, CYW_BCDC_IOCTL_VERSION);
        }
        return;
    } else if (command == CYW_BCDC_WLC_GET_BANDLIST) {
        memset(payload, 0, payload_size);
        if (payload_size >= 2 * sizeof(uint32_t)) {
            stl_le_p(payload, 1);     /* one supported band */
            stl_le_p(payload + 4, 2); /* WLC_BAND_2G */
        }
        return;
    } else if (command == CYW_BCDC_WLC_GET_VAR &&
               iovar_size < payload_size) {
        if (!strcmp(iovar, "ver")) {
            response = firmware_version;
            response_size = sizeof(firmware_version);
        } else if (!strcmp(iovar, "cur_etheraddr")) {
            response = sd->cyw_nic_conf.macaddr.a;
            response_size = sizeof(sd->cyw_nic_conf.macaddr.a);
        } else if (!strcmp(iovar, "cap")) {
            response = capabilities;
            response_size = sizeof(capabilities);
        } else if (!strcmp(iovar, "chanspec")) {
            memset(payload, 0, payload_size);
            if (payload_size >= sizeof(uint32_t)) {
                stl_le_p(payload, 0x2b01); /* channel 1, 20 MHz, 2.4 GHz */
            }
            return;
        }
    }
    memset(payload, 0, payload_size);
    if (response) {
        memcpy(payload, response, MIN(payload_size, response_size));
    }
}

static void cyw_sdio_queue_control_response(SDState *sd)
{
    uint16_t frame_size = lduw_le_p(sd->cyw_transfer);
    uint16_t checksum = lduw_le_p(sd->cyw_transfer + 2);
    uint32_t sw_header;
    uint16_t response_size;
    uint8_t data_offset;

    trace_cyw_sdio_control_header(
        ldl_le_p(sd->cyw_transfer), ldl_le_p(sd->cyw_transfer + 4),
        ldl_le_p(sd->cyw_transfer + 8), ldl_le_p(sd->cyw_transfer + 12));
    if (frame_size < CYW_SDPCM_HEADER_SIZE + CYW_BCDC_DCMD_SIZE ||
        frame_size > sd->cyw_transfer_size ||
        checksum != (uint16_t)~frame_size ||
        sd->cyw_rx_queue_size != sd->cyw_rx_queue_offset) {
        trace_cyw_sdio_control_reject(
            1, frame_size, sd->cyw_transfer_size, 0,
            sd->cyw_rx_queue_size, sd->cyw_rx_queue_offset);
        sd->cyw_control_rejections++;
        return;
    }
    sw_header = ldl_le_p(sd->cyw_transfer + 4);
    data_offset = extract32(sw_header, 24, 8);
    /*
     * Once brcmfmac enables txglom, control frames carry an eight-byte
     * hardware-extension header between the length tag and software header.
     */
    if ((data_offset < CYW_SDPCM_HEADER_SIZE ||
         data_offset + CYW_BCDC_DCMD_SIZE > frame_size) &&
        frame_size >= CYW_SDPCM_HEADER_SIZE + CYW_SDPCM_HWEXT_SIZE +
                      CYW_BCDC_DCMD_SIZE) {
        uint32_t extended_sw_header =
            ldl_le_p(sd->cyw_transfer + sizeof(uint32_t) +
                     CYW_SDPCM_HWEXT_SIZE);
        uint8_t extended_data_offset =
            extract32(extended_sw_header, 24, 8);

        if (extended_data_offset >=
                CYW_SDPCM_HEADER_SIZE + CYW_SDPCM_HWEXT_SIZE &&
            extended_data_offset + CYW_BCDC_DCMD_SIZE <= frame_size) {
            sw_header = extended_sw_header;
            data_offset = extended_data_offset;
        }
    }
    if (data_offset < CYW_SDPCM_HEADER_SIZE ||
        data_offset + CYW_BCDC_DCMD_SIZE > frame_size ||
        frame_size > sizeof(sd->cyw_rx_queue)) {
        trace_cyw_sdio_control_reject(
            2, frame_size, sd->cyw_transfer_size, data_offset,
            sd->cyw_rx_queue_size, sd->cyw_rx_queue_offset);
        sd->cyw_control_rejections++;
        return;
    }
    sd->cyw_tx_sequence_max = extract32(sw_header, 0, 8) + 2;
    if (extract32(sw_header, 8, 4) == CYW_SDPCM_DATA_CHANNEL) {
        uint8_t *bcdc = sd->cyw_transfer + data_offset;
        uint32_t packet_offset = data_offset + CYW_BCDC_HEADER_SIZE +
                                 ((uint32_t)bcdc[3] << 2);

        if (extract32(bcdc[0], 4, 4) != CYW_BCDC_PROTOCOL_VERSION ||
            packet_offset > frame_size) {
            trace_cyw_sdio_control_reject(
                3, frame_size, sd->cyw_transfer_size, data_offset,
                sd->cyw_rx_queue_size, sd->cyw_rx_queue_offset);
            sd->cyw_control_rejections++;
            return;
        }
        if (!cyw_sdio_inject_packet_drop(sd, CYW_PACKET_DROP_TX)) {
            qemu_send_packet(qemu_get_queue(sd->cyw_nic),
                             sd->cyw_transfer + packet_offset,
                             frame_size - packet_offset);
            sd->cyw_data_tx_packets++;
        }
        return;
    }
    if (extract32(sw_header, 8, 4) != CYW_SDPCM_CONTROL_CHANNEL) {
        trace_cyw_sdio_control_reject(
            4, frame_size, sd->cyw_transfer_size, data_offset,
            sd->cyw_rx_queue_size, sd->cyw_rx_queue_offset);
        sd->cyw_control_rejections++;
        return;
    }
    trace_cyw_sdio_control(frame_size, sd->cyw_transfer_size, data_offset,
                           ldl_le_p(sd->cyw_transfer + data_offset),
                           ldl_le_p(sd->cyw_transfer + data_offset + 8));

    /*
     * Firmware replies use the ordinary receive header even when the host
     * request used the transmit-glom hardware extension.
     */
    response_size = CYW_SDPCM_HEADER_SIZE + frame_size - data_offset;
    memset(sd->cyw_rx_queue, 0, response_size);
    memcpy(sd->cyw_rx_queue + CYW_SDPCM_HEADER_SIZE,
           sd->cyw_transfer + data_offset, frame_size - data_offset);
    stw_le_p(sd->cyw_rx_queue, response_size);
    stw_le_p(sd->cyw_rx_queue + 2, (uint16_t)~response_size);
    stl_le_p(sd->cyw_rx_queue + sizeof(uint32_t),
             sd->cyw_rx_sequence++ |
             (CYW_SDPCM_CONTROL_CHANNEL << 8) |
             (CYW_SDPCM_HEADER_SIZE << 24));
    stl_le_p(sd->cyw_rx_queue + 2 * sizeof(uint32_t),
             (uint32_t)(extract32(sw_header, 0, 8) + 2) << 8);
    cyw_sdio_bcdc_response(
        sd,
        sd->cyw_rx_queue + CYW_SDPCM_HEADER_SIZE,
        response_size - CYW_SDPCM_HEADER_SIZE);
    if (sd->cyw_link_event_pending) {
        stl_le_p(sd->cyw_rx_queue + 8,
                 ldl_le_p(sd->cyw_rx_queue + 8) |
                 (DIV_ROUND_UP(88, 16) << 16));
    }
    sd->cyw_rx_queue_size = response_size;
    sd->cyw_rx_queue_offset = 0;
    sd->cyw_func1_regs[0x1b] = response_size;
    sd->cyw_func1_regs[0x1c] = response_size >> 8;
    sd->cyw_control_requests++;
    sd->cyw_intstatus |= CYW_SDIO_I_HMB_FRAME_IND;
    cyw_sdio_update_irq(sd);
}

static void cyw_sdio_queue_link_event(SDState *sd)
{
    static const uint8_t bssid[6] = { 0x02, 0, 0, 0, 0, 1 };
    const uint32_t bcdc_offset = CYW_SDPCM_HEADER_SIZE;
    const uint32_t ethernet_offset = bcdc_offset + CYW_BCDC_HEADER_SIZE;
    const uint32_t vendor_offset = ethernet_offset + 14;
    const uint32_t message_offset = vendor_offset + 10;
    const uint32_t frame_size = message_offset + 48;

    memset(sd->cyw_rx_queue, 0, frame_size);
    stw_le_p(sd->cyw_rx_queue, frame_size);
    stw_le_p(sd->cyw_rx_queue + 2, (uint16_t)~frame_size);
    stl_le_p(sd->cyw_rx_queue + 4,
             sd->cyw_rx_sequence++ |
             (CYW_SDPCM_EVENT_CHANNEL << 8) |
             (CYW_SDPCM_HEADER_SIZE << 24));
    stl_le_p(sd->cyw_rx_queue + 8, sd->cyw_tx_sequence_max << 8);
    sd->cyw_rx_queue[bcdc_offset] = CYW_BCDC_PROTOCOL_VERSION << 4;
    memcpy(sd->cyw_rx_queue + ethernet_offset,
           sd->cyw_nic_conf.macaddr.a, 6);
    memcpy(sd->cyw_rx_queue + ethernet_offset + 6, bssid, sizeof(bssid));
    stw_be_p(sd->cyw_rx_queue + ethernet_offset + 12, 0x886c);
    stw_be_p(sd->cyw_rx_queue + vendor_offset, 0x8001);
    stw_be_p(sd->cyw_rx_queue + vendor_offset + 2, 48);
    sd->cyw_rx_queue[vendor_offset + 5] = 0x00;
    sd->cyw_rx_queue[vendor_offset + 6] = 0x10;
    sd->cyw_rx_queue[vendor_offset + 7] = 0x18;
    stw_be_p(sd->cyw_rx_queue + vendor_offset + 8, 1);
    stw_be_p(sd->cyw_rx_queue + message_offset, 2);
    stw_be_p(sd->cyw_rx_queue + message_offset + 2, 1);
    stl_be_p(sd->cyw_rx_queue + message_offset + 4, 0); /* SET_SSID */
    memcpy(sd->cyw_rx_queue + message_offset + 24, bssid, sizeof(bssid));
    memcpy(sd->cyw_rx_queue + message_offset + 30, "wlan0", 6);
    sd->cyw_rx_queue_size = frame_size;
    sd->cyw_rx_queue_offset = 0;
    sd->cyw_func1_regs[0x1b] = frame_size;
    sd->cyw_func1_regs[0x1c] = frame_size >> 8;
    sd->cyw_link_event_pending = false;
    sd->cyw_intstatus |= CYW_SDIO_I_HMB_FRAME_IND;
    trace_cyw_sdio_link_event(frame_size);
    cyw_sdio_update_irq(sd);
}

static void cyw_sdio_backplane_write(SDState *sd, uint32_t address,
                                     uint8_t value)
{
    uint32_t aligned = address & ~3;
    uint32_t offset;
    int index = cyw_sdio_wrapper_index(aligned, &offset);

    if (index >= 0 && offset == CYW_BCMA_IOCTL) {
        sd->cyw_core_ioctl[index] = deposit32(
            sd->cyw_core_ioctl[index], (address & 3) * 8, 8, value);
        return;
    }
    if (index >= 0 && offset == CYW_BCMA_RESET_CTL) {
        uint32_t old = sd->cyw_core_reset[index];

        sd->cyw_core_reset[index] = deposit32(
            sd->cyw_core_reset[index], (address & 3) * 8, 8, value) & BIT(0);
        if (index == CYW_ARMCR4_CORE_INDEX && (old & BIT(0)) &&
            !(sd->cyw_core_reset[index] & BIT(0))) {
            cyw_sdio_armcr4_release(sd);
        } else if (index == CYW_ARMCR4_CORE_INDEX &&
                   sd->cyw_core_reset[index] & BIT(0)) {
            sd->cyw_firmware_started = false;
        }
        return;
    }
    if (aligned == cyw_sdio_core_base[3] + CYW_ARMCR4_BANKIDX) {
        sd->cyw_armcr4_bankidx = deposit32(
            sd->cyw_armcr4_bankidx, (address & 3) * 8, 8, value);
        return;
    }
    if (aligned >= CYW_SDIO_CORE_BASE &&
        aligned < CYW_SDIO_CORE_BASE + 0x1000) {
        uint32_t core_offset = aligned - CYW_SDIO_CORE_BASE;

        switch (core_offset) {
        case CYW_SDIO_INTSTATUS:
            sd->cyw_intstatus &= ~((uint32_t)value <<
                                   ((address & 3) * 8));
            cyw_sdio_update_irq(sd);
            return;
        case CYW_SDIO_HOSTINTMASK:
            sd->cyw_hostintmask = deposit32(
                sd->cyw_hostintmask, (address & 3) * 8, 8, value);
            cyw_sdio_update_irq(sd);
            return;
        case CYW_SDIO_TOSBMAILBOX:
            sd->cyw_tosbmailbox = deposit32(
                sd->cyw_tosbmailbox, (address & 3) * 8, 8, value);
            if (sd->cyw_tosbmailbox & CYW_SDIO_SMB_INT_ACK) {
                sd->cyw_intstatus &= ~CYW_SDIO_I_HMB_HOST_INT;
                sd->cyw_tosbmailbox &= ~CYW_SDIO_SMB_INT_ACK;
                cyw_sdio_update_irq(sd);
            }
            return;
        case CYW_SDIO_TOSBMAILBOXDATA:
            sd->cyw_tosbmailboxdata = deposit32(
                sd->cyw_tosbmailboxdata, (address & 3) * 8, 8, value);
            return;
        }
    }
    if (address < sizeof(sd->cyw_reset_vector)) {
        sd->cyw_reset_vector = deposit32(
            sd->cyw_reset_vector, address * 8, 8, value);
        sd->cyw_reset_vector_valid = true;
        return;
    }
    if (address >= CYW_SDIO_RAM_BASE &&
        address < CYW_SDIO_RAM_BASE + sd->cyw_ram_size) {
        if (sd->cyw_firmware_started) {
            trace_cyw_sdio_runtime_ram_write(address);
        }
        sd->cyw_ram[address - CYW_SDIO_RAM_BASE] = value;
        sd->cyw_firmware_bytes++;
        sd->cyw_firmware_started = false;
    }
}

static uint8_t cyw_sdio_read_byte(SDState *sd, uint8_t function,
                                  uint32_t address)
{
    if (function == 0) {
        if (address >= 0x1000 && address < 0x1300) {
            return cyw_sdio_cis_read(address);
        }
        if (address < sizeof(sd->cyw_cccr)) {
            return sd->cyw_cccr[address];
        }
        if (address < sizeof(sd->cyw_fbr)) {
            return sd->cyw_fbr[address];
        }
        return 0xff;
    }
    if (function == 1) {
        if (address >= CYW_SDIO_FUNC1_MISC_BASE &&
            address < CYW_SDIO_FUNC1_MISC_BASE +
                      sizeof(sd->cyw_func1_regs)) {
            return sd->cyw_func1_regs[
                address - CYW_SDIO_FUNC1_MISC_BASE];
        }
        address = cyw_sdio_backplane_address(sd, address);
        return cyw_sdio_backplane_read(sd, address);
    }
    if (function == 2 && sd->cyw_transfer_offset <
                         sd->cyw_transfer_size) {
        uint32_t offset = sd->cyw_rx_queue_offset +
                          sd->cyw_transfer_offset;

        return offset < sd->cyw_rx_queue_size ?
               sd->cyw_rx_queue[offset] : 0;
    }
    return 0xff;
}

static void cyw_sdio_write_byte(SDState *sd, uint8_t function,
                                uint32_t address, uint8_t value)
{
    if (function == 0) {
        if (address < sizeof(sd->cyw_cccr)) {
            sd->cyw_cccr[address] = value;
            if (address == CYW_SDIO_CCCR_IO_ENABLE) {
                sd->cyw_cccr[CYW_SDIO_CCCR_IO_READY] =
                    value & (BIT(1) | BIT(2));
                if (value & BIT(2)) {
                    cyw_sdio_firmware_ready(sd);
                }
            } else if (address == CYW_SDIO_CCCR_INT_ENABLE) {
                cyw_sdio_update_irq(sd);
            } else if (address == CYW_SDIO_CCCR_IO_ABORT &&
                       value & BIT(3)) {
                sd->cyw_transfer_size = 0;
                sd->cyw_transfer_offset = 0;
            } else if (address == CYW_SDIO_CCCR_BRCM_CARDCTRL &&
                       value & BIT(1)) {
                memset(sd->cyw_ram, 0, sd->cyw_ram_size);
                sd->cyw_reset_vector = 0;
                sd->cyw_nvram_size = 0;
                sd->cyw_reset_vector_valid = false;
                sd->cyw_firmware_started = false;
                sd->cyw_intstatus = 0;
                sd->cyw_tohostmailboxdata = 0;
                cyw_sdio_update_irq(sd);
            }
        } else if (address < sizeof(sd->cyw_fbr)) {
            sd->cyw_fbr[address] = value;
            if (address == 0x110 || address == 0x111) {
                sd->cyw_block_size[1] =
                    lduw_le_p(&sd->cyw_fbr[0x110]);
            } else if (address == 0x210 || address == 0x211) {
                sd->cyw_block_size[2] =
                    lduw_le_p(&sd->cyw_fbr[0x210]);
            }
        }
        return;
    }
    if (function == 1) {
        if (address >= CYW_SDIO_FUNC1_MISC_BASE &&
            address < CYW_SDIO_FUNC1_MISC_BASE +
                      sizeof(sd->cyw_func1_regs)) {
            unsigned int index = address - CYW_SDIO_FUNC1_MISC_BASE;

            sd->cyw_func1_regs[index] = value;
            if (index == CYW_SDIO_FUNC1_CHIPCLKCSR) {
                sd->cyw_func1_regs[index] = value | BIT(6) | BIT(7);
            } else if (index == CYW_SDIO_FUNC1_SLEEPCSR &&
                       value & BIT(0)) {
                sd->cyw_func1_regs[index] |= BIT(1);
            }
            return;
        }
        address = cyw_sdio_backplane_address(sd, address);
        cyw_sdio_backplane_write(sd, address, value);
        return;
    }
    if (function == 2 && sd->cyw_transfer_offset <
                         sizeof(sd->cyw_transfer)) {
        sd->cyw_transfer[sd->cyw_transfer_offset] = value;
        sd->cyw_tx_bytes++;
    }
}

static void cyw_sdio_reset(DeviceState *dev)
{
    SDState *sd = CYW43455_SDIO(dev);

    memset(sd->cyw_cccr, 0, sizeof(sd->cyw_cccr));
    memset(sd->cyw_fbr, 0, sizeof(sd->cyw_fbr));
    memset(sd->cyw_func1_regs, 0, sizeof(sd->cyw_func1_regs));
    memset(sd->cyw_transfer, 0, sizeof(sd->cyw_transfer));
    memset(sd->cyw_rx_queue, 0, sizeof(sd->cyw_rx_queue));
    if (sd->cyw_ram) {
        memset(sd->cyw_ram, 0, sd->cyw_ram_size);
    }
    sd->cyw_cccr[0x00] = 0x32;
    sd->cyw_cccr[0x01] = 0x03;
    sd->cyw_cccr[CYW_SDIO_CCCR_CAPS] = BIT(7) | BIT(1);
    sd->cyw_cccr[0x09] = 0x00;
    sd->cyw_cccr[0x0a] = 0x10;
    sd->cyw_cccr[CYW_SDIO_CCCR_SPEED] = BIT(0);
    sd->cyw_fbr[0x109] = 0x00;
    sd->cyw_fbr[0x10a] = 0x11;
    sd->cyw_fbr[0x209] = 0x00;
    sd->cyw_fbr[0x20a] = 0x12;
    sd->cyw_block_size[0] = 64;
    sd->cyw_block_size[1] = 64;
    sd->cyw_block_size[2] = 512;
    sd->cyw_transfer_size = 0;
    sd->cyw_transfer_offset = 0;
    sd->cyw_transfer_address = 0;
    sd->cyw_rca = 0;
    sd->cyw_function = 0;
    sd->cyw_selected = false;
    sd->cyw_transfer_write = false;
    sd->cyw_transfer_increment = false;
    sd->cyw_command_count = 0;
    sd->cyw_command_failures = 0;
    sd->cyw_firmware_bytes = 0;
    sd->cyw_tx_bytes = 0;
    sd->cyw_rx_bytes = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(sd->cyw_core_ioctl); i++) {
        sd->cyw_core_ioctl[i] = CYW_BCMA_IOCTL_CLK;
        sd->cyw_core_reset[i] = 0;
    }
    sd->cyw_armcr4_bankidx = 0;
    sd->cyw_reset_vector = 0;
    sd->cyw_nvram_size = 0;
    sd->cyw_shared_address = 0;
    sd->cyw_intstatus = 0;
    sd->cyw_hostintmask = 0;
    sd->cyw_tosbmailbox = 0;
    sd->cyw_tohostmailbox = 0;
    sd->cyw_tosbmailboxdata = 0;
    sd->cyw_tohostmailboxdata = 0;
    sd->cyw_rx_queue_size = 0;
    sd->cyw_rx_queue_offset = 0;
    sd->cyw_start_failures = 0;
    sd->cyw_control_requests = 0;
    sd->cyw_control_rejections = 0;
    sd->cyw_data_tx_packets = 0;
    sd->cyw_data_rx_packets = 0;
    sd->cyw_packet_drop_packets_seen = 0;
    sd->cyw_packet_drops_injected = 0;
    sd->cyw_rx_sequence = 0;
    sd->cyw_tx_sequence_max = 0;
    sd->cyw_reset_vector_valid = false;
    sd->cyw_firmware_started = false;
    sd->cyw_sdio_irq = false;
    sd->cyw_link_event_pending = false;
    sd->cyw_link_event_sent = false;
    sdbus_set_sdio_irq(
        SD_BUS(qdev_get_parent_bus(DEVICE(sd))), false);
}

static size_t cyw_sdio_do_command(SDState *sd, SDRequest *req,
                                  uint8_t *response, size_t respsz)
{
    uint32_t value = 0;

    if (req->cmd == sd->cyw_fail_command &&
        sd->cyw_command_count >= sd->cyw_fail_after) {
        sd->cyw_command_count++;
        sd->cyw_command_failures++;
        return 0;
    }
    sd->cyw_command_count++;
    trace_cyw_sdio_command(req->cmd, req->arg, sd->cyw_command_count);
    switch (req->cmd) {
    case 0:
        cyw_sdio_reset(DEVICE(sd));
        return 0;
    case 5:
        value = BIT(31) | (CYW_SDIO_FUNCTIONS << 28) | CYW_SDIO_OCR;
        break;
    case 3:
        sd->cyw_rca = 1;
        value = (uint32_t)sd->cyw_rca << 16;
        break;
    case 7:
        sd->cyw_selected = extract32(req->arg, 16, 16) == sd->cyw_rca;
        value = 0;
        break;
    case 52:
    {
        bool write = req->arg & BIT(31);
        bool raw = req->arg & BIT(27);
        uint8_t function = extract32(req->arg, 28, 3);
        uint32_t address = extract32(req->arg, 9, 17);
        uint8_t data = req->arg;

        if (function > CYW_SDIO_FUNCTIONS) {
            value = BIT(9);
            break;
        }
        if (write) {
            cyw_sdio_write_byte(sd, function, address, data);
        }
        value = write && !raw ? data :
                cyw_sdio_read_byte(sd, function, address);
        break;
    }
    case 53:
    {
        uint32_t count = extract32(req->arg, 0, 9);
        bool block_mode = req->arg & BIT(27);

        sd->cyw_transfer_write = req->arg & BIT(31);
        sd->cyw_function = extract32(req->arg, 28, 3);
        sd->cyw_transfer_increment = req->arg & BIT(26);
        sd->cyw_transfer_address = extract32(req->arg, 9, 17);
        if (sd->cyw_function > CYW_SDIO_FUNCTIONS) {
            sd->cyw_transfer_size = 0;
            sd->cyw_transfer_offset = 0;
            value = BIT(9);
            break;
        }
        if (!count) {
            count = 512;
        }
        if (block_mode) {
            count *= sd->cyw_block_size[sd->cyw_function];
        }
        /*
         * Function 1 is a streaming backplane aperture.  brcmfmac uses the
         * largest legal multi-block requests while downloading firmware, so
         * it must not inherit the bounded function-2 packet staging size.
         */
        sd->cyw_transfer_size = sd->cyw_function == 2 ?
            MIN(count, (uint32_t)sizeof(sd->cyw_transfer)) : count;
        sd->cyw_transfer_offset = 0;
        value = 0;
        break;
    }
    default:
        return 0;
    }
    if (respsz < sizeof(value)) {
        return 0;
    }
    stl_be_p(response, value);
    return sizeof(value);
}

static size_t cyw_sdio_write_data(SDState *sd, const void *buf, size_t len)
{
    const uint8_t *bytes = buf;
    size_t count = MIN(len, sd->cyw_transfer_size -
                            sd->cyw_transfer_offset);

    for (size_t i = 0; i < count; i++) {
        uint32_t address = sd->cyw_transfer_address;

        cyw_sdio_write_byte(sd, sd->cyw_function, address, bytes[i]);
        sd->cyw_transfer_offset++;
        if (sd->cyw_transfer_increment) {
            sd->cyw_transfer_address++;
        }
    }
    if (sd->cyw_function == 2 &&
        sd->cyw_transfer_offset == sd->cyw_transfer_size) {
        if (sd->cyw_firmware_started) {
            cyw_sdio_queue_control_response(sd);
        } else {
            memcpy(sd->cyw_rx_queue, sd->cyw_transfer,
                   sd->cyw_transfer_size);
            sd->cyw_rx_queue_size = sd->cyw_transfer_size;
            sd->cyw_rx_queue_offset = 0;
        }
    }
    return count;
}

static size_t cyw_sdio_read_data(SDState *sd, void *buf, size_t len)
{
    uint8_t *bytes = buf;
    size_t count = MIN(len, sd->cyw_transfer_size -
                            sd->cyw_transfer_offset);

    for (size_t i = 0; i < count; i++) {
        uint32_t address = sd->cyw_transfer_address;

        bytes[i] = cyw_sdio_read_byte(sd, sd->cyw_function, address);
        sd->cyw_transfer_offset++;
        sd->cyw_rx_bytes++;
        if (sd->cyw_transfer_increment) {
            sd->cyw_transfer_address++;
        }
    }
    if (sd->cyw_function == 2 &&
        sd->cyw_transfer_offset == sd->cyw_transfer_size) {
        uint32_t available = sd->cyw_rx_queue_size -
                             sd->cyw_rx_queue_offset;

        sd->cyw_rx_queue_offset += MIN(sd->cyw_transfer_size, available);
        if (sd->cyw_rx_queue_offset == sd->cyw_rx_queue_size) {
            sd->cyw_rx_queue_offset = 0;
            sd->cyw_rx_queue_size = 0;
            sd->cyw_func1_regs[0x1b] = 0;
            sd->cyw_func1_regs[0x1c] = 0;
            if (sd->cyw_link_event_pending) {
                cyw_sdio_queue_link_event(sd);
            } else {
                qemu_flush_queued_packets(qemu_get_queue(sd->cyw_nic));
            }
        }
    }
    return count;
}

static bool cyw_sdio_receive_ready(SDState *sd)
{
    return sd->cyw_transfer_write &&
           sd->cyw_transfer_offset < sd->cyw_transfer_size;
}

static bool cyw_sdio_data_ready(SDState *sd)
{
    return !sd->cyw_transfer_write &&
           sd->cyw_transfer_offset < sd->cyw_transfer_size;
}

static bool cyw_sdio_get_inserted(SDState *sd)
{
    return true;
}

static bool cyw_sdio_get_readonly(SDState *sd)
{
    return false;
}

static uint8_t cyw_sdio_get_dat_lines(SDState *sd)
{
    return sd->cyw_sdio_irq ? 0xd : 0xf;
}

static bool cyw_sdio_get_cmd_line(SDState *sd)
{
    return true;
}

static void cyw_sdio_set_voltage(SDState *sd, uint16_t millivolts)
{
}

static bool cyw_sdio_can_receive(NetClientState *nc)
{
    SDState *sd = qemu_get_nic_opaque(nc);

    return sd->cyw_firmware_started &&
           sd->cyw_rx_queue_size == sd->cyw_rx_queue_offset;
}

static bool cyw_sdio_inject_packet_drop(SDState *sd, uint8_t direction)
{
    bool selected = sd->cyw_packet_drop_direction == direction ||
                    sd->cyw_packet_drop_direction == CYW_PACKET_DROP_BOTH;
    bool drop;

    if (!selected) {
        return false;
    }
    drop = sd->cyw_packet_drop_packets_seen >= sd->cyw_packet_drop_after &&
           sd->cyw_packet_drops_injected < sd->cyw_packet_drop_count;
    sd->cyw_packet_drop_packets_seen++;
    if (drop) {
        sd->cyw_packet_drops_injected++;
        trace_cyw_sdio_packet_drop(
            direction, sd->cyw_packet_drop_packets_seen,
            sd->cyw_packet_drops_injected, sd->cyw_packet_drop_count);
    }
    return drop;
}

static ssize_t cyw_sdio_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    SDState *sd = qemu_get_nic_opaque(nc);
    uint32_t frame_size = CYW_SDPCM_HEADER_SIZE +
                          CYW_BCDC_HEADER_SIZE + size;
    uint8_t *bcdc;

    if (!cyw_sdio_can_receive(nc) ||
        frame_size > sizeof(sd->cyw_rx_queue)) {
        return 0;
    }
    if (cyw_sdio_inject_packet_drop(sd, CYW_PACKET_DROP_RX)) {
        return size;
    }
    stw_le_p(sd->cyw_rx_queue, frame_size);
    stw_le_p(sd->cyw_rx_queue + 2, (uint16_t)~frame_size);
    stl_le_p(sd->cyw_rx_queue + 4,
             sd->cyw_rx_sequence++ |
             (CYW_SDPCM_DATA_CHANNEL << 8) |
             (CYW_SDPCM_HEADER_SIZE << 24));
    stl_le_p(sd->cyw_rx_queue + 8, sd->cyw_tx_sequence_max << 8);
    bcdc = sd->cyw_rx_queue + CYW_SDPCM_HEADER_SIZE;
    bcdc[0] = CYW_BCDC_PROTOCOL_VERSION << 4;
    bcdc[1] = 0;
    bcdc[2] = 0;
    bcdc[3] = 0;
    memcpy(bcdc + CYW_BCDC_HEADER_SIZE, buf, size);
    sd->cyw_rx_queue_size = frame_size;
    sd->cyw_rx_queue_offset = 0;
    sd->cyw_func1_regs[0x1b] = frame_size;
    sd->cyw_func1_regs[0x1c] = frame_size >> 8;
    sd->cyw_data_rx_packets++;
    sd->cyw_intstatus |= CYW_SDIO_I_HMB_FRAME_IND;
    cyw_sdio_update_irq(sd);
    return size;
}

static NetClientInfo cyw_sdio_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = cyw_sdio_can_receive,
    .receive = cyw_sdio_receive,
};

static void cyw_sdio_realize(DeviceState *dev, Error **errp)
{
    SDState *sd = CYW43455_SDIO(dev);

    if (sd->cyw_packet_drop_direction > CYW_PACKET_DROP_BOTH) {
        error_setg(errp,
                   "packet-drop-direction must be 0 (none), 1 (TX), "
                   "2 (RX), or 3 (both)");
        return;
    }
    if (sd->cyw_packet_drop_direction != CYW_PACKET_DROP_NONE) {
        if (sd->cyw_packet_drop_after == UINT64_MAX) {
            error_setg(errp,
                       "packet-drop-direction requires packet-drop-after");
            return;
        }
        if (!sd->cyw_packet_drop_count) {
            error_setg(errp, "packet-drop-count must be nonzero");
            return;
        }
    }
    sd->cyw_ram_size = CYW_SDIO_DEFAULT_RAM_SIZE;
    sd->cyw_ram = g_malloc0(sd->cyw_ram_size);
    qemu_macaddr_default_if_unset(&sd->cyw_nic_conf.macaddr);
    sd->cyw_nic = qemu_new_nic(
        &cyw_sdio_net_info, &sd->cyw_nic_conf,
        object_get_typename(OBJECT(dev)), dev->id,
        &dev->mem_reentrancy_guard, sd);
    qemu_format_nic_info_str(
        qemu_get_queue(sd->cyw_nic), sd->cyw_nic_conf.macaddr.a);
    cyw_sdio_reset(dev);
}

static void cyw_sdio_unrealize(DeviceState *dev)
{
    SDState *sd = CYW43455_SDIO(dev);

    qemu_del_nic(sd->cyw_nic);
    sd->cyw_nic = NULL;
}

static int cyw_sdio_post_load(void *opaque, int version_id)
{
    SDState *sd = opaque;

    if (version_id < 2) {
        for (unsigned int i = 0; i < ARRAY_SIZE(sd->cyw_core_ioctl); i++) {
            sd->cyw_core_ioctl[i] = CYW_BCMA_IOCTL_CLK;
            sd->cyw_core_reset[i] = 0;
        }
        sd->cyw_armcr4_bankidx = 0;
    }
    if (version_id < 3) {
        sd->cyw_reset_vector = 0;
        sd->cyw_nvram_size = 0;
        sd->cyw_start_failures = 0;
        sd->cyw_reset_vector_valid = false;
        sd->cyw_firmware_started = false;
    }
    if (version_id < 4) {
        sd->cyw_intstatus = 0;
        sd->cyw_hostintmask = 0;
        sd->cyw_tosbmailbox = 0;
        sd->cyw_tohostmailbox = 0;
        sd->cyw_tosbmailboxdata = 0;
        sd->cyw_tohostmailboxdata = 0;
        sd->cyw_sdio_irq = false;
    }
    if (version_id < 5) {
        memset(sd->cyw_rx_queue, 0, sizeof(sd->cyw_rx_queue));
        sd->cyw_rx_queue_size = 0;
        sd->cyw_rx_queue_offset = 0;
        sd->cyw_control_requests = 0;
        sd->cyw_control_rejections = 0;
        sd->cyw_rx_sequence = 0;
    }
    if (version_id < 6) {
        sd->cyw_data_tx_packets = 0;
        sd->cyw_data_rx_packets = 0;
    }
    if (version_id < 7) {
        sd->cyw_shared_address = 0;
    }
    if (version_id < 8) {
        memset(sd->cyw_transfer + 2048, 0,
               sizeof(sd->cyw_transfer) - 2048);
        memset(sd->cyw_rx_queue + 4096, 0,
               sizeof(sd->cyw_rx_queue) - 4096);
    }
    if (version_id < 9) {
        sd->cyw_link_event_pending = false;
        sd->cyw_link_event_sent = false;
        sd->cyw_tx_sequence_max = 0;
    }
    if (version_id < 10) {
        sd->cyw_packet_drop_packets_seen = 0;
        sd->cyw_packet_drops_injected = 0;
    }
    if (sd->cyw_ram_size != CYW_SDIO_DEFAULT_RAM_SIZE ||
        (sd->cyw_function == 2 &&
         sd->cyw_transfer_size > sizeof(sd->cyw_transfer)) ||
        sd->cyw_transfer_offset > sd->cyw_transfer_size ||
        sd->cyw_function > CYW_SDIO_FUNCTIONS ||
        sd->cyw_armcr4_bankidx >= CYW_ARMCR4_BANK_COUNT ||
        sd->cyw_rx_queue_size > sizeof(sd->cyw_rx_queue) ||
        sd->cyw_rx_queue_offset > sd->cyw_rx_queue_size) {
        return -EINVAL;
    }
    sd->cyw_sdio_irq = false;
    cyw_sdio_update_irq(sd);
    return 0;
}

static void cyw_sdio_get_command_count(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_command_count;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_command_failures(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_command_failures;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_firmware_bytes(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_firmware_bytes;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_tx_bytes(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_tx_bytes;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_rx_bytes(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_rx_bytes;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_reset_vector(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    uint32_t value = CYW43455_SDIO(obj)->cyw_reset_vector;

    visit_type_uint32(v, name, &value, errp);
}

static void cyw_sdio_get_nvram_size(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    uint32_t value = CYW43455_SDIO(obj)->cyw_nvram_size;

    visit_type_uint32(v, name, &value, errp);
}

static void cyw_sdio_get_start_failures(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_start_failures;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_shared_address(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    uint32_t value = CYW43455_SDIO(obj)->cyw_shared_address;

    visit_type_uint32(v, name, &value, errp);
}

static void cyw_sdio_get_firmware_started(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    bool value = CYW43455_SDIO(obj)->cyw_firmware_started;

    visit_type_bool(v, name, &value, errp);
}

static void cyw_sdio_get_control_requests(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_control_requests;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_control_rejections(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_control_rejections;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_data_tx_packets(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_data_tx_packets;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_data_rx_packets(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_data_rx_packets;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_packet_drop_packets_seen(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = CYW43455_SDIO(obj)->cyw_packet_drop_packets_seen;

    visit_type_uint64(v, name, &value, errp);
}

static void cyw_sdio_get_packet_drops_injected(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value = CYW43455_SDIO(obj)->cyw_packet_drops_injected;

    visit_type_uint32(v, name, &value, errp);
}

static const Property cyw_sdio_properties[] = {
    DEFINE_PROP_UINT8("fail-command", SDState, cyw_fail_command, UINT8_MAX),
    DEFINE_PROP_UINT64("fail-after", SDState, cyw_fail_after, UINT64_MAX),
    DEFINE_PROP_UINT8("packet-drop-direction", SDState,
                      cyw_packet_drop_direction, CYW_PACKET_DROP_NONE),
    DEFINE_PROP_UINT64("packet-drop-after", SDState,
                       cyw_packet_drop_after, UINT64_MAX),
    DEFINE_PROP_UINT32("packet-drop-count", SDState,
                       cyw_packet_drop_count, 1),
    DEFINE_NIC_PROPERTIES(SDState, cyw_nic_conf),
};

static const VMStateDescription cyw_sdio_vmstate = {
    .name = TYPE_CYW43455_SDIO,
    .version_id = 10,
    .minimum_version_id = 1,
    .post_load = cyw_sdio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(cyw_cccr, SDState, 0x100),
        VMSTATE_UINT8_ARRAY(cyw_fbr, SDState, 0x300),
        VMSTATE_UINT8_ARRAY(cyw_func1_regs, SDState, 0x20),
        VMSTATE_PARTIAL_BUFFER(cyw_transfer, SDState, 2048),
        VMSTATE_STATIC_BUFFER(cyw_rx_queue, SDState, 5, NULL, 0, 4096),
        /*
         * Version 8 carries complete maximum-size CMD53/BCDC buffers.
         * The legacy prefixes above preserve version 7 stream loading.
         */
        VMSTATE_BUFFER_V(cyw_transfer, SDState, 8),
        VMSTATE_BUFFER_V(cyw_rx_queue, SDState, 8),
        VMSTATE_UINT32(cyw_transfer_size, SDState),
        VMSTATE_UINT32(cyw_transfer_offset, SDState),
        VMSTATE_UINT32(cyw_transfer_address, SDState),
        VMSTATE_UINT32(cyw_ram_size, SDState),
        VMSTATE_BUFFER_POINTER_UNSAFE(cyw_ram, SDState, 1,
                                      CYW_SDIO_DEFAULT_RAM_SIZE),
        VMSTATE_UINT16(cyw_rca, SDState),
        VMSTATE_UINT16_ARRAY(cyw_block_size, SDState, 3),
        VMSTATE_UINT8(cyw_function, SDState),
        VMSTATE_UINT8(cyw_fail_command, SDState),
        VMSTATE_BOOL(cyw_selected, SDState),
        VMSTATE_BOOL(cyw_transfer_write, SDState),
        VMSTATE_BOOL(cyw_transfer_increment, SDState),
        VMSTATE_UINT64(cyw_command_count, SDState),
        VMSTATE_UINT64(cyw_fail_after, SDState),
        VMSTATE_UINT64(cyw_command_failures, SDState),
        VMSTATE_UINT64(cyw_firmware_bytes, SDState),
        VMSTATE_UINT64(cyw_tx_bytes, SDState),
        VMSTATE_UINT64(cyw_rx_bytes, SDState),
        VMSTATE_UINT32_ARRAY_V(cyw_core_ioctl, SDState, 4, 2),
        VMSTATE_UINT32_ARRAY_V(cyw_core_reset, SDState, 4, 2),
        VMSTATE_UINT32_V(cyw_armcr4_bankidx, SDState, 2),
        VMSTATE_UINT32_V(cyw_reset_vector, SDState, 3),
        VMSTATE_UINT32_V(cyw_nvram_size, SDState, 3),
        VMSTATE_UINT32_V(cyw_shared_address, SDState, 7),
        VMSTATE_UINT32_V(cyw_intstatus, SDState, 4),
        VMSTATE_UINT32_V(cyw_hostintmask, SDState, 4),
        VMSTATE_UINT32_V(cyw_tosbmailbox, SDState, 4),
        VMSTATE_UINT32_V(cyw_tohostmailbox, SDState, 4),
        VMSTATE_UINT32_V(cyw_tosbmailboxdata, SDState, 4),
        VMSTATE_UINT32_V(cyw_tohostmailboxdata, SDState, 4),
        VMSTATE_UINT32_V(cyw_rx_queue_size, SDState, 5),
        VMSTATE_UINT32_V(cyw_rx_queue_offset, SDState, 5),
        VMSTATE_UINT64_V(cyw_start_failures, SDState, 3),
        VMSTATE_UINT64_V(cyw_control_requests, SDState, 5),
        VMSTATE_UINT64_V(cyw_control_rejections, SDState, 5),
        VMSTATE_UINT64_V(cyw_data_tx_packets, SDState, 6),
        VMSTATE_UINT64_V(cyw_data_rx_packets, SDState, 6),
        VMSTATE_UINT64_V(cyw_packet_drop_packets_seen, SDState, 10),
        VMSTATE_UINT32_V(cyw_packet_drops_injected, SDState, 10),
        VMSTATE_UINT8_V(cyw_rx_sequence, SDState, 5),
        VMSTATE_UINT8_V(cyw_tx_sequence_max, SDState, 9),
        VMSTATE_BOOL_V(cyw_reset_vector_valid, SDState, 3),
        VMSTATE_BOOL_V(cyw_firmware_started, SDState, 3),
        VMSTATE_BOOL_V(cyw_sdio_irq, SDState, 4),
        VMSTATE_BOOL_V(cyw_link_event_pending, SDState, 9),
        VMSTATE_BOOL_V(cyw_link_event_sent, SDState, 9),
        VMSTATE_END_OF_LIST()
    },
};

static void cyw_sdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->desc = "Infineon CYW43455 SDIO transport";
    dc->realize = cyw_sdio_realize;
    dc->unrealize = cyw_sdio_unrealize;
    dc->vmsd = &cyw_sdio_vmstate;
    device_class_set_legacy_reset(dc, cyw_sdio_reset);
    device_class_set_props(dc, cyw_sdio_properties);
    object_class_property_add(
        klass, "command-count", "uint64", cyw_sdio_get_command_count,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "command-failures", "uint64", cyw_sdio_get_command_failures,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "firmware-bytes", "uint64", cyw_sdio_get_firmware_bytes,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "tx-bytes", "uint64", cyw_sdio_get_tx_bytes,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "rx-bytes", "uint64", cyw_sdio_get_rx_bytes,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "reset-vector", "uint32", cyw_sdio_get_reset_vector,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "nvram-size", "uint32", cyw_sdio_get_nvram_size,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "start-failures", "uint64", cyw_sdio_get_start_failures,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "shared-address", "uint32", cyw_sdio_get_shared_address,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "firmware-started", "bool", cyw_sdio_get_firmware_started,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "control-requests", "uint64", cyw_sdio_get_control_requests,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "control-rejections", "uint64",
        cyw_sdio_get_control_rejections, NULL, NULL, NULL);
    object_class_property_add(
        klass, "data-tx-packets", "uint64", cyw_sdio_get_data_tx_packets,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "data-rx-packets", "uint64", cyw_sdio_get_data_rx_packets,
        NULL, NULL, NULL);
    object_class_property_add(
        klass, "packet-drop-packets-seen", "uint64",
        cyw_sdio_get_packet_drop_packets_seen, NULL, NULL, NULL);
    object_class_property_add(
        klass, "packet-drops-injected", "uint32",
        cyw_sdio_get_packet_drops_injected, NULL, NULL, NULL);
    sc->set_voltage = cyw_sdio_set_voltage;
    sc->get_dat_lines = cyw_sdio_get_dat_lines;
    sc->get_cmd_line = cyw_sdio_get_cmd_line;
    sc->do_command = cyw_sdio_do_command;
    sc->write_data = cyw_sdio_write_data;
    sc->read_data = cyw_sdio_read_data;
    sc->receive_ready = cyw_sdio_receive_ready;
    sc->data_ready = cyw_sdio_data_ready;
    sc->get_inserted = cyw_sdio_get_inserted;
    sc->get_readonly = cyw_sdio_get_readonly;
}

static void sdmmc_common_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    device_class_set_props(dc, sdmmc_common_properties);
    dc->vmsd = &sd_vmstate;
    device_class_set_legacy_reset(dc, sd_reset);
    dc->bus_type = TYPE_SD_BUS;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    sc->set_voltage = sd_set_voltage;
    sc->get_dat_lines = sd_get_dat_lines;
    sc->get_cmd_line = sd_get_cmd_line;
    sc->do_command = sd_do_command;
    sc->write_data = sd_write_data;
    sc->read_data = sd_read_data;
    sc->receive_ready = sd_receive_ready;
    sc->data_ready = sd_data_ready;
    sc->get_inserted = sd_get_inserted;
    sc->get_readonly = sd_get_readonly;
}

static void sd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->realize = sd_realize;
    device_class_set_props(dc, sd_properties);

    sc->set_cid = sd_set_cid;
    sc->set_csd = sd_set_csd;
    sc->proto = &sd_proto_sd;
}

/*
 * We do not model the chip select pin, so allow the board to select
 * whether card should be in SSI or MMC/SD mode.  It is also up to the
 * board to ensure that ssi transfers only occur when the chip select
 * is asserted.
 */
static void sd_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->desc = "SD SPI";
    sc->proto = &sd_proto_spi;
}

static void emmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    assert(qcrypto_hmac_supports(QCRYPTO_HASH_ALGO_SHA256));

    dc->desc = "eMMC";
    dc->realize = emmc_realize;
    device_class_set_props(dc, emmc_properties);
    object_class_property_add(klass, "cache-dirty-sectors", "uint32",
                              emmc_get_cache_dirty_sectors,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        klass, "cache-dirty-sectors",
        "Number of volatile eMMC write-cache sectors awaiting flush");
    object_class_property_add_bool(klass, "cache-flush-active",
                                   emmc_get_cache_flush_active, NULL);
    object_class_property_set_description(
        klass, "cache-flush-active",
        "Whether a timer-driven eMMC cache flush is active");
    object_class_property_add(klass, "cache-flush-completed-sectors",
                              "uint64", emmc_get_cache_flush_completed,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        klass, "cache-flush-completed-sectors",
        "Durable sectors completed by the active or latest timed flush");
    object_class_property_add_bool(klass, "program-active",
                                   emmc_get_program_active, NULL);
    object_class_property_set_description(
        klass, "program-active",
        "Whether timer-driven eMMC sector programming is active");
    object_class_property_add(klass, "program-pending-sectors", "uint32",
                              emmc_get_program_pending, NULL, NULL, NULL);
    object_class_property_set_description(
        klass, "program-pending-sectors",
        "Number of received eMMC sectors awaiting durable programming");
    object_class_property_add(klass, "program-completed-sectors",
                              "uint64", emmc_get_program_completed,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        klass, "program-completed-sectors",
        "Durable sectors completed by the active or latest timed program");
    object_class_property_add_bool(klass, "erase-active",
                                   emmc_get_erase_active, NULL);
    object_class_property_set_description(
        klass, "erase-active",
        "Whether timer-driven eMMC high-capacity erase is active");
    object_class_property_add(klass, "erase-pending-groups", "uint64",
                              emmc_get_erase_pending, NULL, NULL, NULL);
    object_class_property_set_description(
        klass, "erase-pending-groups",
        "Number of eMMC high-capacity erase groups awaiting durability");
    object_class_property_add(klass, "erase-completed-groups", "uint64",
                              emmc_get_erase_completed, NULL, NULL, NULL);
    object_class_property_set_description(
        klass, "erase-completed-groups",
        "Durable groups completed by the active or latest timed erase");

    sc->proto = &sd_proto_emmc;

    sc->set_cid = emmc_set_cid;
    sc->set_csd = emmc_set_csd;
}

static const TypeInfo sd_types[] = {
    {
        .name           = TYPE_SDMMC_COMMON,
        .parent         = TYPE_DEVICE,
        .abstract       = true,
        .instance_size  = sizeof(SDState),
        .class_size     = sizeof(SDCardClass),
        .class_init     = sdmmc_common_class_init,
        .instance_init  = sd_instance_init,
        .instance_finalize = sd_instance_finalize,
    },
    {
        .name           = TYPE_SD_CARD,
        .parent         = TYPE_SDMMC_COMMON,
        .class_init     = sd_class_init,
    },
    {
        .name           = TYPE_SD_CARD_SPI,
        .parent         = TYPE_SD_CARD,
        .class_init     = sd_spi_class_init,
    },
    {
        .name           = TYPE_EMMC,
        .parent         = TYPE_SDMMC_COMMON,
        .class_init     = emmc_class_init,
    },
    {
        .name           = TYPE_CYW43455_SDIO,
        .parent         = TYPE_SDMMC_COMMON,
        .class_init     = cyw_sdio_class_init,
    },
};

DEFINE_TYPES(sd_types)
