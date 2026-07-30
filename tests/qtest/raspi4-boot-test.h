/*
 * Raspberry Pi 4 qtest shared definitions and helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TESTS_QTEST_RASPI4_BOOT_TEST_H
#define TESTS_QTEST_RASPI4_BOOT_TEST_H

#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qemu/units.h"
#include "block/nvme.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "hw/i2c/bcm2835_i2c.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/nvram/bcm2835_otp.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sdhci-internal.h"
#include "hw/usb/dwc2-device-transport.h"
#include "hw/usb/dwc2-regs.h"
#include "hw/usb/usb.h"
#include "libqtest-single.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include <libfdt.h>
#include "tests/unit/crypto-tls-x509-helpers.h"
#include <poll.h>

#ifdef CONFIG_TASN1
#endif
#ifndef _WIN32
#endif

#define EEPROM_SIZE (512 * 1024)
#define OTP_SIZE 512
#define PI4_BOARD_REVISION 0x00b03115
#define CM4_BOARD_REVISION 0x00b03140
#define CM4_TEST_EMMC_CID "aa0151434d34454d5510123456782900"
#define FILE_MAGIC 0x55aaf11f
#define SD_SIZE (8 * 1024 * 1024)
#define SD_PARTITION_LBA 2048
#define SD_PARTITION_SECTORS 14336
#define AB_SD_SIZE (32 * 1024 * 1024)
#define LOGICAL_EBR_GAP 16384
#define FAT_SECTORS 64
#define FAT_ROOT_ENTRIES 512
#define FAT_ROOT_SECTORS 32
#define FAT_DATA_SECTOR (1 + FAT_SECTORS + FAT_ROOT_SECTORS)
#define TEST_KERNEL_SIZE 4096
#define TEST_NETWORK_CONFIG \
    "arm_64bit=1\ninclude extra.txt\n"
#define TEST_NETWORK_INCLUDE \
    "dtoverlay=test,enable=on\n" \
    "initramfs initramfs-a,initramfs-b followkernel\n"
#define TEST_NETWORK_INITRAMFS_A "QEMU network initramfs A"
#define TEST_NETWORK_INITRAMFS_B "QEMU network initramfs B"
#define TEST_RESTART_WATCHDOG_NS 10009765
#define RASPI4_PCIE_ECAM_BASE 0xfd500000
#define RASPI4_LOW_RAM_END 0xfc000000ULL
#define RASPI4_HIGH_RAM_BASE (4 * GiB)
#define RASPI4_PCIE_MMIO_BASE 0x600000000ULL
#define RASPI4_PCIE_EXT_CFG_INDEX (RASPI4_PCIE_ECAM_BASE + 0x9000)
#define RASPI4_PCIE_EXT_CFG_DATA (RASPI4_PCIE_ECAM_BASE + 0x8000)
#define RASPI4_PCIE_MSI_TARGET_LOW 0x0fffffffcULL
#define RASPI4_PCIE_MSI_TARGET_HIGH 0xffffffffcULL
#define RASPI4_PCIE_MSI_BAR_CONFIG_LO (RASPI4_PCIE_ECAM_BASE + 0x4044)
#define RASPI4_PCIE_MSI_BAR_CONFIG_HI (RASPI4_PCIE_ECAM_BASE + 0x4048)
#define RASPI4_PCIE_MSI_DATA_CONFIG (RASPI4_PCIE_ECAM_BASE + 0x404c)
#define RASPI4_PCIE_MSI_INTR_STATUS (RASPI4_PCIE_ECAM_BASE + 0x4500)
#define RASPI4_PCIE_MSI_INTR_CLR (RASPI4_PCIE_ECAM_BASE + 0x4508)
#define RASPI4_PCIE_MSI_INTR_MASK_SET (RASPI4_PCIE_ECAM_BASE + 0x4510)
#define RASPI4_PCIE_MSI_INTR_MASK_CLR (RASPI4_PCIE_ECAM_BASE + 0x4514)
#define RASPI4_PCIE_MSI_DATA_CONFIG_VALUE 0xffe06540
#define BCM2711_GICD_ISPENDR5 0xff841214
#define BCM2711_GICD_PCI_MSI_BIT BIT(20)
#define RASPI4_NVME_BAR RASPI4_PCIE_MMIO_BASE
#define RASPI4_VL805_USBCMD (RASPI4_PCIE_MMIO_BASE + 0x40)
#define RASPI4_NVME_ADMIN_SQ 0x200000
#define RASPI4_NVME_ADMIN_CQ 0x210000
#define RASPI4_NVME_IO_SQ 0x220000
#define RASPI4_NVME_IO_CQ 0x230000
#define RASPI4_NVME_DATA 0x240000
#define BCM2711_DWC2_BASE 0xfe980000
#define DWC2_QOM_PATH "/machine/soc/peripherals/dwc2"
#define DWC2_HOST_DMA 0x280000
#define BCM2711_EMMC_BASE 0xfe300000
#define BCM2711_EMMC2_BASE 0xfe340000
#define BCM2711_UART0_BASE 0xfe201000
#define PL011_DR 0x00
#define PL011_FR 0x18
#define PL011_LCRH 0x2c
#define PL011_CR 0x30
#define PL011_FR_RXFE BIT(4)
#define BCM2711_MMC_IRQ_OR_QOM_PATH \
    "/machine/soc/peripherals/mmc_irq_orgate"


typedef struct Fat16Builder {
    uint8_t *image;
    uint16_t next_cluster;
    unsigned directory_index;
} Fat16Builder;

typedef struct FatVariantBuilder {
    uint8_t *image;
    size_t image_size;
    uint32_t partition_sectors;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t root_entries;
    uint32_t root_sectors;
    uint32_t data_start_sector;
    uint32_t root_cluster;
    uint32_t next_cluster;
    unsigned int sectors_per_cluster;
    unsigned int fat_type;
    unsigned int directory_index;
} FatVariantBuilder;

typedef struct FdtLookupCellOffsets {
    size_t local;
    size_t external;
} FdtLookupCellOffsets;

typedef enum TestOverlayExportVariant {
    TEST_EXPORT_VALID,
    TEST_EXPORT_NONEMPTY,
    TEST_EXPORT_MISSING_SYMBOL,
    TEST_EXPORT_COLLISION,
    TEST_EXPORT_INTRA_CYCLE,
} TestOverlayExportVariant;

typedef enum TestDuplicateEepromFile {
    TEST_EEPROM_DUPLICATE_NONE,
    TEST_EEPROM_DUPLICATE_BOOTCONF,
    TEST_EEPROM_DUPLICATE_SIGNATURE,
    TEST_EEPROM_DUPLICATE_PUBLIC_KEY,
} TestDuplicateEepromFile;

typedef enum TestBootsysFault {
    TEST_BOOTSYS_VALID,
    TEST_BOOTSYS_TRUST_MISSING,
    TEST_BOOTSYS_WRONG_TRUST,
    TEST_BOOTSYS_MISSING,
    TEST_BOOTSYS_FORMAT,
    TEST_DEPENDENCY_MISSING,
    TEST_DEPENDENCY_FORMAT,
    TEST_DEPENDENCY_HEADER_CHECKSUM,
    TEST_DEPENDENCY_HASH,
} TestBootsysFault;

typedef struct TestTlsTransport {
    GByteArray *rx;
    size_t rx_offset;
    GByteArray *tx;
    bool saw_fin;
    bool saw_syn;
} TestTlsTransport;

#define BCM2711_GPIO_BASE 0xfe200000
#define BCM2711_AVS_BASE 0xfd5d2000
#define BCM2711_AVS_TEMP_STATUS 0x200
#define BCM2711_AVS_TEMP_VALID (BIT(16) | BIT(10))
#define BCM2711_GENET_BASE 0xfd580000
#define GENET_SYS_REV_CTRL 0x0000
#define GENET_INTRL2_0 0x0200
#define GENET_INTRL2_1 0x0240
#define GENET_INTRL2_CPU_STAT 0x00
#define GENET_INTRL2_CPU_SET 0x04
#define GENET_INTRL2_CPU_CLEAR 0x08
#define GENET_INTRL2_CPU_MASK_STATUS 0x0c
#define GENET_INTRL2_CPU_MASK_CLEAR 0x14
#define GENET_UMAC_CMD 0x0808
#define GENET_UMAC_MDIO_CMD 0x0e14
#define GENET_UMAC_MIB_RX_START 0x0c00
#define GENET_UMAC_MIB_RX_PKT 0x0c28
#define GENET_UMAC_MIB_RX_BYTES 0x0c2c
#define GENET_UMAC_MIB_RX_MULTICAST 0x0c30
#define GENET_UMAC_MIB_RX_BROADCAST 0x0c34
#define GENET_UMAC_MIB_RX_GOOD_PKT 0x0c64
#define GENET_UMAC_MIB_RX_UNICAST 0x0c68
#define GENET_UMAC_MIB_TX_START 0x0c80
#define GENET_UMAC_MIB_TX_PKT 0x0ca8
#define GENET_UMAC_MIB_TX_BYTES 0x0ce8
#define GENET_UMAC_MIB_TX_GOOD_PKT 0x0cec
#define GENET_UMAC_MIB_TX_UNICAST 0x0cf0
#define GENET_UMAC_MIB_CTRL 0x0d80
#define GENET_UMAC_MDF_ERR_CNT 0x0e38
#define GENET_UMAC_MDF_CTRL 0x0e50
#define GENET_UMAC_MDF_ADDR 0x0e54
#define GENET_MIB_RESET_RX BIT(0)
#define GENET_MIB_RESET_TX BIT(2)
#define GENET_UMAC_CMD_PROMISC BIT(4)
#define GENET_MDIO_START_BUSY BIT(29)
#define GENET_MDIO_READ_FAIL BIT(28)
#define GENET_MDIO_RD (2U << 26)
#define GENET_MDIO_PHY_SHIFT 21
#define GENET_MDIO_REG_SHIFT 16
#define GENET_IRQ_MDIO_DONE BIT(23)
#define GENET_IRQ_MDIO_ERROR BIT(24)
#define GENET_IRQ_LINK_UP BIT(4)
#define GENET_IRQ_LINK_DOWN BIT(5)
#define GENET_IRQ_TBUF_UNDERRUN BIT(8)
#define GENET_IRQ_RBUF_OVERFLOW BIT(9)
#define GENET_EXT_RGMII_OOB_CTRL 0x008c
#define GENET_RGMII_LINK BIT(4)
#define GENET_RBUF_OVFL_CNT 0x0394
#define GENET_RBUF_ERR_CNT 0x0398
#define GENET_RDMA_DESC 0x2000
#define GENET_TDMA_DESC 0x4000
#define GENET_RDMA_REG 0x2c00
#define GENET_TDMA_REG 0x4c00
#define GENET_DMA_COMMON 0x440
#define GENET_DMA_CTRL 0x04
#define GENET_DMA_RING_CONS_INDEX 0x08
#define GENET_DMA_RING_PROD_INDEX 0x0c
#define GENET_RDMA_RING_PROD_INDEX 0x08
#define GENET_DMA_RING_BUF_SIZE 0x10
#define GENET_DMA_RING_START 0x14
#define GENET_DMA_SOP BIT(13)
#define GENET_DMA_EOP BIT(14)
#define GENET_DMA_TX_UNDERRUN BIT(9)
#define GENET_DMA_RX_PREFIX_SIZE 66
#define GENET_IRQ1_TX_RING0 BIT(0)
#define GENET_IRQ1_RX_RING0 BIT(16)
#define BCM2711_PWM_BASE 0xfe20c000
#define BCM2711_PWM1_BASE 0xfe20c800
#define PWM_CTL 0x00
#define PWM_STA 0x04
#define PWM_DMAC 0x08
#define PWM_RNG1 0x10
#define PWM_DAT1 0x14
#define PWM_FIF1 0x18
#define PWM_RNG2 0x20
#define PWM_DAT2 0x24
#define PWM_STA_FULL1 BIT(0)
#define PWM_STA_EMPT1 BIT(1)
#define PWM_STA_WERR1 BIT(2)
#define PWM_STA_RERR1 BIT(3)
#define PWM_STA_GAPO1 BIT(4)
#define PWM_STA_GAPO2 BIT(5)
#define PWM_STA_STA1 BIT(9)
#define PWM_STA_STA2 BIT(10)
#define PWM_CTL_MODE1 BIT(1)
#define PWM_CTL_RPTL1 BIT(2)
#define PWM_CTL_SBIT1 BIT(3)
#define PWM_CTL_POLA1 BIT(4)
#define PWM_CTL_USEF1 BIT(5)
#define PWM_CTL_MSEN1 BIT(7)
#define PWM_CTL_PWEN2 BIT(8)
#define PWM_CTL_USEF2 BIT(13)
#define PWM_CTL_MSEN2 BIT(15)
#define PWM_DMAC_ENABLE BIT(31)
#define PWM_QOM_PATH "/machine/soc/peripherals/pwm"
#define PWM1_QOM_PATH "/machine/soc/peripherals/pwm1"
#define BCM2711_DMA_BASE 0xfe007000
#define DMA_CS 0x00
#define DMA_CB_ADDR 0x04
#define DMA_CS_ACTIVE BIT(0)
#define DMA_CS_END BIT(1)
#define DMA_CS_INT BIT(2)
#define DMA_CS_DREQ BIT(3)
#define DMA_CS_ISHELD BIT(5)
#define DMA_CS_PRIORITY(value) ((value) << 16)
#define DMA_CS_PANIC_PRIORITY(value) ((value) << 20)
#define DMA_TI_INT_EN BIT(0)
#define DMA_TI_TDMODE BIT(1)
#define DMA_TI_D_DREQ BIT(6)
#define DMA_TI_D_INC BIT(4)
#define DMA_TI_S_INC BIT(8)
#define DMA_TI_S_DREQ BIT(10)
#define DMA_PERMAP_PWM 5
#define DMA_PERMAP_PWM1 1
#define DMA_PERMAP_SPI_TX 6
#define DMA_PERMAP_SPI_RX 7
#define DMA_TI_PERMAP_PWM (DMA_PERMAP_PWM << 16)
#define DMA_TI_PERMAP_PWM1 (DMA_PERMAP_PWM1 << 16)
#define DMA_PWM_FIFO_BUS 0x7e20c018
#define DMA_PWM1_FIFO_BUS 0x7e20c818
#define BCM2711_SPI_BASE 0xfe204000
#define BCM2711_BSC0_BASE 0xfe205000
#define BCM2711_BSC1_BASE 0xfe804000
#define BCM2711_BSC2_BASE 0xfe805000
#define BCM2711_BSC0_QOM_PATH \
    "/machine/soc/peripherals/bcm2835-i2c0"
#define BCM2711_BSC1_QOM_PATH \
    "/machine/soc/peripherals/bcm2835-i2c1"
#define BCM2711_SDHOST_BASE 0xfe202000
#define SDHOST_CMD 0x00
#define SDHOST_TIMEOUT 0x08
#define SDHOST_CDIV 0x0c
#define SDHOST_STATUS 0x20
#define SDHOST_VDD 0x30
#define SDHOST_EDM 0x34
#define SDHOST_DATA 0x40
#define SDHOST_CMD_NEW BIT(15)
#define SDHOST_CMD_FAIL BIT(14)
#define SDHOST_STATUS_CMD_TIMEOUT BIT(6)
#define SDHOST_STATUS_FIFO_ERROR BIT(3)
#define SDHOST_FIFO_WORDS 16
#define DMA_SPI_FIFO_BUS 0x7e204004
#define SPI_CS 0x00
#define SPI_FIFO 0x04
#define SPI_CLK 0x08
#define SPI_DLEN 0x0c
#define SPI_DC 0x14
#define SPI_CS_DONE BIT(16)
#define SPI_CS_RXD BIT(17)
#define SPI_CS_TXD BIT(18)
#define SPI_CS_CSPOL1 BIT(22)
#define SPI_CS_INTD BIT(9)
#define SPI_CS_DMAEN BIT(8)
#define SPI_CS_TA BIT(7)
#define SPI_CS_ADCS BIT(11)
#define SPI_QOM_PATH "/machine/soc/peripherals/bcm2835-spi0"
#define BCM2711_AUX_BASE 0xfe215000
#define BCM2711_CPRMAN_BASE 0xfe101000
#define CPRMAN_VPU_CTL 0x08
#define CPRMAN_VPU_DIV 0x0c
#define CPRMAN_PWM_CTL 0xa0
#define CPRMAN_PWM_DIV 0xa4
#define CPRMAN_PASSWORD 0x5a000000
#define AUX_IRQ 0x00
#define AUX_ENABLES 0x04
#define AUX_MU_IO 0x40
#define AUX_MU_IER 0x44
#define AUX_MU_IIR 0x48
#define AUX_MU_LCR 0x4c
#define AUX_MU_MCR 0x50
#define AUX_MU_LSR 0x54
#define AUX_MU_MSR 0x58
#define AUX_MU_SCRATCH 0x5c
#define AUX_MU_CNTL 0x60
#define AUX_MU_STAT 0x64
#define AUX_MU_BAUD 0x68
#define AUX_MU_STAT_RX_LEVEL_SHIFT 16
#define AUX_MU_STAT_TX_LEVEL_SHIFT 24
#define AUX_QOM_PATH "/machine/soc/peripherals/aux"
#define AUX_SPI1_BASE 0x80
#define AUX_SPI2_BASE 0xc0
#define AUX_SPI_CNTL0 0x00
#define AUX_SPI_CNTL1 0x04
#define AUX_SPI_STAT 0x08
#define AUX_SPI_PEEK 0x0c
#define AUX_SPI_IO 0x20
#define AUX_SPI_TXHOLD 0x30
#define AUX_SPI_CNTL0_VAR_WIDTH BIT(14)
#define AUX_SPI_CNTL0_ENABLE BIT(11)
#define AUX_SPI_CNTL0_CLEARFIFO BIT(9)
#define AUX_SPI_CNTL0_MSBF_OUT BIT(6)
#define AUX_SPI_CNTL1_TXEMPTY_IRQ BIT(7)
#define AUX_SPI_CNTL1_IDLE_IRQ BIT(6)
#define AUX_SPI_CNTL1_MSBF_IN BIT(1)
#define AUX_SPI_STAT_TX_EMPTY BIT(9)
#define AUX_SPI_STAT_RX_EMPTY BIT(7)
#define AUX_SPI_STAT_BUSY BIT(6)
#define GPIO_GPFSEL0 0x00
#define GPIO_GPFSEL1 0x04
#define GPIO_GPFSEL2 0x08
#define GPIO_GPFSEL4 0x10
#define GPIO_GPFSEL5 0x14
#define GPIO_GPSET0 0x1c
#define GPIO_GPSET1 0x20
#define GPIO_GPCLR0 0x28
#define GPIO_GPLEV0 0x34
#define GPIO_GPLEV1 0x38
#define GPIO_GPEDS0 0x40
#define GPIO_GPEDS1 0x44
#define GPIO_GPREN0 0x4c
#define GPIO_GPREN1 0x50
#define GPIO_GPFEN0 0x58
#define GPIO_GPFEN1 0x5c
#define GPIO_GPHEN0 0x64
#define GPIO_GPLEN0 0x70
#define GPIO_GPAREN0 0x7c
#define GPIO_PULL0 0xe4
#define GPIO_PULL1 0xe8
#define GPIO_QOM_PATH "/machine/soc/peripherals/gpio"
#define GPIO_BRIDGE_LINE_SIZE 96
#define PROPERTY_QOM_PATH "/machine/soc/peripherals/property"
#define FRAMEBUFFER_QOM_PATH "/machine/soc/peripherals/fb"
#define PROPERTY_BUFFER_ADDRESS 0x100000
#define BCM2711_MAILBOX_READ 0xfe00b880
#define BCM2711_MAILBOX_STATUS 0xfe00b898
#define BCM2711_MAILBOX_WRITE 0xfe00b8a0
#define BCM2711_MAILBOX_EMPTY BIT(30)
#define PROPERTY_CHANNEL 8
#define BCM2711_HDMI0_I2C_BASE 0xfef04500
#define BCM2711_HDMI0_AUTO_I2C_BASE 0xfef00b00
#define BCM2711_HDMI1_I2C_BASE 0xfef09500
#define BCM2711_HDMI1_AUTO_I2C_BASE 0xfef05b00
#define BCM2711_AON_INTR_BASE 0xfef00100
#define BCM2711_HDMI0_BASE 0xfef00700
#define BCM2711_HDMI1_BASE 0xfef05700
#define BCM2711_HDMI0_CEC_BASE 0xfef04300
#define BCM2711_HDMI1_CEC_BASE 0xfef09300
#define BCM2711_HDMI_HOTPLUG 0x1a8
#define BCM2711_CEC_CNTRL_1 0x10
#define BCM2711_CEC_CNTRL_5 0x20
#define BCM2711_CEC_TX_DATA_1 0x28
#define BCM2711_CEC_RX_DATA_1 0x38
#define BCM2711_CEC_TX_EOM BIT(31)
#define BCM2711_CEC_TX_STATUS_GOOD BIT(30)
#define BCM2711_CEC_RX_EOM BIT(29)
#define BCM2711_CEC_RX_STATUS_GOOD BIT(28)
#define BCM2711_CEC_REC_WRD_CNT_SHIFT 24
#define BCM2711_CEC_CLEAR_RECEIVE_OFF BIT(21)
#define BCM2711_CEC_START_XMIT_BEGIN BIT(20)
#define BCM2711_CEC_MESSAGE_LENGTH_SHIFT 16
#define BCM2711_CEC_TX_SW_RESET BIT(27)
#define BCM2711_CEC_RX_SW_RESET BIT(26)
#define BCM2711_CEC_RX_CEC_INT BIT(23)
#define BCM2711_AON_STATUS 0x00
#define BCM2711_AON_CLEAR 0x08
#define BCM2711_AON_MASK_STATUS 0x0c
#define BCM2711_AON_MASK_SET 0x10
#define BCM2711_AON_MASK_CLEAR 0x14
#define HDMI_I2C_CHIP_ADDRESS 0x00
#define HDMI_I2C_DATA_IN 0x04
#define HDMI_I2C_COUNT 0x24
#define HDMI_I2C_CONTROL 0x28
#define HDMI_I2C_ENABLE 0x2c
#define HDMI_I2C_DATA_OUT 0x30
#define HDMI_I2C_CONTROL_HIGH 0x50
#define HDMI_I2C_AUTO_CONTROL0 0x26c
#define HDMI_I2C_INTERRUPT BIT(1)
#define HDMI_I2C_NOACK BIT(2)
#define HDMI_I2C_READ 1
#define TEST_BOOTSYS_PAYLOAD_SIZE 128
#define TEST_BOOTSYS_RSA_SIZE 256
#define TEST_BOOTSYS_HMAC_SIZE 20
#define TEST_BOOTSYS_SECTION_SIZE \
    QEMU_ALIGN_UP(8 + TEST_BOOTSYS_PAYLOAD_SIZE + sizeof(uint32_t) * 2 + \
                  TEST_BOOTSYS_RSA_SIZE + TEST_BOOTSYS_HMAC_SIZE, 8)
#define TEST_DEPENDENCY_MAGIC 0x55aaf44f
#define TEST_DEPENDENCY_NAME_SIZE 16
#define TEST_DEPENDENCY_HASH_SIZE 32
#define TEST_DEPENDENCY_FRAME_SIZE 44
#define TEST_BOOTSYS_SHA256 \
    "0d8b498ef08ab3f9628e42ebb8bbcdcfd8ff9eb542b777c4c16fe1c9a0939d54"
#define TEST_BOOTSYS_DEVKEY_SHA256 \
    "81e8df6a1ed5f13cb671f467a6ee24f5821c1a2cc72da6c66d4a94c08fe0b782"
#define TEST_DEPENDENCIES_SHA256 \
    "8c7592e4959b8beb4c9f6b4378101394689592601552977350d453ca0129ee45"
#define TEST_BOOTSYS_MACHINE_OPTION \
    ",bootsys-trusted-sha256=" TEST_BOOTSYS_SHA256
#define TEST_BOOTSYS_WRONG_MACHINE_OPTION \
    ",bootsys-trusted-sha256=" \
    "0000000000000000000000000000000000000000000000000000000000000000"
#define TEST_BOOTSYS_DEVKEY_MACHINE_OPTION \
    ",bootsys-trusted-sha256=" TEST_BOOTSYS_DEVKEY_SHA256
#define TEST_RECOVERY_PAYLOAD_SIZE 4
#define TEST_RECOVERY_RSA_SIZE 256
#define TEST_RECOVERY_HMAC_SIZE 20
#define TEST_RECOVERY_SIZE \
    (TEST_RECOVERY_PAYLOAD_SIZE + sizeof(uint32_t) * 2 + \
     TEST_RECOVERY_RSA_SIZE + TEST_RECOVERY_HMAC_SIZE)

/* Shared between the raspi4 qtest translation units. */

