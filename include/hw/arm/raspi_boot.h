/*
 * Raspberry Pi boot-media helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RASPI_BOOT_H
#define HW_ARM_RASPI_BOOT_H

#include "qapi/error.h"
#include "system/block-backend.h"

#define RASPI_FAT_SHORT_NAME_LEN 11

typedef enum RaspiFatResult {
    RASPI_FAT_ERROR = -1,
    RASPI_FAT_NOT_FOUND = 0,
    RASPI_FAT_FOUND = 1,
} RaspiFatResult;

typedef bool (*RaspiBootMediaRead)(void *opaque, int64_t offset,
                                   int64_t bytes, void *buffer,
                                   Error **errp);

typedef struct RaspiFatVolume {
    BlockBackend *blk;
    RaspiBootMediaRead read;
    void *read_opaque;
    const uint8_t *memory;
    int64_t media_size;
    int64_t partition_offset;
    int64_t partition_size;
    unsigned partition_number;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t fat_sectors;
    uint32_t root_entries;
    uint32_t root_cluster;
    uint32_t data_start_sector;
    uint32_t cluster_count;
    unsigned fat_type;
} RaspiFatVolume;

typedef struct RaspiFatFile {
    uint32_t first_cluster;
    uint32_t size;
    int64_t directory_entry_offset;
    uint8_t attributes;
} RaspiFatFile;

bool raspi_fat_open(RaspiFatVolume *volume, BlockBackend *blk, Error **errp);
bool raspi_fat_open_partition(RaspiFatVolume *volume, BlockBackend *blk,
                              unsigned partition_number, Error **errp);
bool raspi_fat_open_reader(RaspiFatVolume *volume, RaspiBootMediaRead read,
                           void *opaque, int64_t media_size, Error **errp);
bool raspi_fat_open_reader_partition(
    RaspiFatVolume *volume, RaspiBootMediaRead read, void *opaque,
    int64_t media_size, unsigned partition_number, Error **errp);
bool raspi_fat_open_memory(RaspiFatVolume *volume, const uint8_t *memory,
                           size_t memory_size, Error **errp);
RaspiFatResult raspi_fat_find_file(RaspiFatVolume *volume,
                                   const char name[RASPI_FAT_SHORT_NAME_LEN],
                                   RaspiFatFile *file, Error **errp);
RaspiFatResult raspi_fat_find_path(RaspiFatVolume *volume, const char *path,
                                   RaspiFatFile *file, Error **errp);
uint8_t *raspi_fat_read_file(RaspiFatVolume *volume,
                             const RaspiFatFile *file, size_t maximum,
                             size_t *length, Error **errp);
bool raspi_fat_rename_file(RaspiFatVolume *volume, const RaspiFatFile *file,
                           const char name[RASPI_FAT_SHORT_NAME_LEN],
                           Error **errp);

#endif
