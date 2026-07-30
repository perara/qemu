/*
 * DWC2 device-mode host transport protocol
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_DWC2_DEVICE_TRANSPORT_H
#define HW_USB_DWC2_DEVICE_TRANSPORT_H

/*
 * Every request and response starts with this 16-byte little-endian header:
 *
 *   0..3   magic "D2HT"
 *   4      protocol version
 *   5      opcode (bit 7 marks a response)
 *   6      endpoint number
 *   7      flags (reserved, must be zero)
 *   8..11  payload length
 *   12..15 value
 *
 * Request value carries the enumeration speed or maximum IN length.
 * Response value is a non-negative byte count or a negative errno.
 */
#define DWC2_DEVICE_TRANSPORT_MAGIC        0x54483244U
#define DWC2_DEVICE_TRANSPORT_VERSION      1
#define DWC2_DEVICE_TRANSPORT_HEADER_SIZE  16
#define DWC2_DEVICE_TRANSPORT_MAX_PAYLOAD  (64 * 1024)
#define DWC2_DEVICE_TRANSPORT_RESPONSE      0x80

enum DWC2DeviceTransportOpcode {
    DWC2_DEVICE_TRANSPORT_CONNECT = 1,
    DWC2_DEVICE_TRANSPORT_DISCONNECT,
    DWC2_DEVICE_TRANSPORT_RESET,
    DWC2_DEVICE_TRANSPORT_ENUM_DONE,
    DWC2_DEVICE_TRANSPORT_OUT,
    DWC2_DEVICE_TRANSPORT_SETUP,
    DWC2_DEVICE_TRANSPORT_IN,
    DWC2_DEVICE_TRANSPORT_SUSPEND,
    DWC2_DEVICE_TRANSPORT_RESUME,
    DWC2_DEVICE_TRANSPORT_STATE,
};

#define DWC2_DEVICE_TRANSPORT_STATE_PULLUP       (1U << 0)
#define DWC2_DEVICE_TRANSPORT_STATE_EP0_SETUP    (1U << 1)

#endif