char *qom_get_string(QTestState *qts, const char *property);
uint32_t qom_get_uint32(QTestState *qts, const char *property);
uint32_t qom_path_get_uint32(QTestState *qts, const char *path,
                                    const char *property);
uint64_t qom_get_uint64(QTestState *qts, const char *property);
int32_t qom_get_int32(QTestState *qts, const char *property);
bool qom_get_bool(QTestState *qts, const char *property);
uint64_t qom_path_get_uint64(QTestState *qts, const char *path,
                                    const char *property);
void qom_path_set_uint64(QTestState *qts, const char *path,
                                const char *property, uint64_t value);
void qom_path_set_uint32(QTestState *qts, const char *path,
                                const char *property, uint32_t value);
size_t raspi4_dwc2_endpoint_transfer(QTestState *qts, uint8_t address,
                                            uint8_t endpoint, uint8_t type,
                                            uint16_t max_packet, bool in,
                                            uint32_t pid, void *buffer,
                                            size_t length);
void raspi4_dwc2_reset_root_port(QTestState *qts);
void raspi4_dwc2_get_device_descriptor(QTestState *qts,
                                               uint8_t address,
                                               uint8_t descriptor[18]);
size_t raspi4_dwc2_control(QTestState *qts, uint8_t address,
                                  uint8_t request_type, uint8_t request,
                                  uint16_t value, uint16_t index,
                                  uint8_t *data, uint16_t length);
