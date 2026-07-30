/*
 * Raspberry Pi 4 Model B and Compute Module 4 machine internals
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RASPI4B_INTERNAL_H
#define HW_ARM_RASPI4B_INTERNAL_H

#include "qemu/osdep.h"
#include "monitor/qdev.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/raspi_boot.h"
#include "hw/arm/raspi_firmware.h"
#include "hw/arm/raspi_handoff.h"
#include "hw/arm/raspi_overlay.h"
#include "hw/arm/raspi_platform.h"
#include "hw/arm/raspi_secure.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci-host/gpex.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_device.h"
#include "hw/usb/usb.h"
#include "hw/usb/msd.h"
#include "hw/usb/dwc2-regs.h"
#include "hw/usb/hcd-xhci-pci.h"
#include "hw/usb/xhci.h"
#include "hw/scsi/scsi.h"
#include "hw/sd/sd.h"
#include "qemu/error-report.h"
#include "system/device_tree.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/arm/boot.h"
#include "qom/object.h"
#include "hw/arm/bcm2838.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "crypto/tlssession.h"
#include "crypto/tlscredsx509.h"
#include "crypto/hash.h"
#include "net/checksum.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "target/arm/arm-powerctl.h"
#include <libfdt.h>


#define TYPE_RASPI4B_MACHINE MACHINE_TYPE_NAME("raspi4b")
#define TYPE_RASPI_CM4_MACHINE MACHINE_TYPE_NAME("raspi-cm4")
#define TYPE_RASPI4_VL805 "raspi4-vl805"
#define TYPE_RASPI4_PCIE_ROOT_PORT "raspi4-pcie-root-port"
#define RASPI4_PCIE_VENDOR_ID 0x14e4
#define RASPI4_PCIE_DEVICE_ID 0x2711
#define RASPI4_PCIE_REVISION 0x20
OBJECT_DECLARE_SIMPLE_TYPE(Raspi4bMachineState, RASPI4B_MACHINE)

#define RASPI4_USB_BOOT_MAX_DEVICES 8
#define RASPI4_EDID_DISCONNECTED 0
#define RASPI4_EDID_VALID 1
#define RASPI4_EDID_INVALID 2

typedef struct Raspi4UsbBootNotifier {
    Notifier notifier;
    Raspi4bMachineState *machine;
} Raspi4UsbBootNotifier;

#define RASPI4_EEPROM_SIZE       (512 * KiB)
#define RASPI4_EEPROM_STATUS_SIZE 512
#define RASPI4_EEPROM_STATUS_WP_OFFSET 0
#define RASPI4_EEPROM_STATUS_UPDATE_VALID_OFFSET 1
#define RASPI4_EEPROM_STATUS_UPDATE_TIMESTAMP_OFFSET 2
#define RASPI4_EEPROM_STATUS_METADATA_SIZE 6
#define RASPI4_CAPABILITIES_2020_TIMESTAMP 1607685317
#define RASPI4_CAPABILITIES_2021_TIMESTAMP 1625568293
#define RASPI4_CAPABILITIES_2020 0x1f
#define RASPI4_CAPABILITIES_2021 0x7f
#define RASPI4_EEPROM_MAGIC      0x55aaf00f
#define RASPI4_EEPROM_MAGIC_MASK 0xfffff00f
#define RASPI4_EEPROM_FILE_MAGIC 0x55aaf11f
#define RASPI4_EEPROM_FILE_HDR   24
#define RASPI4_EEPROM_NAME_LEN   12
#define RASPI4_UART0_DR          0x00
#define RASPI4_UART0_IBRD        0x24
#define RASPI4_UART0_FBRD        0x28
#define RASPI4_UART0_LCRH        0x2c
#define RASPI4_UART0_CR          0x30
#define RASPI4_EEPROM_DEPENDENCY_MAGIC 0x55aaf44f
#define RASPI4_EEPROM_DEPENDENCY_NAME_LEN 16
#define RASPI4_EEPROM_DEPENDENCY_MAX 32
#define RASPI4_EEPROM_DEPENDENCY_HASH_SIZE 32
#define RASPI4_EEPROM_DEPENDENCY_MAX_UNCOMPRESSED (16 * MiB)
#define RASPI4_LZ4_FRAME_MAGIC 0x184d2204
#define RASPI4_BOOTSYS_RSA_SIZE  256
#define RASPI4_BOOTSYS_HMAC_SIZE 20
#define RASPI4_BOOTSYS_TRAILER_SIZE \
    (sizeof(uint32_t) * 2 + RASPI4_BOOTSYS_RSA_SIZE + \
     RASPI4_BOOTSYS_HMAC_SIZE)
#define RASPI4_DEFAULT_BOOT_ORDER 0xf41
#define RASPI4_USB_MSD_DISCOVER_TIMEOUT 20000
#define RASPI4_USB_MSD_DISCOVER_TIMEOUT_MIN 5000
#define RASPI4_USB_MSD_LUN_TIMEOUT 2000
#define RASPI4_USB_MSD_LUN_TIMEOUT_MIN 100
#define RASPI4_USB_MSD_STARTUP_DELAY 0
#define RASPI4_USB_MSD_STARTUP_DELAY_MAX 30000
#define RASPI4_USB_MSD_PWR_OFF_TIME 1000
#define RASPI4_USB_MSD_PWR_OFF_TIME_MAX 5000
#define RASPI4_USB_MSD_NEW_BOARD_MIN_OFF_MS 2000
#define RASPI4_USB_MSD_EXCLUDE_MAX 4
#define RASPI4_BOOT_WATCHDOG_PARTITION_MAX 63
#define RASPI4_SD_OVERCURRENT_RETRY_MS 5000
#define RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED 1U
#define RASPI4_SD_QUIRK_CLOCK_LIMIT_HZ 12500000U
#define RASPI4_FATAL_ERROR_PATTERN_COUNT 3
#define RASPI4_FATAL_ERROR_REBOOT_MS \
    (RASPI4_FATAL_ERROR_PATTERN_COUNT * 1000)
#define RASPI4_HDMI_DELAY_DEFAULT 5
#define RASPI4_NETCONSOLE_MAX 32
#define RASPI4_NETCONSOLE_SOURCE_PORT 6665
#define RASPI4_NETCONSOLE_DESTINATION_PORT 6666
#define RASPI4_USB_BOT_MAX_RECOVERIES 8
#define RASPI4_USB_BOT_CASE_COUNT 13
#define RASPI4_RPIBOOT_EP0_DMA    0x00100000
#define RASPI4_RPIBOOT_EP1_DMA    0x00110000
#define RASPI4_USB_BOOT_DMA       0x00120000
#define RASPI4_XHCI_BOOT_DMA      0x00140000
#define RASPI4_RPIBOOT_BOOT_MESSAGE_SIZE 24
#define RASPI4_RPIBOOT_MAX_BOOTCODE_SIZE (128 * MiB)
#define RASPI4_RPIBOOT_MAX_FILE_SIZE (512 * MiB)
#define RASPI4_VIDEOCORE_BOUNDARY_VERSION 1
#define RASPI4_BOOTLOADER_SIGNED_CONFIG BIT(0)
#define RASPI4_BOOTLOADER_SIGNED_DEVKEY_REVOKED BIT(2)
#define RASPI4_BOOTLOADER_SIGNED_CUSTOMER_KEY BIT(3)
#define RASPI4_PERIPHERAL_POLICY "preserve-disabled-unmodeled-v1"
#define RASPI4_EXCLUDED_DVP "brcm,brcm2711-dvp"
#define RASPI4_EXCLUDED_WIFI "brcm,bcm2835-mmc"
#define RASPI4_EXCLUDED_BLUETOOTH "brcm,bcm43438-bt"
#define RASPI4_PERIPHERAL_EXCLUSIONS \
    RASPI4_EXCLUDED_DVP ";" RASPI4_EXCLUDED_WIFI ";" \
    RASPI4_EXCLUDED_BLUETOOTH

enum Raspi4RpibootPhase {
    RASPI4_RPIBOOT_EXPECT_MESSAGE,
    RASPI4_RPIBOOT_EXPECT_BOOTCODE,
    RASPI4_RPIBOOT_EXPECT_STATUS,
    RASPI4_RPIBOOT_BOOTCODE_READY,
    RASPI4_RPIBOOT_FILE_SEND_SIZE,
    RASPI4_RPIBOOT_FILE_WAIT_SIZE,
    RASPI4_RPIBOOT_FILE_SEND_READ,
    RASPI4_RPIBOOT_FILE_WAIT_DATA,
    RASPI4_RPIBOOT_FILE_SEND_DONE,
    RASPI4_RPIBOOT_COMPLETE,
    RASPI4_RPIBOOT__MAX,
};

enum Raspi4RpibootControlPending {
    RASPI4_RPIBOOT_CONTROL_NONE,
    RASPI4_RPIBOOT_CONTROL_SET_ADDRESS,
    RASPI4_RPIBOOT_CONTROL_SET_CONFIGURATION,
    RASPI4_RPIBOOT_CONTROL_SET_ENDPOINT_HALT,
    RASPI4_RPIBOOT_CONTROL_CLEAR_ENDPOINT_HALT,
    RASPI4_RPIBOOT_CONTROL__MAX,
};
#define RASPI4_DHCP_TIMEOUT 45000
#define RASPI4_DHCP_TIMEOUT_MIN 5000
#define RASPI4_DHCP_REQ_TIMEOUT 4000
#define RASPI4_DHCP_REQ_TIMEOUT_MIN 500
#define RASPI4_TFTP_FILE_TIMEOUT 30000
#define RASPI4_TFTP_FILE_TIMEOUT_MIN 5000
#define RASPI4_DHCP_XID 0x52506934
#define RASPI4_DHCP_OPTION97_DEFAULT 0x34695052
#define RASPI4_PXE_OPTION43_DEFAULT "Raspberry Pi Boot"
#define RASPI4_PXE_OPTION43_MAX UINT8_MAX
#define RASPI4_DHCP_PHASE_NONE 0
#define RASPI4_DHCP_PHASE_DISCOVER 1
#define RASPI4_DHCP_PHASE_REQUEST 2
#define RASPI4_TFTP_CLIENT_PORT 49152
#define RASPI4_DNS_CLIENT_PORT 49153
#define RASPI4_DNS_NAME_MAX 253
#define RASPI4_HTTP_PATH_MAX 255
#define RASPI4_HTTP_DEFAULT_PATH "net_install"
#define RASPI4_HTTP_DEFAULT_HOST "fw-download-alias1.raspberrypi.com"
#define RASPI4_HTTP_DEFAULT_PORT 80
#define RASPI4_HTTPS_DEFAULT_PORT 443
#define RASPI4_HTTP_CLIENT_PORT 49154
#define RASPI4_HTTP_TCP_PAYLOAD_MAX 1400
#define RASPI4_HTTP_HEADER_MAX (8 * KiB)
#define RASPI4_HTTP_OOO_MAX UINT16_MAX
#define RASPI4_TLS_TX_MAX UINT16_MAX
#define RASPI4_TFTP_CLASSIC_BLOCK_SIZE 512
#define RASPI4_TFTP_REQUESTED_BLOCK_SIZE 1024
#define RASPI4_TFTP_PATH_MAX 255
#define RASPI4_TFTP_PREFIX_STR_MAX 32
#define RASPI4_TFTP_PREFIX_MAX 32
#define RASPI4_TFTP_RETRANSMIT_INITIAL_MS 500
#define RASPI4_TFTP_RETRANSMIT_MAX_MS 4000
#define RASPI4_TFTP_DALLY_MS 500
#define RASPI4_NETWORK_ARTIFACTS_MAX 80
#define RASPI4_CONFIG_MAX_SIZE (1 * MiB)
#define RASPI4_RECOVERY_REBOOT_DELAY_MS 10
#define RASPI4_RESTART_WATCHDOG_DELAY_MS 10
#define RASPI4_RECOVERY_MAX_SIZE (4 * MiB)
#define RASPI4_RECOVERY_SIGNED_MAX_SIZE (110 * KiB)
#define RASPI4_RECOVERY_RSA_SIZE 256
#define RASPI4_RECOVERY_HMAC_SIZE 20
#define RASPI4_RECOVERY_TRAILER_SIZE \
    (sizeof(uint32_t) * 2 + RASPI4_RECOVERY_RSA_SIZE + \
     RASPI4_RECOVERY_HMAC_SIZE)
#define RASPI4_PCIE_ECAM_BASE 0xfd500000
#define RASPI4_PCIE_ECAM_SIZE (1 * MiB)
#define RASPI4_PCIE_REG_SIZE 0x9310
#define RASPI4_PCIE_MMIO_BASE 0x600000000ULL
#define RASPI4_PCIE_MMIO_PCI_BASE 0xc0000000
#define RASPI4_PCIE_MMIO_SIZE (1 * GiB)
#define RASPI4_PCIE_MSI_TARGET_LOW 0x0fffffffcULL
#define RASPI4_PCIE_MSI_TARGET_HIGH 0xffffffffcULL
#define RASPI4_PCIE_MSI_BAR_CONFIG_LO 0x4044
#define RASPI4_PCIE_MSI_BAR_CONFIG_HI 0x4048
#define RASPI4_PCIE_MSI_DATA_CONFIG 0x404c
#define RASPI4_PCIE_MSI_INTR_STATUS 0x4500
#define RASPI4_PCIE_MSI_INTR_CLR 0x4508
#define RASPI4_PCIE_MSI_INTR_MASK_SET 0x4510
#define RASPI4_PCIE_MSI_INTR_MASK_CLR 0x4514
#define RASPI4_PCIE_MSI_DATA_MASK 0x1f
#define RASPI4_SIGNATURE_MAX_SIZE (64 * KiB)
#define RASPI4_FIRMWARE_MAX_SIZE  (16 * MiB)
#define RASPI4_FIXUP_MAX_SIZE     (4 * MiB)
#define RASPI4_KERNEL_MAX_SIZE    (256 * MiB)
#define RASPI4_DTB_MAX_SIZE       (4 * MiB)
#define RASPI_HAT_EEPROM_MAX_SIZE (1 * MiB)
#define RASPI4_CMDLINE_MAX_SIZE   (64 * KiB)
#define RASPI4_INITRAMFS_MAX_SIZE (1 * GiB)
#define RASPI_CM4_BOARD_REVISION   0x00b03140
#define RASPI4_REV_MEMORY_SHIFT    20
#define RASPI4_REV_MEMORY_MASK     (0x7U << RASPI4_REV_MEMORY_SHIFT)

#define RASPI4_PM_RSTS_HADDRF     0x00000002
#define RASPI4_PM_RSTS_HADWRF     0x00000020
#define RASPI4_PM_RSTS_HADSRF     0x00000200
#define RASPI4_PM_RSTS_HADPOR     0x00001000

/*
 * Raspberry Pi's default network-install server certificate is signed by
 * this long-lived intermediate.  Keep the trust boundary board-local instead
 * of inheriting the host operating system's CA store.
 *
 * SHA-256 of the DER form:
 * fe1878e7c9ae0f0857c41a39996ad4de0e8d6e846b65ca6bd0ace0a325a732b4
 */