void raspi4_dwc2_set_address(QTestState *qts, uint8_t old_address,
                                    uint8_t new_address);
void raspi4_dwc2_bot_command(QTestState *qts, uint8_t address,
                                    uint8_t lun, uint32_t tag,
                                    const uint8_t *cdb, uint8_t cdb_length,
                                    uint8_t *data, size_t data_length,
                                    bool require_success);
void wait_for_migration_complete(QTestState *qts);
QTestState *migrate_to_new_qtest_commands(
    QTestState *source, const char *destination_base,
    char **directory_out, char **socket_path_out);
QTestState *migrate_to_new_qtest(QTestState *source,
                                        const char *command,
                                        char **directory_out,
                                        char **socket_path_out);
void assert_bus_card_type(QTestState *qts, const char *bus_path,
                                 const char *card_type);
int gpio_bridge_connect(const char *path);
void gpio_bridge_write(int fd, const char *data);
char *gpio_bridge_read(int fd);
void dwc2_transport_write_all(int fd, const void *data, size_t length);
void dwc2_transport_read_all(int fd, void *data, size_t length);
int32_t dwc2_transport_exchange(int fd, uint8_t opcode, uint8_t ep,
                                       uint32_t value, const void *payload,
                                       uint32_t length, void *reply,
                                       uint32_t reply_capacity);
void qom_set_uint32(QTestState *qts, const char *property,
                           uint32_t value);
void qom_set_int32(QTestState *qts, const char *property,
                          int32_t value);
void qom_set_bool(QTestState *qts, const char *property, bool value);
void make_test_public_key(uint8_t public_key[264]);
uint8_t *make_eeprom_image(const char *config, bool corrupt);
uint8_t *make_otp_image(uint32_t bootmode, uint32_t bootmode_copy,
                               uint32_t board_revision, bool customer_key);
void assert_bootloader_reserved_blob(
    QTestState *qts, const char *compatible,
    const uint8_t *expected, size_t expected_size);
void assert_bootloader_device_tree(QTestState *qts,
                                          uint32_t boot_mode,
                                          uint32_t partition,
                                          uint32_t tryboot);
uint32_t firmware_bootloader_u32(QTestState *qts,
                                       const char *property);
void assert_bootloader_build_identity(QTestState *qts, bool valid,
                                             uint32_t timestamp,
                                             const char *version);
void assert_bootloader_update_timestamp(QTestState *qts, bool valid,
                                               uint32_t timestamp);
void assert_bootloader_usb_identity(QTestState *qts, bool valid,
                                           uint32_t version,
                                           uint32_t route_string,
                                           uint32_t root_hub_port,
                                           uint32_t lun);
void assert_firmware_system_identity(QTestState *qts,
                                            uint32_t expected_revision,
                                            uint32_t expected_serial);
uint64_t firmware_kaslr_seed(QTestState *qts);
uint32_t firmware_min_boot_version(QTestState *qts);
uint32_t firmware_sdram_size_gbit(QTestState *qts);
uint32_t firmware_board_revision_ext(QTestState *qts);
void assert_firmware_chosen_string(QTestState *qts,
                                          const char *name,
                                          const char *expected);