static const uint8_t raspi4_network_install_ca[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC+jCCAn+gAwIBAgICEAAwCgYIKoZIzj0EAwIwgbcxCzAJBgNVBAYTAkdCMRAw\n"
    "DgYDVQQIDAdFbmdsYW5kMRIwEAYDVQQHDAlDYW1icmlkZ2UxHTAbBgNVBAoMFFJh\n"
    "c3BiZXJyeSBQSSBMaW1pdGVkMRwwGgYDVQQLDBNSYXNwYmVycnkgUEkgRUNDIENB\n"
    "MR0wGwYDVQQDDBRSYXNwYmVycnkgUEkgUm9vdCBDQTEmMCQGCSqGSIb3DQEJARYX\n"
    "c3VwcG9ydEByYXNwYmVycnlwaS5jb20wIBcNMjExMjA5MTEzMjU1WhgPMjA3MTEx\n"
    "MjcxMTMyNTVaMIGrMQswCQYDVQQGEwJHQjEQMA4GA1UECAwHRW5nbGFuZDEdMBsG\n"
    "A1UECgwUUmFzcGJlcnJ5IFBJIExpbWl0ZWQxHDAaBgNVBAsME1Jhc3BiZXJyeSBQ\n"
    "SSBFQ0MgQ0ExJTAjBgNVBAMMHFJhc3BiZXJyeSBQSSBJbnRlcm1lZGlhdGUgQ0Ex\n"
    "JjAkBgkqhkiG9w0BCQEWF3N1cHBvcnRAcmFzcGJlcnJ5cGkuY29tMHYwEAYHKoZI\n"
    "zj0CAQYFK4EEACIDYgAEcN9K6Cpv+od3w6yKOnec4EbyHCBzF+X2ldjorc0b2Pq0\n"
    "N+ZvyFHkhFZSgk2qvemsVEWIoPz+K4JSCpgPstz1fEV6WzgjYKfYI71ghELl5TeC\n"
    "byoPY+ee3VZwF1PTy0cco2YwZDAdBgNVHQ4EFgQUJ6YzIqFh4rhQEbmCnEbWmHEo\n"
    "XAUwHwYDVR0jBBgwFoAUIIAVCSiDPXut23NK39LGIyAA7NAwEgYDVR0TAQH/BAgw\n"
    "BgEB/wIBADAOBgNVHQ8BAf8EBAMCAYYwCgYIKoZIzj0EAwIDaQAwZgIxAJYM+wIM\n"
    "PC3wSPqJ1byJKA6D+ZyjKR1aORbiDQVEpDNWRKiQ5QapLg8wbcED0MrRKQIxAKUT\n"
    "v8TJkb/8jC/oBVTmczKlPMkciN+uiaZSXahgYKyYhvKTatCTZb+geSIhc0w/2w==\n"
    "-----END CERTIFICATE-----\n";

typedef enum Raspi4ProgramResult {
    RASPI4_PROGRAM_ERROR,
    RASPI4_PROGRAM_NOR_VIOLATION,
    RASPI4_ERASE_INTERRUPTED,
    RASPI4_PROGRAM_INTERRUPTED,
    RASPI4_VERIFY_INTERRUPTED,
    RASPI4_VERIFY_FAILED,
    RASPI4_PROGRAM_PENDING,
    RASPI4_PROGRAM_OK,
} Raspi4ProgramResult;

typedef enum Raspi4EepromFailStage {
    RASPI4_EEPROM_FAIL_ERASE,
    RASPI4_EEPROM_FAIL_PROGRAM,
    RASPI4_EEPROM_FAIL_VERIFY,
    RASPI4_EEPROM_FAIL_VERIFY_MISMATCH,
    RASPI4_EEPROM_FAIL_RENAME,
    RASPI4_EEPROM_FAIL_REBOOT,
} Raspi4EepromFailStage;

typedef enum Raspi4EepromFlashStage {
    RASPI4_EEPROM_FLASH_IDLE,
    RASPI4_EEPROM_FLASH_ERASE,
    RASPI4_EEPROM_FLASH_PROGRAM,
    RASPI4_EEPROM_FLASH_VERIFY,
    RASPI4_EEPROM_FLASH_COMPLETE,
    RASPI4_EEPROM_FLASH__MAX,
} Raspi4EepromFlashStage;

typedef enum Raspi4BootAttemptResult {
    RASPI4_BOOT_ATTEMPT_FAILED,
    RASPI4_BOOT_ATTEMPT_READY,
    RASPI4_BOOT_ATTEMPT_PENDING,
} Raspi4BootAttemptResult;

typedef enum Raspi4SelfUpdateResult {
    RASPI4_SELF_UPDATE_CONTINUE,
    RASPI4_SELF_UPDATE_PENDING,
    RASPI4_SELF_UPDATE_FAILED,
} Raspi4SelfUpdateResult;

typedef enum Raspi4SelfUpdateStatus {
    RASPI4_SELF_UPDATE_NONE,
    RASPI4_SELF_UPDATE_DISABLED,
    RASPI4_SELF_UPDATE_FROZEN,
    RASPI4_SELF_UPDATE_UNSUPPORTED,
    RASPI4_SELF_UPDATE_UP_TO_DATE,
    RASPI4_SELF_UPDATE_STALE,
    RASPI4_SELF_UPDATE_INVALID,
    RASPI4_SELF_UPDATE_WRITE_PROTECTED,
    RASPI4_SELF_UPDATE_PROGRAM_FAILED,
    RASPI4_SELF_UPDATE_UPDATED_REBOOT,
    RASPI4_SELF_UPDATE_STATUS__MAX,
} Raspi4SelfUpdateStatus;

typedef enum Raspi4PendingBootAction {
    RASPI4_PENDING_NONE,
    RASPI4_PENDING_USB_DISCOVERY,
    RASPI4_PENDING_USB_LUN,
    RASPI4_PENDING_RESTART,
    RASPI4_PENDING_RECOVERY_REBOOT,
    RASPI4_PENDING_RESTART_WATCHDOG,
    RASPI4_PENDING_SD_DETECT,
    RASPI4_PENDING_SD_RETRY,
    RASPI4_PENDING_NETWORK_DHCP,
    RASPI4_PENDING_NETWORK_TFTP,
    RASPI4_PENDING_NETWORK_ARP,
    RASPI4_PENDING_NETWORK_TFTP_DALLY,
    RASPI4_PENDING_NETWORK_DNS,
    RASPI4_PENDING_NETWORK_HTTP,
    RASPI4_PENDING_FIRMWARE_DELAY,
    RASPI4_PENDING_EEPROM_FLASH,
    RASPI4_PENDING_USB_STARTUP,
    RASPI4_PENDING_SD_OVERCURRENT,
    RASPI4_PENDING_USB_POWER_OFF,
    RASPI4_PENDING_FATAL_REBOOT,
    RASPI4_PENDING_NET_INSTALL_KEYBOARD,
    RASPI4_PENDING_NETCONSOLE_LINK,
    RASPI4_PENDING__MAX,
} Raspi4PendingBootAction;

typedef enum Raspi4HttpState {
    RASPI4_HTTP_IDLE,
    RASPI4_HTTP_SYN_SENT,
    RASPI4_HTTP_RESPONSE,
    RASPI4_HTTP__MAX,
} Raspi4HttpState;

typedef struct Raspi4NetworkArtifact {
    uint32_t size;
    uint8_t *data;
    char *filename;
    char *expected_sha256;
    uint32_t expected_size;
    uint32_t maximum_size;
    uint8_t kind;
    bool required;
    bool missing;
    uint8_t wire_filename[RASPI4_TFTP_PATH_MAX + 1];
} Raspi4NetworkArtifact;

typedef enum Raspi4NetworkArtifactKind {
    RASPI4_NETWORK_ARTIFACT_CONFIG,
    RASPI4_NETWORK_ARTIFACT_INCLUDE,
    RASPI4_NETWORK_ARTIFACT_START,
    RASPI4_NETWORK_ARTIFACT_FIXUP,
    RASPI4_NETWORK_ARTIFACT_KERNEL,
    RASPI4_NETWORK_ARTIFACT_DTB,
    RASPI4_NETWORK_ARTIFACT_CMDLINE,
    RASPI4_NETWORK_ARTIFACT_INITRAMFS,
    RASPI4_NETWORK_ARTIFACT_OVERLAY_MAP,
    RASPI4_NETWORK_ARTIFACT_OVERLAY,
    RASPI4_NETWORK_ARTIFACT_SECURE_SIGNATURE,
    RASPI4_NETWORK_ARTIFACT_SECURE_IMAGE,
    RASPI4_NETWORK_ARTIFACT_EEPROM_UPDATE,
    RASPI4_NETWORK_ARTIFACT_EEPROM_SIGNATURE,
    RASPI4_NETWORK_ARTIFACT__MAX,
} Raspi4NetworkArtifactKind;

typedef enum Raspi4NetworkDiscoveryPhase {
    RASPI4_NETWORK_DISCOVERY_NONE,
    RASPI4_NETWORK_DISCOVERY_CONFIG,
    RASPI4_NETWORK_DISCOVERY_BASE,
    RASPI4_NETWORK_DISCOVERY_OVERLAYS,
    RASPI4_NETWORK_DISCOVERY_READY,
    RASPI4_NETWORK_DISCOVERY_SELF_UPDATE,
    RASPI4_NETWORK_DISCOVERY__MAX,
} Raspi4NetworkDiscoveryPhase;

const char *raspi4_network_artifact_filename(
    const Raspi4NetworkArtifact *artifact);
BCM2711GenetState *raspi4_genet(Raspi4bMachineState *s);
void raspi4_network_tftp_fail(Raspi4bMachineState *s,
                                     uint16_t error_code);

typedef enum Raspi4BootHealth {
    RASPI4_HEALTH_NONE,
    RASPI4_HEALTH_KERNEL_STARTED,
    RASPI4_HEALTH_USERSPACE_READY,
    RASPI4_HEALTH_FAILED,
    RASPI4_HEALTH__MAX,
} Raspi4BootHealth;

typedef enum Raspi4MacAddressSource {
    RASPI4_MAC_CONFIGURED,
    RASPI4_MAC_EXPLICIT,
    RASPI4_MAC_CUSTOMER_OTP,
    RASPI4_MAC__MAX,
} Raspi4MacAddressSource;

typedef struct Raspi4BootConfig {
    uint32_t boot_order;
    bool signed_boot;
    bool boot_uart;
    bool vl805;
    bool wake_on_gpio;
    bool power_off_on_halt;
    uint32_t bootvar0;
    uint8_t partition;
    bool partition_walk;
    int32_t max_restarts;
    bool reboot_on_fatal_error;
    int32_t sd_boot_max_retries;
    bool sd_overcurrent_check;
    uint32_t sd_quirks;
    uint32_t usb_msd_discover_timeout;
    uint32_t usb_msd_lun_timeout;
    uint32_t usb_msd_startup_delay;
    uint32_t usb_msd_power_off_time;
    uint32_t usb_msd_exclude_vid_pid[RASPI4_USB_MSD_EXCLUDE_MAX];
    uint8_t usb_msd_exclude_count;
    uint32_t boot_watchdog_timeout;
    uint8_t boot_watchdog_partition;
    int32_t net_boot_max_retries;
    uint32_t dhcp_timeout;
    uint32_t dhcp_req_timeout;
    uint32_t dhcp_option97;
    char pxe_option43[RASPI4_PXE_OPTION43_MAX + 1];
    bool netconsole_enabled;
    char netconsole[RASPI4_NETCONSOLE_MAX + 1];
    uint16_t netconsole_source_port;
    uint16_t netconsole_destination_port;
    uint32_t netconsole_source_ip;
    uint32_t netconsole_destination_ip;
    uint8_t netconsole_destination_mac[6];
    MACAddr mac_address;
    uint8_t mac_address_source;
    bool tftp_ip_set;
    uint32_t tftp_ip;
    uint8_t tftp_prefix;
    char tftp_prefix_str[RASPI4_TFTP_PREFIX_STR_MAX + 1];
    bool client_ip_set;
    uint32_t client_ip;
    bool subnet_set;
    uint32_t subnet;
    bool gateway_set;
    uint32_t gateway;
    uint32_t tftp_file_timeout;
    bool http_host_set;
    char http_host[RASPI4_DNS_NAME_MAX + 1];
    char http_path[RASPI4_HTTP_PATH_MAX + 1];
    uint16_t http_port;
    bool http_cacert_hash_set;
    char http_cacert_hash[65];
    bool disable_hdmi;
    uint32_t hdmi_delay;
    bool net_install_enabled;
    bool net_install_at_power_on;
    uint32_t net_install_keyboard_wait;
    bool enable_self_update;
    bool freeze_version;
} Raspi4BootConfig;

typedef struct Raspi4EepromFiles {
    const uint8_t *bootsys;
    size_t bootsys_size;
    const uint8_t *bootconf;
    size_t bootconf_size;
    const uint8_t *bootconf_signature;
    size_t bootconf_signature_size;
    const uint8_t *public_key;
    size_t public_key_size;
    struct {
        const uint8_t *contents;
        size_t contents_size;
        char name[RASPI4_EEPROM_DEPENDENCY_NAME_LEN + 1];
    } dependencies[RASPI4_EEPROM_DEPENDENCY_MAX];
    unsigned int dependency_count;
} Raspi4EepromFiles;

typedef enum Raspi4OtpProvisionResult {
    RASPI4_OTP_PROVISION_NONE,
    RASPI4_OTP_PROVISION_READY,
    RASPI4_OTP_PROVISION_CONFIG_INVALID,
    RASPI4_OTP_PROVISION_SIGNATURE_INVALID,
    RASPI4_OTP_PROVISION_KEY_MISMATCH,
    RASPI4_OTP_PROVISION_PERSISTENCE_REQUIRED,
    RASPI4_OTP_PROVISION_WRITE_FAILED,
    RASPI4_OTP_PROVISION_JTAG_UNSUPPORTED,
} Raspi4OtpProvisionResult;

typedef enum Raspi4OtpCommitResult {
    RASPI4_OTP_COMMIT_OK,
    RASPI4_OTP_COMMIT_INTERRUPTED,
    RASPI4_OTP_COMMIT_WRITE_FAILED,
} Raspi4OtpCommitResult;

struct Raspi4bMachineState {
    RaspiBaseMachineState parent_obj;
    BCM2838State soc;
    MemoryRegion pcie_registers;
    MemoryRegion pcie_mmio_alias;
    MemoryRegion pcie_msi_doorbell_low;
    MemoryRegion pcie_msi_doorbell_high;
    MemoryRegion *pcie_ecam;
    PCIBus *pcie_bus;
    XHCIState *vl805_xhci;
    qemu_irq pcie_msi_irq;
    uint32_t pcie_ext_cfg_index;
    uint32_t pcie_regs[RASPI4_PCIE_REG_SIZE / sizeof(uint32_t)];
    bool cm4;
    bool soc_initialized;
    bool behavioral_boot;
    bool global_en;
    bool usb_boot_on_xhci;
    bool usb_boot_external;
    bool nrpiboot;
    bool nrpiboot_sampled;
    bool nrpiboot_gpio40_driven;
    bool rpiboot_dwc2_active;
    bool rpiboot_control_in;
    uint8_t rpiboot_control_pending;
    uint8_t rpiboot_control_value;
    uint8_t rpiboot_configuration;
    uint8_t rpiboot_phase;
    uint32_t rpiboot_bulk_expected;
    uint32_t rpiboot_bulk_received;
    uint32_t rpiboot_expected_bootcode;
    uint32_t rpiboot_bootcode_size;
    uint32_t rpiboot_bootcode_alloc;
    bool rpiboot_bootcode_trusted;
    uint8_t rpiboot_file_index;
    uint32_t rpiboot_file_expected;
    uint32_t rpiboot_config_size;
    uint32_t rpiboot_config_alloc;
    uint32_t rpiboot_boot_img_size;
    uint32_t rpiboot_boot_img_alloc;
    bool rpiboot_second_stage;
    uint8_t rpiboot_boot_message[RASPI4_RPIBOOT_BOOT_MESSAGE_SIZE];
    uint8_t *rpiboot_bootcode;
    uint8_t *rpiboot_config;
    uint8_t *rpiboot_boot_img;
    bool provision_recover_stale;
    bool provision_stale_recovered;
    bool provision_rpiboot_owned;
    bool provision_emmc_owned;
    bool sd_recovery_enabled;
    bool eeprom_write_protect;
    bool eeprom_nwp;
    bool eeprom_nwp_sampled;
    bool eeprom_nwp_bridge_driven;
    int32_t thermal_temperature_millicelsius;
    bool thermal_sensor_valid;
    uint32_t firmware_throttled_initial;
    uint64_t eeprom_stuck_zero_offset;
    uint8_t eeprom_stuck_zero_mask;
    bool network_boot_wire;
    bool wireless_model;
    char *wireless_netdev;
    bool net_install_requested;
    bool boot_disable_hdmi;
    uint32_t hdmi_delay;
    bool hdmi_diagnostics_pending;
    bool hdmi_diagnostics_visible;
    uint64_t hdmi_diagnostics_remaining_ns;
    bool netconsole_enabled;
    uint8_t netconsole[RASPI4_NETCONSOLE_MAX + 1];
    uint16_t netconsole_source_port;
    uint16_t netconsole_destination_port;
    uint32_t netconsole_source_ip;
    uint32_t netconsole_destination_ip;
    uint8_t netconsole_destination_mac[6];
    uint64_t netconsole_packets;
    uint64_t netconsole_bytes;
    bool net_install_enabled;
    bool net_install_at_power_on;
    bool net_install_override;
    bool net_install_usb_fallback_consumed;
    bool usb_boot_files_missing;
    uint32_t net_install_keyboard_wait;
    bool net_install_keyboard_present;
    bool net_install_shift_held;
    bool enable_self_update;
    bool freeze_version;
    uint8_t self_update_status;
    char *eeprom_drive;
    char *hat_eeprom_drive;
    char *eeprom_status_drive;
    char *hdmi_edid_file[RASPI_FIRMWARE_MAX_EDIDS];
    uint8_t hdmi_edid_name[RASPI_FIRMWARE_MAX_EDIDS]
                          [RASPI_FIRMWARE_EDID_NAME_MAX];
    uint8_t hdmi_edid_status[RASPI_FIRMWARE_MAX_EDIDS];
    char *usb_boot_drive;
    char *usb_boot_drives;
    char *usb_boot_controller;
    char *network_boot_drive;
    char *nvme_drive;
    char *emmc_drive;
    char *emmc_boot_drive;
    char *emmc_rpmb_drive;
    char *emmc_cid;
    uint8_t emmc_data_error;
    uint64_t emmc_data_error_after;
    uint32_t emmc_data_error_count;
    uint64_t emmc_cache_size;
    bool emmc_cache_power_loss_on_reset;
    uint64_t emmc_cache_flush_sector_delay_us;
    uint64_t emmc_program_sector_delay_us;
    uint64_t emmc_erase_group_delay_us;
    char *otp_drive;
    char *gpio_chardev;
    char *provision_state_file;
    int provision_lock_fd;
    Notifier provision_exit_notifier;
    bool provision_exit_registered;
    BlockBackend *eeprom;
    BlockBackend *hat_eeprom;
    BlockBackend *eeprom_status;
    bool eeprom_status_loaded;
    BlockBackend *usb_boot;
    BlockBackend *usb_boot_devices[RASPI4_USB_BOOT_MAX_DEVICES];
    uint8_t usb_boot_device_count;
    uint8_t usb_boot_group_count;
    uint8_t usb_boot_device_numbers[RASPI4_USB_BOOT_MAX_DEVICES];
    uint8_t usb_boot_luns[RASPI4_USB_BOOT_MAX_DEVICES];
    uint8_t usb_boot_selected_index;
    uint8_t usb_boot_selected_device;
    uint8_t usb_boot_selected_lun;
    bool usb_boot_identity_valid;
    uint8_t usb_boot_version;
    uint32_t usb_boot_route_string;
    uint32_t usb_boot_root_hub_port;
    uint32_t usb_msd_exclude_vid_pid[RASPI4_USB_MSD_EXCLUDE_MAX];
    uint32_t usb_boot_last_excluded_vid_pid;
    uint8_t usb_msd_exclude_count;
    uint8_t usb_boot_excluded_device_count;
    uint8_t usb_boot_eligible_device_count;
    uint64_t usb_boot_controller_reads;
    uint64_t usb_boot_controller_bytes;
    uint64_t usb_boot_controller_failures;
    bool usb_boot_bot_stall_once;
    uint8_t usb_boot_bot_stall_count;
    uint8_t usb_boot_bot_stalls_consumed;
    uint8_t usb_boot_bot_phase_count;
    uint8_t usb_boot_bot_phases_consumed;
    uint8_t usb_boot_bot_case_count;
    uint8_t usb_boot_bot_cases_consumed;
    uint64_t usb_boot_bot_recoveries;
    uint64_t usb_boot_bot_phase_errors;
    uint64_t usb_boot_bot_cases_tested;
    BlockBackend *network_boot;
    BlockBackend *nvme;
    BlockBackend *emmc_boot;
    BlockBackend *emmc_rpmb;
    BlockBackend *otp;
    BlockBackend *sd;
    uint64_t eeprom_fail_after;
    Raspi4EepromFailStage eeprom_fail_stage;
    uint64_t eeprom_erased_bytes;
    uint64_t eeprom_programmed_bytes;
    uint64_t eeprom_verified_bytes;
    uint32_t eeprom_dirty_sector_count;
    uint32_t eeprom_program_page_count;
    uint32_t eeprom_nor_violation_bits;
    uint8_t eeprom_flash_stage;
    uint32_t eeprom_erase_delay_us;
    uint32_t eeprom_program_delay_us;
    uint32_t eeprom_verify_delay_us;
    uint64_t eeprom_flash_elapsed_us;
    uint64_t eeprom_flash_deadline_us;
    uint8_t *eeprom_flash_image;
    uint32_t eeprom_flash_image_size;
    bool eeprom_flash_timing_complete;
    uint8_t otp_rpiboot_gpio;
    uint8_t otp_provision_fail_after;
    uint8_t otp_provision_rows_programmed;
    uint32_t otp_bootmode;
    uint32_t otp_secure_boot_flags;
    uint32_t otp_board_revision;
    uint32_t board_revision;
    uint32_t configured_board_revision;
    uint32_t min_boot_version;
    uint32_t reset_status;
    uint8_t boot_partition;
    uint8_t selected_boot_partition;
    uint8_t eeprom_partition;
    bool partition_walk;
    bool tryboot;
    bool tryboot_a_b;
    bool otp_secure_boot;
    bool secure_public_key_valid;
    uint8_t secure_public_key[RASPI_SECURE_PUBLIC_KEY_SIZE];
    uint32_t boot_order;
    bool boot_uart_enabled;
    bool boot_uart_active;
    bool boot_uart_started;
    uint64_t boot_uart_bytes;
    uint32_t boot_uart_lines;
    bool vl805_enabled;
    bool vl805_initialized;
    uint32_t bootvar0;
    uint8_t eeprom_config_append[RASPI4_EEPROM_SIZE];
    uint32_t eeprom_config_append_size;
    uint8_t *eeprom_bootconf;
    uint32_t eeprom_bootconf_size;
    uint8_t eeprom_public_key[RASPI_SECURE_PUBLIC_KEY_SIZE];
    uint32_t eeprom_public_key_size;
    int32_t max_restarts;
    int32_t sd_boot_max_retries;
    bool sd_overcurrent;
    bool sd_overcurrent_check;
    uint32_t sd_quirks;
    bool sd_overcurrent_warning;
    bool sd_power_enabled;
    uint64_t sd_overcurrent_retry_count;
    uint32_t usb_msd_discover_timeout;
    uint32_t usb_msd_lun_timeout;
    uint32_t usb_msd_startup_delay;
    uint32_t usb_msd_power_off_time;
    bool usb_power_enabled;
    bool usb_power_off_consumed;
    bool usb_power_cycle_legacy;
    uint64_t usb_power_off_elapsed_ms;
    uint32_t boot_watchdog_timeout;
    uint8_t boot_watchdog_partition;
    bool boot_watchdog_armed;
    uint64_t boot_watchdog_remaining_ns;
    bool reboot_on_fatal_error;
    uint64_t fatal_error_reboot_count;
    int32_t net_boot_max_retries;
    uint32_t dhcp_timeout;
    uint32_t dhcp_req_timeout;
    uint32_t dhcp_option97;
    uint8_t pxe_option43[RASPI4_PXE_OPTION43_MAX + 1];
    MACAddr boot_mac_address;
    uint8_t boot_mac_address_source;
    bool tftp_ip_set;
    uint32_t tftp_ip;
    uint8_t tftp_prefix_mode;
    uint8_t tftp_prefix[RASPI4_TFTP_PREFIX_MAX + 1];
    bool network_tftp_prefix_fallback;
    bool client_ip_set;
    uint32_t client_ip;
    bool subnet_set;
    uint32_t subnet;
    bool gateway_set;
    uint32_t gateway;
    uint32_t tftp_file_timeout;
    bool http_host_set;
    uint8_t http_host[RASPI4_DNS_NAME_MAX + 1];
    uint8_t http_path[RASPI4_HTTP_PATH_MAX + 1];
    uint16_t http_port;
    bool http_cacert_hash_set;
    uint8_t http_cacert_hash[65];
    uint32_t network_attempt;
    uint8_t network_dhcp_phase;
    uint32_t network_dhcp_xid;
    uint64_t network_dhcp_retransmit_count;
    uint64_t network_dhcp_retransmit_remaining_ns;
    uint32_t network_offered_ip;
    uint32_t network_dhcp_server_ip;
    uint32_t network_proxy_tftp_ip;
    uint32_t network_server_ip;
    uint32_t network_subnet;
    uint32_t network_gateway;
    uint32_t network_next_hop_ip;
    uint32_t network_dns_server_ip;
    uint16_t network_dns_query_id;
    uint64_t network_dns_retransmit_count;
    bool network_arp_for_dns;
    uint8_t network_tftp_hostname[RASPI4_DNS_NAME_MAX + 1];
    uint8_t network_server_mac[6];
    uint16_t network_tftp_server_port;
    uint16_t network_tftp_next_block;
    uint32_t network_tftp_block_number;
    uint32_t network_tftp_size;
    uint32_t network_tftp_expected_size;
    uint32_t network_tftp_file_retransmits;
    uint64_t network_tftp_retransmit_count;
    uint64_t network_tftp_retransmit_remaining_ns;
    uint16_t network_tftp_dally_server_port;
    uint16_t network_tftp_dally_block;
    uint16_t network_tftp_block_size;
    bool network_tftp_options_disabled;
    bool network_tftp_oack_accepted;
    bool network_tftp_tsize_valid;
    uint32_t network_tftp_tsize;
    uint8_t *network_tftp_data;
    char *network_tftp_filename;
    char *network_tftp_expected_sha256;
    Raspi4NetworkArtifact
        network_artifacts[RASPI4_NETWORK_ARTIFACTS_MAX];
    uint8_t network_artifact_count;
    uint8_t network_artifact_index;
    uint8_t network_base_artifact_index;
    uint8_t network_overlay_artifact_index;
    bool network_response_discovery;
    bool network_prefix_fallback;
    uint8_t network_discovery_phase;
    bool network_http_mode;
    bool network_http_default_host;
    bool network_http_tls;
    uint8_t network_http_state;
    uint16_t network_http_client_port;
    uint32_t network_http_client_seq;
    uint32_t network_http_request_seq;
    uint32_t network_http_server_seq;
    uint32_t network_http_response_size;
    uint32_t network_http_content_length;
    bool network_http_header_parsed;
    uint8_t *network_http_response;
    uint32_t network_http_ooo_sequence;
    uint32_t network_http_ooo_size;
    bool network_http_ooo_fin;
    uint32_t network_http_ooo_fin_sequence;
    uint8_t *network_http_ooo_data;
    uint8_t *network_http_ooo_valid;
    char *http_tls_creds;
    char *bootsys_trusted_sha256;
    char *recovery_trusted_sha256;
    char *rpiboot_bootcode_trusted_sha256;
    uint8_t recovery_sha256[65];
    uint32_t recovery_key_index;
    char bootsys_sha256[65];
    char bootsys_dependencies_sha256[65];
    uint32_t bootsys_dependency_count;
    uint32_t bootsys_key_index;
    QCryptoTLSSession *network_tls_session;
    bool network_tls_handshake_complete;
    uint32_t network_tls_rx_size;
    uint32_t network_tls_rx_offset;
    uint8_t *network_tls_rx;
    uint32_t network_tls_tx_sequence;
    uint32_t network_tls_tx_size;
    uint8_t *network_tls_tx;
    uint64_t boot_attempt_count;
    uint64_t boot_restart_count;
    uint64_t boot_retry_count;
    uint64_t boot_elapsed_ms;
    uint8_t boot_order_index;
    uint8_t selected_boot_mode;
    bool eeprom_build_timestamp_valid;
    uint32_t eeprom_build_timestamp;
    bool eeprom_update_timestamp_valid;
    uint32_t eeprom_update_timestamp;
    bool eeprom_capabilities_valid;
    uint32_t eeprom_capabilities;
    uint8_t eeprom_version[RASPI_FIRMWARE_BOOTLOADER_VERSION_MAX + 1];
    uint32_t bootloader_signed;
    uint8_t boot_health;
    uint32_t secure_image_size;
    uint32_t secure_signature_size;
    uint32_t firmware_size;
    uint32_t fixup_size;
    uint32_t kernel_size;
    uint32_t device_tree_size;
    uint32_t cmdline_size;
    uint32_t initramfs_size;
    RaspiFirmwareConfig firmware_config;
    RaspiFirmwareManifest firmware_manifest;
    bool firmware_config_initialized;
    RaspiArmHandoff handoff;
    RaspiOverlayState overlay_state;
    uint8_t handoff_core_mask;
    char *boot_state;
    char *boot_source;
    char *secure_boot_status;
    char *secure_image_sha256;
    char *secure_signature_sha256;
    char *firmware_file;
    char *firmware_status;
    char *handoff_status;
    char *firmware_sha256;
    char *fixup_sha256;
    char *kernel_sha256;
    char *device_tree_sha256;
    char *cmdline_sha256;
    char *initramfs_sha256;
    char *recovery_status;
    uint8_t recovery_status_id;
    char *reset_cause;
    QEMUTimer *recovery_reboot_timer;
    QEMUTimer *eeprom_flash_timer;
    QEMUTimer *boot_timeout_timer;
    QEMUTimer *network_dhcp_retransmit_timer;
    QEMUTimer *network_tftp_retransmit_timer;
    QEMUTimer *boot_restart_timer;
    QEMUTimer *boot_watchdog_timer;
    QEMUTimer *hdmi_diagnostics_timer;
    bool boot_usb_discovery_wait;
    bool bootcode_delay_consumed;
    uint32_t bootcode_delay_seconds;
    bool firmware_uart_enabled;
    bool firmware_uart_started;
    uint64_t firmware_uart_bytes;
    uint32_t firmware_uart_lines;
    uint8_t pending_boot_action;
    uint64_t pending_boot_remaining_ns;
    bool boot_vmstate_registered;
    Raspi4UsbBootNotifier
        usb_boot_insert_notifiers[RASPI4_USB_BOOT_MAX_DEVICES];
    Raspi4UsbBootNotifier
        usb_boot_remove_notifiers[RASPI4_USB_BOOT_MAX_DEVICES];
    bool usb_boot_notifiers_registered;
    QEMUBH *usb_boot_hotplug_bh;
    bool usb_boot_hotplug_pending;
    Notifier network_boot_insert_notifier;
    Notifier network_boot_remove_notifier;
    bool network_boot_notifiers_registered;
    QEMUBH *network_boot_hotplug_bh;
    bool network_boot_hotplug_pending;
    VMChangeStateEntry *network_boot_vmstate;
    Notifier sd_insert_notifier;
    Notifier sd_remove_notifier;
    bool sd_notifiers_registered;
    QEMUBH *sd_hotplug_bh;
    bool sd_hotplug_pending;
};

/* Types shared between the machine and its subsystem modules. */
typedef struct Raspi4BootFilterState {
    bool model;
    bool serial;
    bool gpio;
    bool expression;
    bool other;
    bool disabled;
} Raspi4BootFilterState;
typedef struct Raspi4AutobootConfig {
    bool present;
    bool valid;
    bool partition_set;
    uint8_t partition;
    bool tryboot_a_b;
} Raspi4AutobootConfig;
typedef struct Raspi4BootMediaReader {
    BlockBackend *blk;
    RaspiBootMediaRead read;
    void *opaque;
    int64_t size;
} Raspi4BootMediaReader;
typedef struct Raspi4UsbBootReader {
    Raspi4bMachineState *machine;
    XHCIState *xhci;
    uint8_t addresses[RASPI4_USB_BOOT_MAX_DEVICES];
    uint16_t max_packets[RASPI4_USB_BOOT_MAX_DEVICES];
    uint8_t max_luns[RASPI4_USB_BOOT_MAX_DEVICES];
    bool group_present[RASPI4_USB_BOOT_MAX_DEVICES];
    uint8_t versions[RASPI4_USB_BOOT_MAX_DEVICES];
    uint32_t route_strings[RASPI4_USB_BOOT_MAX_DEVICES];
    uint32_t root_hub_ports[RASPI4_USB_BOOT_MAX_DEVICES];
    uint8_t group_count;
    uint8_t group;
    uint8_t lun;
    uint32_t tag;
    int64_t size;
    uint8_t *bounce;
} Raspi4UsbBootReader;
typedef struct Raspi4UsbBotCase {
    size_t host_length;
    uint32_t expected_residue;
    bool data_out;
    bool requires_recovery;
} Raspi4UsbBotCase;
typedef struct Raspi4NetworkDiscovery {
    Raspi4bMachineState *machine;
    RaspiFatVolume *volume;
    bool preserve_data;
} Raspi4NetworkDiscovery;
typedef struct Raspi4NetworkReadProbe {
    Raspi4bMachineState *machine;
    char *missing_path;
    size_t missing_maximum;
} Raspi4NetworkReadProbe;
typedef struct Raspi4DhcpOptions {
    uint8_t message;
    uint8_t overload;
    uint32_t server;
    uint32_t tftp_server;
    uint32_t subnet;
    uint32_t gateway;
    uint32_t dns_server;
    char tftp_hostname[RASPI4_DNS_NAME_MAX + 1];
    char bootfile[UINT8_MAX + 1];
    uint8_t pxe_option43[UINT8_MAX];
    uint8_t pxe_option43_length;
    bool message_seen;
    bool overload_seen;
    bool server_seen;
    bool tftp_server_seen;
    bool subnet_seen;
    bool gateway_seen;
    bool dns_server_seen;
    bool tftp_hostname_seen;
    bool bootfile_seen;
    bool pxe_option43_seen;
} Raspi4DhcpOptions;

/* Shared between the machine and its subsystem modules. */
bool raspi4_try_network_firmware(Raspi4bMachineState *s);
void raspi4_set_boot_observation(Raspi4bMachineState *s,
                                 const char *state,
                                 const char *source);
void raspi4_continue_after_eeprom_config(Raspi4bMachineState *s);
void raspi4_execute_after_source_failure(Raspi4bMachineState *s,
                                         uint8_t index);
Raspi4BootAttemptResult raspi4_try_signed_image_with_key(
    Raspi4bMachineState *s,
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t *boot_image, size_t boot_image_size,
    const uint8_t *boot_signature, size_t boot_signature_size,
    const char *media);
BCM2835OTPState *raspi4_otp(Raspi4bMachineState *s);
const char *raspi4_config_path(const Raspi4bMachineState *s);
const char *raspi4_boot_media_name(const Raspi4bMachineState *s);
void raspi4_set_secure_boot_status(Raspi4bMachineState *s,
                                   const char *status);
bool raspi4_ipv4_is_unicast(uint32_t address);
bool raspi4_parse_ipv4(const char *text, uint32_t *value);
void raspi4_set_firmware_status(Raspi4bMachineState *s,
                                const char *status);
void raspi4_set_firmware_filter_inputs(
    Raspi4bMachineState *s, RaspiFirmwareConfig *config);
void raspi4_firmware_uart_begin(Raspi4bMachineState *s);
void raspi4_firmware_uart_manifest(Raspi4bMachineState *s,
                                   const char *source);
void raspi4_apply_firmware_gpu_mem(Raspi4bMachineState *s);
bool raspi4_schedule_bootcode_delay(Raspi4bMachineState *s,
                                    const char *source);
uint8_t *raspi4_read_hat_eeprom(Raspi4bMachineState *s,
                                size_t *size);
Raspi4BootAttemptResult raspi4_finish_firmware(
    Raspi4bMachineState *s, RaspiFatVolume *volume, const char *media,
    const uint8_t *kernel, const uint8_t *device_tree,
    const uint8_t *cmdline, const uint8_t *initramfs,
    RaspiFirmwareReadPath *read_path, void *read_opaque);
void raspi4_observe_network_artifact(
    const Raspi4NetworkArtifact *artifact, uint32_t *size, char **sha256);
unsigned int raspi4_initramfs_file_count(const char *files);
uint8_t *raspi4_concat_network_initramfs(
    const Raspi4bMachineState *s, unsigned int expected,
    uint32_t *size, char **sha256);
void raspi4_schedule_network_wait(Raspi4bMachineState *s, bool dhcp);
bool raspi4_pxe_option43_matches(
    const Raspi4bMachineState *s, const Raspi4DhcpOptions *options);
void raspi4_netconsole_observation(Raspi4bMachineState *s,
                                   const char *state,
                                   const char *source);
const Raspi4NetworkArtifact *raspi4_network_find_artifact(
    const Raspi4bMachineState *s, Raspi4NetworkArtifactKind kind);
void raspi4_network_artifacts_clear(Raspi4bMachineState *s);
bool raspi4_network_prepare_tftp(Raspi4bMachineState *s,
                                 bool preserve_data);
void raspi4_netconsole_begin(Raspi4bMachineState *s);
void raspi4_network_tls_clear(Raspi4bMachineState *s);
bool raspi4_network_static_configured(const Raspi4bMachineState *s);
void raspi4_network_tftp_cancel_retransmit(Raspi4bMachineState *s);
void raspi4_network_tftp_retransmit(void *opaque);
void raspi4_network_dhcp_arm_retransmit(Raspi4bMachineState *s);
void raspi4_network_dhcp_cancel_retransmit(Raspi4bMachineState *s);
void raspi4_network_dhcp_retransmit(void *opaque);
void raspi4_network_boot_tx(void *opaque);
void raspi4_network_vm_state_change(void *opaque, bool running,
                                    RunState state);
bool raspi4_network_begin_tftp(Raspi4bMachineState *s);
bool raspi4_network_begin_http(Raspi4bMachineState *s);
void raspi4_network_complete_artifacts(Raspi4bMachineState *s);
bool raspi4_dns_hostname(const uint8_t *value, size_t length,
                         char output[RASPI4_DNS_NAME_MAX + 1]);
bool raspi4_network_dhcp_receive(void *opaque, const uint8_t *packet,
                                 size_t size);
void raspi4_network_hotplug_bh(void *opaque);
void raspi4_network_media_inserted(Notifier *notifier, void *data);
void raspi4_network_media_removed(Notifier *notifier, void *data);
void raspi4_netconsole_link_changed(void *opaque, bool link_up);

Raspi4SelfUpdateResult
raspi4_apply_self_update(Raspi4bMachineState *s,
                         const uint8_t *update, size_t update_size,
                         const uint8_t *signature, size_t signature_size,
                         const char *media);
Raspi4BootAttemptResult
raspi4_try_firmware(Raspi4bMachineState *s, BlockBackend *blk,
                    const char *media);
Raspi4BootAttemptResult
raspi4_try_network_artifacts(Raspi4bMachineState *s);
Raspi4BootAttemptResult
raspi4_try_sd_firmware(Raspi4bMachineState *s);

void raspi4_rpiboot_dwc2_start(Raspi4bMachineState *s);
bool raspi4_rpiboot_media_read(void *opaque, int64_t offset,
                               int64_t bytes, void *buffer,
                               Error **errp);
bool raspi4_usb_enumerate(Raspi4UsbBootReader *reader, Error **errp);
unsigned int raspi4_usb_bot_case_recovery_count(uint8_t count);
bool raspi4_usb_prepare_reader(Raspi4UsbBootReader *reader,
                               unsigned int group, unsigned int lun,
                               Error **errp);
bool raspi4_usb_media_read(void *opaque, int64_t offset,
                           int64_t bytes, void *buffer,
                           Error **errp);
bool raspi4_usb_media_present(const Raspi4bMachineState *s);
bool raspi4_usb_only_excluded(const Raspi4bMachineState *s);
bool raspi4_usb_legacy_power_cycle(const Raspi4bMachineState *s);
bool raspi4_usb_power_prepare(Raspi4bMachineState *s,
                              const char *source);
bool raspi4_usb_controller_bootable(Raspi4bMachineState *s,
                                    const char *source);
void raspi4_usb_continue_after_power(Raspi4bMachineState *s,
                                     const char *source);
void raspi4_usb_hotplug_bh(void *opaque);
void raspi4_usb_media_inserted(Notifier *notifier, void *data);
void raspi4_usb_media_removed(Notifier *notifier, void *data);
bool raspi4_usb_net_install_fallback(Raspi4bMachineState *s,
                                     uint32_t nibble);
void raspi4b_get_rpiboot_bootcode_size(Object *obj, Visitor *v,
                                       const char *name,
                                       void *opaque, Error **errp);
void raspi4b_get_rpiboot_transfer_received(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque,
                                           Error **errp);
void raspi4b_get_rpiboot_configuration(Object *obj, Visitor *v,
                                       const char *name,
                                       void *opaque, Error **errp);
char *raspi4b_get_rpiboot_bootcode_sha256(Object *obj, Error **errp);
void raspi4b_get_rpiboot_config_size(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp);
char *raspi4b_get_rpiboot_config_sha256(Object *obj, Error **errp);
void raspi4b_get_rpiboot_boot_img_size(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
char *raspi4b_get_rpiboot_boot_img_sha256(Object *obj, Error **errp);
char *raspi4b_get_usb_boot_drive(Object *obj, Error **errp);
void raspi4b_set_usb_boot_drive(Object *obj, const char *value,
                                Error **errp);
char *raspi4b_get_usb_boot_drives(Object *obj, Error **errp);
void raspi4b_set_usb_boot_drives(Object *obj, const char *value,
                                 Error **errp);
char *raspi4b_get_usb_boot_controller(Object *obj, Error **errp);
void raspi4b_set_usb_boot_controller(Object *obj, const char *value,
                                     Error **errp);
bool raspi4b_get_usb_boot_external(Object *obj, Error **errp);
void raspi4b_set_usb_boot_external(Object *obj, bool value,
                                   Error **errp);
bool raspi4b_get_usb_boot_bot_stall_once(Object *obj, Error **errp);
void raspi4b_set_usb_boot_bot_stall_once(Object *obj, bool value,
                                         Error **errp);
void raspi4b_get_usb_boot_bot_stall_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_set_usb_boot_bot_stall_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_bot_phase_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_set_usb_boot_bot_phase_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_bot_case_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_set_usb_boot_bot_case_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
char *raspi4b_get_rpiboot_bootcode_trusted_sha256(Object *obj,
                                                  Error **errp);
char *raspi4b_get_rpiboot_bootcode_trust(Object *obj, Error **errp);
bool raspi4b_get_usb_boot_files_missing(Object *obj, Error **errp);
void raspi4b_get_usb_msd_discover_timeout(Object *obj, Visitor *v,
                                          const char *name,
                                          void *opaque, Error **errp);
void raspi4b_get_usb_msd_lun_timeout(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp);
void raspi4b_get_usb_msd_startup_delay(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
void raspi4b_get_usb_msd_power_off_time(Object *obj, Visitor *v,
                                        const char *name,
                                        void *opaque, Error **errp);
bool raspi4b_get_usb_power_enabled(Object *obj, Error **errp);
bool raspi4b_get_usb_power_off_applicable(Object *obj, Error **errp);
char *raspi4b_get_usb_power_cycle_mode(Object *obj, Error **errp);
void raspi4b_get_usb_power_off_elapsed(Object *obj, Visitor *v,
                                       const char *name,
                                       void *opaque, Error **errp);
void raspi4b_get_usb_power_off_remaining(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp);
void raspi4b_get_usb_boot_selected_index(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp);
void raspi4b_get_usb_boot_selected_device(Object *obj, Visitor *v,
                                          const char *name,
                                          void *opaque, Error **errp);
void raspi4b_get_usb_boot_selected_lun(Object *obj, Visitor *v,
                                       const char *name,
                                       void *opaque, Error **errp);
bool raspi4b_get_usb_boot_identity_valid(Object *obj, Error **errp);
void raspi4b_get_usb_boot_version(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp);
void raspi4b_get_usb_boot_route_string(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
void raspi4b_get_usb_boot_root_hub_port(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp);
char *raspi4b_get_usb_msd_exclude_vid_pid(Object *obj, Error **errp);
void raspi4b_get_usb_boot_excluded_device_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_eligible_device_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_last_excluded_vid_pid(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
char *raspi4b_get_usb_boot_transport(Object *obj, Error **errp);
void raspi4b_get_usb_boot_controller_reads(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_controller_bytes(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_controller_failures(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_bot_recoveries(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_bot_phase_errors(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_usb_boot_bot_cases_tested(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
bool raspi4_usb_boot_uses_xhci(const Raspi4bMachineState *s);
void raspi4_set_recovery_status(Raspi4bMachineState *s,
                                const char *status);
void raspi4_sample_edids(Raspi4bMachineState *s);
void raspi4_clear_firmware_observation(Raspi4bMachineState *s);
Raspi4BootAttemptResult
raspi4_try_rpiboot_firmware(Raspi4bMachineState *s);
Raspi4OtpProvisionResult raspi4_secure_provision_prepare(
    Raspi4bMachineState *s, const uint8_t *config, size_t config_size,
    const uint8_t *eeprom, size_t eeprom_size, uint8_t key_hash[32]);
Raspi4OtpCommitResult raspi4_secure_provision_commit(
    Raspi4bMachineState *s, const uint8_t key_hash[32]);
Raspi4ProgramResult raspi4_program_eeprom(Raspi4bMachineState *s,
                                          const uint8_t *image,
                                          size_t image_length,
                                          bool allow_timing);
bool raspi4_provision_write(Raspi4bMachineState *s,
                            const char *state);
DWC2State *raspi4_dwc2(Raspi4bMachineState *s);
const char *raspi4_boot_source_name(const Raspi4bMachineState *s,
                                    uint32_t nibble);
Raspi4BootAttemptResult
raspi4_try_usb_firmware(Raspi4bMachineState *s, const char *source);
void raspi4_schedule_usb_wait(Raspi4bMachineState *s,
                              const char *source, bool discovery);
void raspi4_schedule_usb_startup(Raspi4bMachineState *s,
                                 const char *source);
void raspi4_schedule_usb_power_off(Raspi4bMachineState *s,
                                   const char *source,
                                   uint32_t delay_ms);
void raspi4_net_install_request_from(Raspi4bMachineState *s,
                                     const char *trigger);

void raspi4_boot_watchdog_arm(Raspi4bMachineState *s);
void raspi4_boot_uart_observation(Raspi4bMachineState *s,
                                  const char *state,
                                  const char *source);
void raspi4_execute_boot_order_from(Raspi4bMachineState *s,
                                    uint8_t start);
bool raspi4_try_sd_recovery(Raspi4bMachineState *s);
void raspi4_boot_config_init(Raspi4bMachineState *s,
                             Raspi4BootConfig *config);
bool raspi4_boot_filters_active(const Raspi4BootFilterState *filters);
void raspi4_boot_filters_reset(Raspi4BootFilterState *filters);
void raspi4_boot_filter_apply(Raspi4bMachineState *s,
                              Raspi4BootFilterState *filters,
                              const char *filter);
void raspi4_boot_uart_begin(Raspi4bMachineState *s);
void raspi4_boot_uart_end(Raspi4bMachineState *s);
void raspi4_autoboot_parse(RaspiFatVolume *volume, bool tryboot,
                           Raspi4AutobootConfig *config);
bool raspi4_boot_volume_is_bootable(Raspi4bMachineState *s,
                                    RaspiFatVolume *volume);
bool raspi4_boot_media_open(
    const Raspi4BootMediaReader *reader, RaspiFatVolume *volume,
    unsigned int partition, Error **errp);
Raspi4BootAttemptResult
raspi4_try_nvme_firmware(Raspi4bMachineState *s);
bool raspi4_execute_boot_source(Raspi4bMachineState *s,
                                uint32_t nibble, uint8_t index);
void raspi4_execute_boot_order(Raspi4bMachineState *s);
void raspi4_boot_timeout(void *opaque);
void raspi4_boot_watchdog_expired(void *opaque);
char *raspi4b_get_boot_mode(Object *obj, Error **errp);
void raspi4b_set_boot_mode(Object *obj, const char *value, Error **errp);
char *raspi4b_get_bootsys_trusted_sha256(Object *obj, Error **errp);
void raspi4b_set_bootsys_trusted_sha256(Object *obj,
                                        const char *value,
                                        Error **errp);
bool raspi4b_get_boot_disable_hdmi(Object *obj, Error **errp);
char *raspi4b_get_boot_state(Object *obj, Error **errp);
char *raspi4b_get_boot_source(Object *obj, Error **errp);
char *raspi4b_get_bootsys_sha256(Object *obj, Error **errp);
char *raspi4b_get_bootsys_dependencies_sha256(Object *obj,
                                              Error **errp);
void raspi4b_get_bootsys_dependency_count(Object *obj, Visitor *v,
                                          const char *name,
                                          void *opaque, Error **errp);
void raspi4b_get_bootsys_key_index(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp);
char *raspi4b_get_boot_mac_address(Object *obj, Error **errp);
char *raspi4b_get_boot_mac_address_source(Object *obj, Error **errp);
void raspi4b_get_boot_order(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp);
void raspi4b_get_boot_watchdog_timeout(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
void raspi4b_get_boot_watchdog_partition(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp);
bool raspi4b_get_boot_watchdog_armed(Object *obj, Error **errp);
void raspi4b_get_boot_watchdog_remaining(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp);
void raspi4b_get_boot_attempt_count(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp);
void raspi4b_get_boot_restart_count(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp);
void raspi4b_get_boot_retry_count(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp);
void raspi4b_get_boot_elapsed_ms(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp);
void raspi4b_get_boot_order_index(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp);
bool raspi4b_get_boot_uart_enabled(Object *obj, Error **errp);
bool raspi4b_get_boot_uart_active(Object *obj, Error **errp);
void raspi4b_get_boot_uart_bytes(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_boot_uart_lines(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
char *raspi4b_get_boot_uart_format(Object *obj, Error **errp);
char *raspi4b_get_boot_health(Object *obj, Error **errp);
void raspi4b_set_boot_health(Object *obj, const char *value,
                             Error **errp);
void raspi4b_get_boot_partition(Object *obj, Visitor *v,
                                const char *name, void *opaque,
                                Error **errp);
void raspi4b_get_bootloader_signed(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp);
void raspi4_pcie_msi_update(Raspi4bMachineState *s);
bool raspi4_recovery_parse_write_protect(
    const uint8_t *config, size_t config_size, int *value);
bool raspi4_eeprom_status_store(Raspi4bMachineState *s, bool value,
                                Error **errp);
bool raspi4_eeprom_nwp_live(Raspi4bMachineState *s,
                            bool *bridge_driven);
BCM2835PowerMgtState *raspi4_powermgt(Raspi4bMachineState *s);
BCM2835PropertyState *raspi4_property(Raspi4bMachineState *s);
void raspi4_hdmi_diagnostics_show(Raspi4bMachineState *s);
const char *raspi4_recovery_status_from_id(uint8_t id);
bool raspi4_recovery_signature_valid(const uint8_t *image,
                                     size_t image_length,
                                     const uint8_t *signature,
                                     size_t signature_length,
                                     bool *timestamp_valid,
                                     uint32_t *timestamp);
const char *raspi4_validate_recovery(Raspi4bMachineState *s,
                                     const uint8_t *image,
                                     size_t image_length);
bool raspi4_eeprom_update_timestamp_store(Raspi4bMachineState *s,
                                          bool valid,
                                          uint32_t timestamp);
bool raspi4_sd_overcurrent_live(Raspi4bMachineState *s,
                                bool *bridge_driven);
bool raspi4_mac_address_valid(const MACAddr *mac);
bool raspi4_parse_sha256(const char *text, char digest[65]);
void raspi4_uart0_write(Raspi4bMachineState *s, const char *text,
                        uint64_t *bytes, uint32_t *lines);
void raspi4_uart0_configure(Raspi4bMachineState *s);
uint8_t *raspi4_read_firmware_artifact(RaspiFatVolume *volume,
                                       const RaspiFatFile *file,
                                       size_t maximum, uint32_t *size,
                                       char **sha256);
uint8_t *raspi4_read_initramfs(
    RaspiFatVolume *volume, const RaspiFirmwareManifest *manifest,
    uint32_t *size, char **sha256);
bool raspi4_open_boot_volume(Raspi4bMachineState *s,
                             const Raspi4BootMediaReader *reader,
                             RaspiFatVolume *volume, Error **errp);
bool raspi4_firmware_status_is_missing_boot_files(
                                                  const Raspi4bMachineState *s);
void raspi4_sd_hotplug_bh(void *opaque);
void raspi4_resume_after_bootcode_delay(Raspi4bMachineState *s);
uint32_t raspi4_encode_boot_partition(uint8_t partition);
QEMUTimer *raspi4_pending_boot_timer(Raspi4bMachineState *s);
const char *raspi4b_boot_health_name(uint8_t health);

extern const VMStateDescription vmstate_raspi4_boot;

void raspi4_recovery_reboot(void *opaque);
bool raspi4_otp_customer_key_present(BCM2835OTPState *otp);
void raspi4_eeprom_flash_schedule(Raspi4bMachineState *s,
                                  bool continue_cadence);
void raspi4_eeprom_flash_step(void *opaque);
bool raspi4_eeprom_files_parse(const uint8_t *image,
                               size_t image_size,
                               Raspi4EepromFiles *files);
RaspiSecureResult raspi4_secure_eeprom_verify(
    Raspi4bMachineState *s, const Raspi4EepromFiles *files);
const uint8_t *raspi4_eeprom_find_string(const uint8_t *image,
                                         size_t image_size,
                                         const char *prefix);
bool raspi4b_get_eeprom_write_protect(Object *obj, Error **errp);
void raspi4b_set_eeprom_write_protect(Object *obj, bool value,
                                      Error **errp);
bool raspi4b_get_eeprom_nwp(Object *obj, Error **errp);
void raspi4b_set_eeprom_nwp(Object *obj, bool value, Error **errp);
bool raspi4b_get_eeprom_nwp_sampled(Object *obj, Error **errp);
char *raspi4b_get_eeprom_nwp_source(Object *obj, Error **errp);
void raspi4b_get_eeprom_stuck_zero_offset(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_set_eeprom_stuck_zero_offset(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_eeprom_stuck_zero_mask(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_set_eeprom_stuck_zero_mask(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_eeprom_fail_after(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp);
void raspi4b_get_otp_provision_fail_after(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_otp_provision_rows_programmed(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_eeprom_erased_bytes(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp);
void raspi4b_get_eeprom_programmed_bytes(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp);
void raspi4b_get_eeprom_verified_bytes(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
void raspi4b_get_eeprom_dirty_sector_count(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque, Error **errp);
void raspi4b_get_eeprom_program_page_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
void raspi4b_get_eeprom_nor_violation_bits(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
char *raspi4b_get_eeprom_flash_stage(Object *obj, Error **errp);
void raspi4b_set_eeprom_fail_after(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp);
void raspi4b_get_eeprom_erase_delay(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp);
void raspi4b_set_eeprom_erase_delay(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp);
void raspi4b_get_eeprom_program_delay(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp);
void raspi4b_set_eeprom_program_delay(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp);
void raspi4b_get_eeprom_verify_delay(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp);
void raspi4b_set_eeprom_verify_delay(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp);
void raspi4b_get_eeprom_flash_elapsed(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp);
char *raspi4b_get_eeprom_fail_stage(Object *obj, Error **errp);
void raspi4b_set_eeprom_fail_stage(Object *obj, const char *value,
                                   Error **errp);
char *raspi4b_get_eeprom_drive(Object *obj, Error **errp);
char *raspi4b_get_eeprom_status_drive(Object *obj, Error **errp);
char *raspi4b_get_recovery_trusted_sha256(Object *obj, Error **errp);
char *raspi4b_get_otp_drive(Object *obj, Error **errp);
void raspi4b_get_otp_rpiboot_gpio(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp);
void raspi4b_set_eeprom_drive(Object *obj, const char *value,
                              Error **errp);
void raspi4b_set_eeprom_status_drive(Object *obj, const char *value,
                                     Error **errp);
char *raspi4b_get_recovery_status(Object *obj, Error **errp);
char *raspi4b_get_recovery_sha256(Object *obj, Error **errp);
void raspi4b_get_recovery_key_index(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp);
void raspi4b_get_otp_bootmode(Object *obj, Visitor *v,
                              const char *name, void *opaque,
                              Error **errp);
void raspi4b_get_otp_secure_boot_flags(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
void raspi4b_get_otp_board_revision(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp);
bool raspi4b_get_eeprom_build_timestamp_valid(Object *obj,
                                              Error **errp);
void raspi4b_get_eeprom_build_timestamp(Object *obj, Visitor *v,
                                        const char *name,
                                        void *opaque, Error **errp);
bool raspi4b_get_eeprom_update_timestamp_valid(Object *obj,
                                               Error **errp);
void raspi4b_get_eeprom_update_timestamp(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp);
bool raspi4b_get_eeprom_capabilities_valid(Object *obj, Error **errp);
void raspi4b_get_eeprom_capabilities(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp);
char *raspi4b_get_eeprom_version(Object *obj, Error **errp);
void raspi4b_get_eeprom_config_append_size(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque,
                                           Error **errp);
char *raspi4b_get_eeprom_config_append_sha256(Object *obj,
                                              Error **errp);
bool raspi4b_get_otp_secure_boot(Object *obj, Error **errp);
bool raspi4_provision_parse_bool(const uint8_t *config,
                                 size_t config_size,
                                 const char *key, bool *present,
                                 bool *value);
GByteArray *raspi4_lz4_frame_decode(const uint8_t *frame,
                                    size_t frame_size);
bool raspi4_buffer_contains(const uint8_t *haystack,
                            size_t haystack_size,
                            const uint8_t *needle,
                            size_t needle_size);

char *raspi4_provision_read(Raspi4bMachineState *s);
bool raspi4_provision_lock(Raspi4bMachineState *s);
void raspi4_provision_release(Raspi4bMachineState *s);
void raspi4_provision_exit_notify(Notifier *notifier, void *data);

void raspi4_net_install_request(Raspi4bMachineState *s);
bool raspi4_parse_mac_address(const char *value, MACAddr *mac);
bool raspi4_parse_mac_address_otp(Raspi4bMachineState *s,
                                  const char *value, MACAddr *mac);
bool raspi4_parse_netconsole(const char *text,
                             Raspi4BootConfig *config);
bool raspi4_parse_netmask(const char *text, uint32_t *value);
uint32_t raspi4_xxh32_short(const uint8_t *data, size_t size);

#endif /* HW_ARM_RASPI4B_INTERNAL_H */