void write_temp_image(const char *template, const uint8_t *image,
                             size_t size, char **path_out);
void write_temp_text(const char *template, const char *text,
                            char **path_out);
void finalize_edid_block(uint8_t edid[128]);
void make_edid(uint8_t edid[128], const char manufacturer[3],
                      const char *product);
void make_timed_edid(uint8_t edid[128],
                            const char manufacturer[3],
                            uint16_t pixel_clock_10khz,
                            uint16_t hactive, uint16_t hblank,
                            uint16_t hfront, uint16_t hsync,
                            uint16_t vactive, uint16_t vblank,
                            uint16_t vfront, uint16_t vsync);
void overwrite_image(const char *path, const uint8_t *image,
                            size_t size);
QTestState *start_with_eeprom(const char *config, bool corrupt,
                                     char **path_out);
QTestState *start_with_eeprom_and_sd_ram(const char *config,
                                                const char firmware_name[11],
                                                const uint8_t *firmware,
                                                size_t firmware_size,
                                                const char *sd_config,
                                                bool gpt,
                                                unsigned int ram_gib,
                                                uint32_t size_cells,
                                                char **eeprom_path_out,
                                                char **sd_path_out);
QTestState *start_with_eeprom_and_sd(const char *config,
                                            const char firmware_name[11],
                                            const uint8_t *firmware,
                                            size_t firmware_size,
                                            const char *sd_config,
                                            char **eeprom_path_out,
                                            char **sd_path_out);
uint8_t *make_usb_boot_image(const char firmware_name[11],
                                    const uint8_t *firmware,
                                    size_t firmware_size,
                                    const char *media_config);
QTestState *start_with_eeprom_and_media_serial(
    bool network, const char *media_config, const char *serial_path,
    char **eeprom_path_out, char **media_path_out);
QTestState *start_with_eeprom_and_network_config(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    const char *media_config,
    char **eeprom_path_out, char **network_path_out);
QTestState *start_arm32_memory_model_with_otp(
    const char *machine, unsigned int ram_gib, const char *bootconf,
    const char *extra_config,
    const uint8_t *initramfs, size_t initramfs_size,
    const char *otp_path, uint32_t min_boot_version,
    const char *extra_options,
    char **eeprom_path_out, char **media_path_out);
QTestState *start_arm32_memory_model(
    const char *machine, unsigned int ram_gib, const char *extra_config,
    const uint8_t *initramfs, size_t initramfs_size,
    char **eeprom_path_out, char **media_path_out);
QTestState *start_with_complex_firmware_config_variant(
    const char *extra_config, TestOverlayExportVariant export_variant,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    bool wireless_model,
    char **eeprom_path_out, char **sd_path_out, char **hat_path_out);
QTestState *start_with_complex_firmware_config(
    const char *extra_config, char **eeprom_path_out, char **sd_path_out);
QTestState *start_recovery_stage_extra(
    bool bad_signature, bool write_protect, uint64_t fail_after,
    const char *fail_stage, bool permanent, bool fragmented,
    const char *eeprom_debug_path, const char *machine_extra,
    const char *recovery_config, const char *extra_drive,
    char **eeprom_path_out, char **sd_path_out, uint8_t **expected_out);
void assert_file_equals(const char *path, const uint8_t *expected,
                               size_t expected_size);
void raspi4_sdhci_command(QTestState *qts, uint64_t base,
                                 uint16_t block_size, uint16_t block_count,
                                 uint32_t argument, uint16_t transfer_mode,
                                 uint16_t command);
uint32_t cyw_sdio_cmd52_argument(bool write, bool raw,
                                        uint8_t function, uint32_t address,
                                        uint8_t value);
uint32_t cyw_sdio_cmd53_argument(bool write, uint8_t function,
                                        bool increment, uint32_t address,
                                        uint16_t count);
void cyw_sdio_set_backplane_window(QTestState *qts, uint32_t address);
uint32_t cyw_sdio_backplane_readl(QTestState *qts, uint32_t address);
void cyw_sdio_backplane_writel(QTestState *qts, uint32_t address,
                                      uint32_t value);
void cyw_sdio_init_card(QTestState *qts);
void cyw_sdio_start_firmware(QTestState *qts);
void cyw_sdio_send_guest_packet(QTestState *qts,
                                       const uint8_t *packet, size_t size,
                                       uint8_t sequence);
void cyw_sdio_read_guest_packet(QTestState *qts, uint8_t *packet,
                                       size_t size);
void raspi4_hci_uart_write(QTestState *qts, const uint8_t *data,
                                  size_t size);
void raspi4_hci_uart_read(QTestState *qts, uint8_t *data, size_t size);
void raspi4_hci_uart_configure(QTestState *qts);
void raspi4_sdhci_initialize_card(QTestState *qts);
void raspi4_sdhci_initialize_emmc(QTestState *qts);
void raspi4_emmc_switch(QTestState *qts, uint8_t index, uint8_t value);
void raspi4_emmc_write_sector(QTestState *qts, uint32_t address,
                                     uint32_t value);
uint32_t raspi4_emmc_read_word(QTestState *qts, uint32_t address);
void raspi4_emmc_read_ext_csd(QTestState *qts, uint8_t ext_csd[512]);
uint32_t raspi4_emmc_read_cache_size_kib(QTestState *qts);
uint32_t raspi4_emmc_cache_dirty_sectors(QTestState *qts);
bool raspi4_emmc_cache_flush_active(QTestState *qts);
uint64_t raspi4_emmc_cache_flush_completed(QTestState *qts);
uint32_t raspi4_emmc_program_pending(QTestState *qts);
bool raspi4_emmc_program_active(QTestState *qts);
uint64_t raspi4_emmc_program_completed(QTestState *qts);
bool raspi4_emmc_erase_active(QTestState *qts);
uint64_t raspi4_emmc_erase_pending(QTestState *qts);
uint64_t raspi4_emmc_erase_completed(QTestState *qts);
void raspi4_emmc_erase(QTestState *qts, uint32_t start_address,
                              uint32_t end_address);
void raspi4_emmc_write_multiple(QTestState *qts, uint32_t address,
                                       const uint32_t *values,
                                       unsigned int count, bool reliable);
uint32_t raspi4_emmc_status(QTestState *qts);
uint32_t rng200_xorshift32(uint32_t value);
uint32_t bcm2711_genet_mdio_read(QTestState *qts, unsigned int phy,
                                        unsigned int reg);
void genet_socket_read_all(int fd, void *buffer, size_t length);
void genet_socket_send_packet(int fd, const uint8_t *packet,
                                     size_t length);
void genet_wait_register(QTestState *qts, uint64_t address,
                                uint32_t expected);
void bcm2711_property_call_response_bytes(
    QTestState *qts, uint32_t tag, uint32_t *payload, size_t words,
    size_t response_bytes);
void bcm2711_property_call_response(QTestState *qts, uint32_t tag,
                                           uint32_t *payload, size_t words,
                                           size_t response_words);
void bcm2711_property_call(QTestState *qts, uint32_t tag,
                                  uint32_t *payload, size_t words);
void bcm2711_property_raw_exchange(QTestState *qts, uint8_t *request,
                                           size_t size);
uint32_t bcm2711_firmware_edid_call(
    QTestState *qts, uint32_t tag, uint32_t block, uint32_t port,
    uint8_t edid[128]);
bool bcm2711_hdmi_i2c_transfer(QTestState *qts, uint64_t base,
                                      uint8_t address, bool read,
                                      uint8_t *data, size_t length);
void bcm2711_hdmi_i2c_set_pointer(QTestState *qts, uint64_t base,
                                         uint8_t segment, uint8_t offset);
void bcm2711_hdmi_i2c_read(QTestState *qts, uint64_t base,
                                  uint8_t segment, uint8_t offset,
                                  uint8_t *data, size_t length);
void make_firmware_display_timing(
    uint8_t timing[36], uint8_t display_id, uint32_t clock,
    uint16_t hdisplay, uint16_t hfront, uint16_t hsync, uint16_t htotal,
    uint16_t vdisplay, uint16_t vfront, uint16_t vsync, uint16_t vtotal,
    uint32_t flags);
void bcm2711_firmware_display_timing_call(QTestState *qts,
                                                 uint32_t tag,
                                                 uint8_t timing[36]);
void bcm2711_firmware_display_timing_get(QTestState *qts,
                                                uint8_t display_id,
                                                uint8_t timing[36]);
uint32_t bcm2711_get_throttled(QTestState *qts);
uint32_t bcm2711_get_firmware_temperature(QTestState *qts,
                                                  uint32_t tag);
uint32_t bcm2711_firmware_clock_call(QTestState *qts, uint32_t tag,
                                             uint32_t clock_id,
                                             uint32_t value);
uint32_t bcm2711_firmware_power_call(QTestState *qts, uint32_t tag,
                                             uint32_t device_id,
                                             uint32_t value);
uint32_t bcm2711_firmware_framebuffer_scalar(
    QTestState *qts, uint32_t tag, uint32_t value);
int64_t bcm2711_firmware_vsync_submit(QTestState *qts,
                                             uint32_t refresh_hz);
void bcm2711_firmware_vsync_receive(QTestState *qts);
uint32_t bcm2711_firmware_display_power(
    QTestState *qts, uint32_t display_id, uint32_t state);
void assert_bcm2711_framebuffer_surface(
    QTestState *qts, const char *path, uint32_t width, uint32_t height,
    const uint8_t *expected);
void bcm2711_firmware_overscan_call(QTestState *qts, uint32_t tag,
                                            uint32_t values[4]);
void assert_bcm2711_framebuffer_pixel(
    QTestState *qts, const char *path, const uint8_t expected[3]);
void configure_pwm_clock(QTestState *qts);
void program_dma_paced_word(QTestState *qts, unsigned channel,
                                    uint32_t cb_addr, uint32_t source_addr,
                                    uint32_t dest_addr, unsigned permap,
                                    unsigned priority,
                                    unsigned panic_priority,
                                    uint32_t sample);
uint32_t wait_for_dwc2_channel(QTestState *qts,
                                      unsigned int channel);
void assert_firmware_memory_nodes(QTestState *qts,
                                         uint64_t lower_size,
                                         uint64_t upper_size);
void assert_no_firmware_initramfs(QTestState *qts);
void test_cyw43455_sdio_transport(void);
void test_cyw43455_sdio_erom(void);
void test_cyw43455_sdio_core_wrappers(void);
void test_cyw43455_sdio_firmware_start(void);
void test_cyw43455_sdio_bcdc_control(void);
void test_cyw43455_sdio_bcdc_packets(void);
void test_cyw43455_sdio_packet_migration(void);
void test_cyw43455_sdio_packet_loss_recovery(void);
void test_cyw43455_bluetooth_hci(void);
void test_cyw43455_sdio_onboard_topology(void);
void test_cyw43455_sdio_final_dtb(void);
void test_cyw43455_sdio_exact_firmware_download(void);
void test_cyw43455_sdio_fault_reset(void);
void test_sd_active_write_removal_migration(void);
void test_emmc_power_cut_sector_durability_migration(void);
void test_emmc_volatile_cache_power_cut_flush_migration(void);
void test_emmc_timed_cache_flush_power_cut_migration(void);
void test_emmc_timed_cache_flush_all_cut_points(void);
void test_emmc_timed_cache_flush_error_retry(void);
void test_emmc_timed_program_all_cut_points(void);
void test_emmc_timed_reliable_program_migration(void);
void test_emmc_timed_program_error_retry(void);
void test_emmc_timed_erase_all_cut_points(void);
void test_emmc_timed_erase_cache_migration(void);
void test_emmc_timed_erase_error_retry(void);
void test_emmc_erase_sync_cache_invalidation(void);
void test_emmc_mixed_timed_reset_migration_campaign(void);
void test_emmc_reliable_write_cache_bypass_migration(void);
void test_emmc_reliable_write_flush_error_retry(void);
void test_bcm2711_rng200_registers(void);
void test_bcm2711_rng200_migration(void);
void test_bcm2711_genet_registers(void);
void test_bcm2711_genet_packets(void);
void test_bcm2711_genet_dma_faults(void);
void test_bcm2711_thermal_registers(void);
void test_bcm2711_firmware_mailbox_structure(void);
void test_bcm2711_firmware_identity(void);
void test_bcm2711_firmware_edid(void);
void test_bcm2711_hdmi_i2c_edid(void);
void test_bcm2711_hdmi_hotplug(void);
void test_bcm2711_hdmi_cec(void);
void test_bcm2711_firmware_display_timing(void);
void test_bcm2711_firmware_power(void);
void test_bcm2711_firmware_framebuffer_transaction(void);
void test_bcm2711_firmware_framebuffer_display_control(void);
void test_bcm2711_firmware_framebuffer_displays(void);
void test_bcm2711_firmware_framebuffer_transform(void);
void test_bcm2711_firmware_framebuffer_cursor(void);
void test_bcm2711_firmware_overscan(void);
void test_bcm2711_firmware_xhci_reset(void);
void test_bcm2711_firmware_clock(void);
void test_bcm2711_firmware_temperature(void);
void test_bcm2711_firmware_throttled(void);
void test_bcm2711_firmware_framebuffer(void);
void test_bcm2711_firmware_framebuffer_release(void);
void test_bcm2711_firmware_expander_gpio(void);
void test_bcm2711_pwm_registers(void);
void test_bcm2711_pwm_clocked_fifo(void);
void test_bcm2711_pwm_clock_migration(void);
void test_bcm2711_pwm_waveform_gpio_migration(void);
void test_bcm2711_pwm_dma_migration(void);
void test_bcm2711_pwm1_gpio_dma_migration(void);
void test_bcm2711_pwm_dual_fifo_lockstep_migration(void);
void test_bcm2711_pwm_dma_panic_priority_migration(void);
void test_bcm2711_spi_dma_migration(void);
void test_bcm2711_aux_uart_registers(void);
void test_bcm2711_aux_uart_core_clock(void);
void test_bcm2711_aux_uart_migration(void);
void test_bcm2711_aux_spi_migration(void);
void test_bcm2711_dwc2_reset_commands(void);
void test_bcm2711_dwc2_device_registers(void);
void test_bcm2711_dwc2_device_transport(void);
void test_bcm2711_dwc2_device_pio_fifo(void);
void test_bcm2711_dwc2_device_pio_reset_storm(void);
void test_bcm2711_dwc2_device_suspend_migration(void);
void test_bcm2711_dwc2_host_pio_fifo(void);
void test_bcm2711_dwc2_host_pio_async_reset_storm(void);
void test_bcm2711_gpio_external_inputs(void);
void test_bcm2711_gpio_events(void);
void test_bcm2711_gpio_chardev_bridge(void);
void test_bcm2711_gpio_chardev_migration(void);
void test_eeprom_nwp_gpio_chardev(void);
void test_bcm2711_legacy_sdhost_registers(void);
void test_bcm2711_i2c_controllers(void);
void test_bcm2711_i2c_ten_bit_migration(void);
void test_bcm2711_i2c_clock_stretch_timeout(void);
void test_pi4_memory_models(void);
void test_pi4_memory_model_device_tree(void);
void test_imager_repo_url_handoff(void);
void test_firmware_total_mem_models(void);
void test_firmware_total_mem_include_boundary(void);
void test_firmware_gpu_mem_models(void);
void test_firmware_bootcode_delay_sd_reset(void);
void test_firmware_bootcode_delay_network(void);
void test_firmware_bootcode_delay_include_boundary(void);
void test_firmware_bootcode_delay_invalid(void);
void test_firmware_bootcode_delay_migration(void);
void test_eeprom_config_txt_append(void);
void test_firmware_sdram_frequency_models(void);
void test_firmware_sdram_frequency_network(void);
void test_firmware_sdram_frequency_include_boundary(void);
void test_firmware_sdram_frequency_invalid(void);
void test_boot_uart_sd_migration_boundaries(void);
void test_cm4_vl805_eeprom_policy(void);
void test_firmware_uart_2ndstage_sd_reset_migration(void);
void test_firmware_uart_2ndstage_network(void);
void test_firmware_uart_2ndstage_boundaries(void);
void test_arm32_memory_model_device_tree(void);
void test_bcm2711_pcie_platform(void);
void test_bcm2711_pcie_migration(void);

void raspi4_assert_nvme_rw_statuses(QTestState *qts, uint8_t opcode,
                                           const uint8_t *dirty_data,
                                           const uint8_t *first_data,
                                           const uint8_t *second_data,
                                           uint16_t first_status,
                                           uint16_t second_status);
size_t dwc2_rpiboot_standard_control(int fd, const uint8_t setup[8],
                                            void *response,
                                            size_t response_capacity);
void dwc2_rpiboot_configure(int fd, uint8_t address);
void qom_set_uint64(QTestState *qts, const char *property,
                           uint64_t value);
void qom_set_string(QTestState *qts, const char *property,
                           const char *value);
uint8_t *make_test_recovery_sized(size_t payload_size,
                                         size_t *recovery_size);
uint8_t *make_test_recovery(void);
void test_decode_hex(const char *text, uint8_t *output,
                            size_t output_size);
uint8_t *make_secure_otp_image(const uint8_t key_hash[32]);
uint8_t *make_arm64_kernel(void);
uint8_t *make_arm32_kernel(void);
uint32_t fdt_get_u32(const void *fdt, int node, const char *property);
void assert_bootloader_boot_mode(QTestState *qts, uint32_t boot_mode);
uint8_t *make_device_tree(size_t *size);
uint16_t test_hat_crc16(const uint8_t *data, size_t size);
uint8_t *make_hat_overlay(size_t *size);
uint8_t *make_hat_eeprom(const uint8_t *dt, size_t dt_size,
                                size_t *size);
void fat16_init(Fat16Builder *builder);
void fat16_add_file(Fat16Builder *builder, const char name[11],
                           const uint8_t *contents, size_t size);
uint16_t fat16_add_fragmented_file(Fat16Builder *builder,
                                          const char name[11],
                                          const uint8_t *contents,
                                          size_t size);
void fat_variant_set_entry(FatVariantBuilder *builder,
                                  uint32_t cluster, uint32_t value);
void fat_variant_init(FatVariantBuilder *builder,
                             unsigned int fat_type);
uint32_t fat_variant_add_file(FatVariantBuilder *builder,
                                     const char name[11],
                                     const uint8_t *contents, size_t size,
                                     bool fragmented);
void fat16_add_long_file(Fat16Builder *builder, const char *long_name,
                                const char short_name[11],
                                const uint8_t *contents, size_t size);
uint8_t *make_secure_inner_boot_image(size_t *image_size);
char *make_secure_boot_signature(void);
char *make_secure_nvme_boot_signature(void);
char *make_dns_secure_boot_signature(void);
char *make_custom_kernel_boot_command(
    const char *machine, const uint8_t *kernel, size_t kernel_size,
    const char *media_config,
    const uint8_t *cmdline, size_t cmdline_size,
    const uint8_t *initramfs, size_t initramfs_size,
    char **eeprom_path_out, char **media_path_out);
QTestState *start_with_eeprom_and_usb(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    char **eeprom_path_out, char **usb_path_out);
uint8_t *make_usb_boot_image_with_update(
    const char firmware_name[11], const uint8_t *firmware,
    size_t firmware_size, const char *media_config,
    const uint8_t *eeprom_update, const char *eeprom_signature);
uint8_t *make_tryboot_boot_image(const uint8_t *normal_start,
                                        size_t normal_start_size,
                                        const uint8_t *try_start,
                                        size_t try_start_size,
                                        const char *config_override);
uint8_t *make_ab_boot_image(const uint8_t *partition2,
                                   const uint8_t *partition3);
uint8_t *make_logical_boot_image(const uint8_t *partition5,
                                        const uint8_t *partition6);
QTestState *start_with_eeprom_and_usb_controller(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    const char *controller, char **eeprom_path_out, char **usb_path_out);
QTestState *start_with_eeprom_and_network(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    char **eeprom_path_out, char **network_path_out);
QTestState *start_cm4_with_eeprom_and_emmc(
    const char *config, char **eeprom_path_out, char **emmc_path_out);
QTestState *start_recovery_stage(bool bad_signature,
                                        bool write_protect,
                                        uint64_t fail_after,
                                        const char *fail_stage,
                                        bool permanent,
                                        bool fragmented,
                                        const char *eeprom_debug_path,
                                        char **eeprom_path_out,
                                        char **sd_path_out,
                                        uint8_t **expected_out);
QTestState *start_recovery(bool bad_signature, bool write_protect,
                                  uint64_t fail_after, bool permanent,
                                  char **eeprom_path_out, char **sd_path_out,
                                  uint8_t **expected_out);
QTestState *start_with_otp(uint32_t bootmode,
                                  uint32_t bootmode_copy,
                                  uint32_t board_revision,
                                  bool customer_key,
                                  unsigned int rpiboot_gpio,
                                  bool nrpiboot,
                                  char **path_out);
void assert_logical_mbr_rejected(const uint8_t *sd,
                                        const char *eeprom_config);
uint8_t *test_read_handoff_dtb(QTestState *qts);
uint8_t *make_signed_eeprom(bool omit_signature, bool tamper_config,
                                   bool tamper_signature,
                                   unsigned int network_mode,
                                   TestDuplicateEepromFile duplicate_file);
QTestState *start_secure_provision_recovery(
    const char *provision_config, const uint8_t *initial_otp,
    bool attach_otp, bool trust_bootsys, uint8_t otp_fail_after,
    char **eeprom_path_out, char **otp_path_out,
    char **sd_path_out, uint8_t **signed_eeprom_out);
uint8_t *make_signed_nvme_eeprom(void);
uint8_t *make_dns_signed_http_eeprom(void);
void raspi4_trigger_guest_halt(QTestState *qts);
size_t genet_socket_read_packet(int fd, uint8_t *packet,
                                       size_t capacity);
uint16_t test_ip_checksum(const uint8_t *data, size_t length);
size_t test_make_tcp_reply(uint8_t *reply, size_t capacity,
                                  const uint8_t *request, uint32_t sequence,
                                  uint32_t acknowledgement, uint8_t flags,
                                  const uint8_t *payload,
                                  size_t payload_size);
const uint8_t *test_dhcp_option(const uint8_t *packet, size_t size,
                                       uint8_t wanted, uint8_t *value_length);
uint8_t test_dhcp_message_type(const uint8_t *packet, size_t size);
size_t test_make_dhcp_reply(uint8_t *reply, size_t capacity,
                                   const uint8_t *request,
                                   uint8_t message_type);
void test_set_dhcp_server_addresses(uint8_t *packet, size_t size,
                                           uint32_t dhcp_server,
                                           uint32_t tftp_server);
size_t test_add_overloaded_string_option(uint8_t *packet,
                                                size_t capacity,
                                                uint8_t code,
                                                const char *value);
size_t test_add_overloaded_tftp_server_name(uint8_t *packet,
                                                    size_t capacity,
                                                    const char *name);
void test_set_dhcp_bootfile(uint8_t *packet, size_t size,
                                   const char value[7]);
size_t test_add_dhcp_ipv4_option(uint8_t *packet, size_t capacity,
                                        uint8_t code, uint32_t address);
size_t test_add_dhcp_bytes_option(uint8_t *packet, size_t capacity,
                                         uint8_t code, const uint8_t *value,
                                         uint8_t value_length);
size_t test_add_conflicting_overloaded_server(uint8_t *packet,
                                                      size_t capacity,
                                                      uint32_t server);
size_t test_make_arp_reply(uint8_t reply[60],
                                  const uint8_t request[60]);
size_t test_make_dns_reply(uint8_t *reply, size_t capacity,
                                  const uint8_t *request, size_t request_size,
                                  uint32_t address);
size_t test_make_tftp_data(uint8_t *reply, size_t capacity,
                                  const uint8_t *request, uint16_t block,
                                  const uint8_t *data, size_t data_size);
void test_assert_tftp_rrq(const uint8_t *packet,
                                  const char *filename,
                                  bool options);
size_t test_make_tftp_error(uint8_t *reply, size_t capacity,
                                   const uint8_t *request, uint16_t code,
                                   const char *message);
size_t test_make_tftp_not_found(uint8_t *reply, size_t capacity,
                                       const uint8_t *request);
void test_reject_optional_self_update(int socket_fd, uint8_t *client,
                                             size_t capacity);
unsigned int test_transfer_tftp_files(
    int socket_fd, uint8_t *client, size_t capacity,
    const char * const *filenames, const uint8_t * const *files,
    const size_t *sizes, size_t file_count);
void test_transfer_http_file(QTestState *qts, int socket_fd,
                                    uint8_t *client,
                                    size_t client_capacity,
                                    const char *filename,
                                    const char *host,
                                    const uint8_t *contents, size_t size,
                                    uint32_t server_sequence,
                                    bool drop_syn, bool drop_get,
                                    bool reorder_response,
                                    bool fin_response);
ssize_t test_tls_pull(gnutls_transport_ptr_t opaque,
                             void *buf, size_t size);
ssize_t test_tls_push(gnutls_transport_ptr_t opaque,
                             const void *buf, size_t size);
void test_tls_feed_client_packet(
    int socket_fd, TestTlsTransport *transport, uint8_t *client,
    size_t client_size, uint8_t *server, size_t server_capacity,
    uint32_t *server_sequence, uint32_t *client_sequence);
void test_tls_send_server_flight(
    int socket_fd, TestTlsTransport *transport, uint8_t *client,
    size_t client_capacity, uint8_t *server, size_t server_capacity,
    uint32_t *server_sequence, uint32_t *client_sequence);
bool test_tls_drain_client(
    int socket_fd, TestTlsTransport *transport, uint8_t *client,
    size_t client_capacity, uint8_t *server, size_t server_capacity,
    uint32_t *server_sequence, uint32_t *client_sequence);
void test_secure_http_error_case(const uint8_t *response,
                                        size_t response_size,
                                        uint8_t response_flags,
                                        const char *expected_status);
unsigned int test_complete_response_tftp(
    QTestState *qts, int socket_fd, uint8_t *client, size_t capacity,
    const uint8_t *start, size_t start_size, uint32_t expected_server_ip,
    bool finish_dally, bool begin_at_config);
void test_transfer_response_tftp_tail(int socket_fd, uint8_t *client,
                                             size_t capacity);
void run_cm4_rpiboot_dwc2_enumeration(bool secure_provision,
                                              bool trust_bootcode);

void test_cm4_nrpiboot_is_dedicated(void);
void test_cm4_continued_guest_flash_completion(void);
void test_cm4_nrpiboot_gpio40_sampling(void);
void test_cm4_persistent_emmc_boot(void);
void test_cm4_separate_emmc_partitions(void);
void test_hat_eeprom_overlays(void);
void test_usb_mass_storage_boot(void);
void test_usb_msd_exclude_vid_pid(void);
void test_usb_bot_reset_recovery(void);
void test_usb_boot_controller_mode_split(void);
void test_usb_external_mass_storage_boot(void);
void test_usb_mass_storage_multiple_devices(void);
void test_usb_controller_topology_identity(void);
void test_usb_controller_read_error_fallback(void);
void test_nvme_boot(void);
void test_nvme_present_failure_fallback(void);
void test_nvme_command_status_fault_count(void);
void test_nvme_write_status_and_durability(void);
void test_nvme_flush_status_fault_count(void);
void test_nvme_compare_status(void);
void test_boot_filesystem_self_update(void);
void test_boot_filesystem_self_update_policy(void);
void test_boot_filesystem_self_update_failures(void);
void test_boot_filesystem_self_update_usb_nvme(void);
void test_usb_mass_storage_timeouts(void);
void test_usb_power_off_timing_and_board_revisions(void);
void test_usb_mass_storage_hotplug(void);
void test_sd_card_hotplug(void);
void test_sd_retry_hotplug(void);
void test_sd_detect_migration(void);
void test_usb_timeout_migration(void);
void test_usb_power_off_migration(void);
void test_usb_lun_timeout_migration(void);
void test_sd_overcurrent_recovery_and_policy(void);
void test_sd_overcurrent_migration(void);
void test_sd_recovery_update(void);
void test_sd_recovery_executable_validation(void);
void test_sd_recovery_safeguards(void);
void test_sd_recovery_write_protect_contract(void);
void test_sd_recovery_nor_semantics(void);
void test_sd_recovery_fragmented_and_cyclic_files(void);
void test_sd_recovery_fat12_fat32_fragmented_and_malformed(void);
void test_sd_recovery_interrupted_resume(void);
void test_sd_recovery_stage_faults(void);
void test_sd_recovery_timed_flash_power_cut_migration(void);
void test_sd_recovery_verify_mismatch_retry_migration(void);
void test_sd_recovery_backend_io_error_retry(void);
void test_sd_recovery_permanent_file(void);
void test_sd_secure_otp_provisioning(void);
void test_sd_secure_otp_provisioning_fail_closed(void);
void test_sd_secure_otp_provisioning_power_loss(void);
void test_cm4_rpiboot_dwc2_enumeration(void);
void test_cm4_rpiboot_untrusted_bootcode(void);
void test_cm4_rpiboot_secure_provisioning(void);
void test_cm4_rpiboot_active_migration(void);

#endif /* TESTS_QTEST_RASPI4_BOOT_TEST_H */
