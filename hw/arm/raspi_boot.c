/*
 * Raspberry Pi boot-media helpers
 *
 * This is a deliberately small FAT implementation for the early boot path.
 * It reads short or VFAT long-name paths from raw SD media and only writes an
 * existing 8.3 directory name for recovery.bin completion semantics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/arm/raspi_boot.h"
#include <zlib.h>

#define MBR_SIZE 512
#define GPT_HEADER_MIN_SIZE 92
#define GPT_ENTRY_MIN_SIZE 128
#define GPT_ENTRY_ARRAY_MAX_SIZE (16 * MiB)
#define MBR_MAX_LOGICAL_PARTITIONS 128
#define FAT_DIRECTORY_ENTRY_SIZE 32
#define FAT_MAX_CLUSTER_SIZE (128 * 4096)
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_LONG_NAME 0x0f
#define FAT_LFN_CHARS_PER_ENTRY 13
#define FAT_LFN_MAX_ENTRIES 20
#define FAT_PATH_MAX_DEPTH 32
#define FAT_PATH_MAX_LENGTH 1024

typedef struct RaspiFatLfnState {
    gunichar2 name[FAT_LFN_CHARS_PER_ENTRY * FAT_LFN_MAX_ENTRIES + 1];
    unsigned int expected_order;
    uint8_t checksum;
    bool valid;
} RaspiFatLfnState;

typedef struct RaspiMediaReader {
    BlockBackend *blk;
    RaspiBootMediaRead read;
    void *opaque;
    int64_t size;
} RaspiMediaReader;

static bool raspi_block_media_read(void *opaque, int64_t offset,
                                   int64_t bytes, void *buffer,
                                   Error **errp)
{
    BlockBackend *blk = opaque;
    int ret = blk_pread(blk, offset, bytes, buffer, 0);

    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to read Raspberry Pi boot media");
        return false;
    }
    return true;
}

static bool raspi_media_read(const RaspiMediaReader *reader, int64_t offset,
                             int64_t bytes, void *buffer, Error **errp)
{
    if (offset < 0 || bytes < 0 || offset > reader->size ||
        bytes > reader->size - offset) {
        error_setg(errp, "read exceeds Raspberry Pi boot media");
        return false;
    }
    return reader->read(reader->opaque, offset, bytes, buffer, errp);
}

static bool raspi_media_write(BlockBackend *blk, int64_t offset, int64_t bytes,
                              const void *buffer, Error **errp)
{
    int ret = blk_pwrite(blk, offset, bytes, buffer, 0);

    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to write Raspberry Pi boot media");
        return false;
    }
    ret = blk_flush(blk);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to flush Raspberry Pi boot media");
        return false;
    }
    return true;
}

static bool raspi_volume_read(const RaspiFatVolume *volume, int64_t offset,
                              int64_t bytes, void *buffer, Error **errp)
{
    if (volume->read) {
        RaspiMediaReader reader = {
            .blk = volume->blk,
            .read = volume->read,
            .opaque = volume->read_opaque,
            .size = volume->media_size,
        };

        return raspi_media_read(&reader, offset, bytes, buffer, errp);
    }
    if (offset < 0 || bytes < 0 || offset > volume->media_size ||
        bytes > volume->media_size - offset) {
        error_setg(errp, "read exceeds Raspberry Pi memory boot image");
        return false;
    }
    memcpy(buffer, volume->memory + offset, bytes);
    return true;
}

static bool raspi_is_fat_boot_sector(const uint8_t sector[MBR_SIZE])
{
    uint32_t bytes_per_sector = lduw_le_p(sector + 11);
    uint32_t sectors_per_cluster = sector[13];

    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        return false;
    }
    if (bytes_per_sector != 512 && bytes_per_sector != 1024 &&
        bytes_per_sector != 2048 && bytes_per_sector != 4096) {
        return false;
    }
    if (!is_power_of_2(sectors_per_cluster) || sectors_per_cluster > 128 ||
        !lduw_le_p(sector + 14) || !sector[16]) {
        return false;
    }
    return lduw_le_p(sector + 19) || ldl_le_p(sector + 32);
}

static bool raspi_mbr_fat_type(uint8_t type)
{
    switch (type) {
    case 0x01:
    case 0x04:
    case 0x06:
    case 0x0b:
    case 0x0c:
    case 0x0e:
        return true;
    default:
        return false;
    }
}

static bool raspi_mbr_extended_type(uint8_t type)
{
    return type == 0x05 || type == 0x0f || type == 0x85;
}

static bool raspi_mbr_range_valid(uint64_t first_lba, uint64_t sectors,
                                  uint64_t range_first, uint64_t range_sectors,
                                  uint64_t media_sectors)
{
    uint64_t range_end;

    if (!first_lba || !sectors || range_first > media_sectors ||
        range_sectors > media_sectors - range_first) {
        return false;
    }
    range_end = range_first + range_sectors;
    return first_lba >= range_first && first_lba < range_end &&
           sectors <= range_end - first_lba &&
           first_lba < media_sectors && sectors <= media_sectors - first_lba;
}

static bool raspi_mbr_open_fat(const RaspiMediaReader *reader,
                               uint64_t first_lba, uint64_t sectors,
                               unsigned int partition_number,
                               uint64_t *partition_offset,
                               uint64_t *partition_size,
                               uint8_t sector[MBR_SIZE],
                               Error **errp)
{
    *partition_offset = first_lba * MBR_SIZE;
    *partition_size = sectors * MBR_SIZE;
    if (first_lba >= (uint64_t)reader->size / MBR_SIZE ||
        *partition_offset >= (uint64_t)reader->size ||
        *partition_size > (uint64_t)reader->size - *partition_offset) {
        error_setg(errp, "MBR FAT partition %u exceeds the media",
                   partition_number);
        return false;
    }
    if (!raspi_media_read(reader, *partition_offset, MBR_SIZE, sector,
                          errp)) {
        return false;
    }
    if (!raspi_is_fat_boot_sector(sector)) {
        error_setg(errp, "MBR FAT partition %u has an invalid boot sector",
                   partition_number);
        return false;
    }
    return true;
}

static bool raspi_mbr_find_logical_fat(
    const RaspiMediaReader *reader, uint32_t extended_base,
    uint32_t extended_sectors, unsigned int requested_partition,
    uint64_t *partition_offset, uint64_t *partition_size,
    unsigned int *partition_number, uint8_t sector[MBR_SIZE], bool *found,
    Error **errp)
{
    uint64_t media_sectors = (uint64_t)reader->size / MBR_SIZE;
    uint64_t visited[MBR_MAX_LOGICAL_PARTITIONS];
    uint64_t ebr_lba = extended_base;

    *found = false;
    if (!raspi_mbr_range_valid(extended_base, extended_sectors,
                               extended_base, extended_sectors,
                               media_sectors)) {
        error_setg(errp, "MBR extended partition exceeds the media");
        return false;
    }

    for (unsigned int i = 0; i < MBR_MAX_LOGICAL_PARTITIONS; i++) {
        const uint8_t *logical;
        const uint8_t *link;
        uint64_t logical_first;
        uint64_t next_ebr;
        uint32_t logical_relative;
        uint32_t logical_sectors;
        uint32_t link_relative;
        uint32_t link_sectors;
        unsigned int number = i + 5;

        for (unsigned int j = 0; j < i; j++) {
            if (visited[j] == ebr_lba) {
                error_setg(errp, "MBR extended partition chain contains a "
                           "cycle at LBA %" PRIu64, ebr_lba);
                return false;
            }
        }
        visited[i] = ebr_lba;
        if (!raspi_mbr_range_valid(ebr_lba, 1, extended_base,
                                   extended_sectors, media_sectors)) {
            error_setg(errp, "MBR EBR %u lies outside the extended partition",
                       number);
            return false;
        }
        if (!raspi_media_read(reader, ebr_lba * MBR_SIZE, MBR_SIZE, sector,
                              errp)) {
            return false;
        }
        if (sector[510] != 0x55 || sector[511] != 0xaa) {
            error_setg(errp, "MBR EBR %u has an invalid signature", number);
            return false;
        }

        for (unsigned int j = 2; j < 4; j++) {
            const uint8_t *unused = sector + 446 + j * 16;

            if (unused[4] || ldl_le_p(unused + 8) ||
                ldl_le_p(unused + 12)) {
                error_setg(errp, "MBR EBR %u has an unexpected entry %u",
                           number, j + 1);
                return false;
            }
        }

        logical = sector + 446;
        logical_relative = ldl_le_p(logical + 8);
        logical_sectors = ldl_le_p(logical + 12);
        if (!logical[4] || !logical_relative || !logical_sectors) {
            error_setg(errp, "MBR logical partition %u is malformed", number);
            return false;
        }
        logical_first = ebr_lba + logical_relative;
        if (logical_first < ebr_lba ||
            !raspi_mbr_range_valid(logical_first, logical_sectors,
                                   extended_base, extended_sectors,
                                   media_sectors)) {
            error_setg(errp, "MBR logical partition %u exceeds the extended "
                       "partition", number);
            return false;
        }
        if ((!requested_partition || requested_partition == number) &&
            raspi_mbr_fat_type(logical[4])) {
            if (!raspi_mbr_open_fat(reader, logical_first, logical_sectors,
                                    number, partition_offset, partition_size,
                                    sector, errp)) {
                return false;
            }
            *partition_number = number;
            *found = true;
            return true;
        }
        if (requested_partition == number) {
            error_setg(errp, "MBR partition %u is not a FAT partition",
                       requested_partition);
            return false;
        }

        link = sector + 446 + 16;
        link_relative = ldl_le_p(link + 8);
        link_sectors = ldl_le_p(link + 12);
        if (!link[4] && !link_relative && !link_sectors) {
            return true;
        }
        if (!raspi_mbr_extended_type(link[4]) || !link_relative ||
            !link_sectors) {
            error_setg(errp, "MBR EBR %u has a malformed next link", number);
            return false;
        }
        next_ebr = (uint64_t)extended_base + link_relative;
        if (next_ebr < extended_base ||
            !raspi_mbr_range_valid(next_ebr, link_sectors, extended_base,
                                   extended_sectors, media_sectors)) {
            error_setg(errp, "MBR EBR %u next link exceeds the extended "
                       "partition", number);
            return false;
        }
        ebr_lba = next_ebr;
    }

    error_setg(errp, "MBR extended partition chain exceeds %u entries",
               MBR_MAX_LOGICAL_PARTITIONS);
    return false;
}

typedef struct RaspiGptHeader {
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
    uint8_t disk_guid[16];
} RaspiGptHeader;

static bool raspi_gpt_read_header(const RaspiMediaReader *reader, uint64_t lba,
                                  uint64_t media_sectors,
                                  RaspiGptHeader *header, Error **errp)
{
    uint8_t sector[MBR_SIZE];
    uint8_t checked[MBR_SIZE];
    uint32_t header_size;
    uint32_t stored_crc;

    if (lba >= media_sectors ||
        !raspi_media_read(reader, lba * MBR_SIZE, sizeof(sector), sector,
                          errp)) {
        return false;
    }
    if (memcmp(sector, "EFI PART", 8)) {
        error_setg(errp, "GPT header signature is invalid");
        return false;
    }
    if (ldl_le_p(sector + 8) != 0x00010000) {
        error_setg(errp, "GPT revision is unsupported");
        return false;
    }
    header_size = ldl_le_p(sector + 12);
    if (header_size < GPT_HEADER_MIN_SIZE || header_size > sizeof(sector)) {
        error_setg(errp, "GPT header size is invalid");
        return false;
    }
    memcpy(checked, sector, header_size);
    stored_crc = ldl_le_p(checked + 16);
    stl_le_p(checked + 16, 0);
    if (crc32(0, checked, header_size) != stored_crc) {
        error_setg(errp, "GPT header CRC is invalid");
        return false;
    }
    if (ldl_le_p(sector + 20)) {
        error_setg(errp, "GPT header reserved field is nonzero");
        return false;
    }
    header->current_lba = ldq_le_p(sector + 24);
    header->backup_lba = ldq_le_p(sector + 32);
    header->first_usable_lba = ldq_le_p(sector + 40);
    header->last_usable_lba = ldq_le_p(sector + 48);
    memcpy(header->disk_guid, sector + 56, sizeof(header->disk_guid));
    header->entries_lba = ldq_le_p(sector + 72);
    header->entry_count = ldl_le_p(sector + 80);
    header->entry_size = ldl_le_p(sector + 84);
    header->entries_crc = ldl_le_p(sector + 88);
    if (header->current_lba != lba ||
        header->backup_lba >= media_sectors ||
        header->first_usable_lba > header->last_usable_lba ||
        header->last_usable_lba >= media_sectors ||
        header->entry_size < GPT_ENTRY_MIN_SIZE ||
        header->entry_size & 7 || !header->entry_count ||
        header->entry_count > GPT_ENTRY_ARRAY_MAX_SIZE / header->entry_size) {
        error_setg(errp, "GPT header geometry is invalid");
        return false;
    }
    return true;
}

static uint8_t *raspi_gpt_read_entries(const RaspiMediaReader *reader,
                                       const RaspiGptHeader *header,
                                       uint64_t media_sectors,
                                       size_t *entries_size, Error **errp)
{
    uint64_t bytes = (uint64_t)header->entry_count * header->entry_size;
    uint64_t sectors = DIV_ROUND_UP(bytes, MBR_SIZE);
    uint8_t *entries;

    if (header->entries_lba >= media_sectors ||
        sectors > media_sectors - header->entries_lba) {
        error_setg(errp, "GPT entry array exceeds the media");
        return NULL;
    }
    entries = g_malloc(bytes);
    if (!raspi_media_read(reader, header->entries_lba * MBR_SIZE, bytes,
                          entries, errp)) {
        g_free(entries);
        return NULL;
    }
    if (crc32(0, entries, bytes) != header->entries_crc) {
        error_setg(errp, "GPT entry-array CRC is invalid");
        g_free(entries);
        return NULL;
    }
    *entries_size = bytes;
    return entries;
}

static bool raspi_gpt_find_fat(const RaspiMediaReader *reader,
                               unsigned requested_partition,
                               uint64_t *partition_offset,
                               uint64_t *partition_size,
                               unsigned *partition_number,
                               uint8_t sector[MBR_SIZE], Error **errp)
{
    uint64_t media_sectors = reader->size / MBR_SIZE;
    RaspiGptHeader primary;
    RaspiGptHeader backup;
    g_autofree uint8_t *primary_entries = NULL;
    g_autofree uint8_t *backup_entries = NULL;
    size_t primary_size;
    size_t backup_size;

    if (!raspi_gpt_read_header(reader, 1, media_sectors, &primary, errp)) {
        return false;
    }
    if (primary.backup_lba != media_sectors - 1) {
        error_setg(errp, "GPT backup header is not at the end of media");
        return false;
    }
    if (!raspi_gpt_read_header(reader, primary.backup_lba, media_sectors,
                               &backup, errp)) {
        return false;
    }
    if (backup.backup_lba != primary.current_lba ||
        backup.first_usable_lba != primary.first_usable_lba ||
        backup.last_usable_lba != primary.last_usable_lba ||
        backup.entry_count != primary.entry_count ||
        backup.entry_size != primary.entry_size ||
        backup.entries_crc != primary.entries_crc ||
        memcmp(backup.disk_guid, primary.disk_guid,
               sizeof(primary.disk_guid))) {
        error_setg(errp, "GPT primary and backup headers disagree");
        return false;
    }
    primary_size = (size_t)primary.entry_count * primary.entry_size;
    backup_size = (size_t)backup.entry_count * backup.entry_size;
    if (primary.entries_lba < 2 ||
        primary.entries_lba + DIV_ROUND_UP(primary_size, MBR_SIZE) >
            primary.first_usable_lba ||
        backup.entries_lba <= backup.last_usable_lba ||
        backup.entries_lba + DIV_ROUND_UP(backup_size, MBR_SIZE) >
            backup.current_lba) {
        error_setg(errp, "GPT entry-array placement is invalid");
        return false;
    }
    primary_entries = raspi_gpt_read_entries(
        reader, &primary, media_sectors, &primary_size, errp);
    if (!primary_entries) {
        return false;
    }
    backup_entries = raspi_gpt_read_entries(
        reader, &backup, media_sectors, &backup_size, errp);
    if (!backup_entries) {
        return false;
    }
    if (primary_size != backup_size ||
        memcmp(primary_entries, backup_entries, primary_size)) {
        error_setg(errp, "GPT primary and backup entry arrays disagree");
        return false;
    }

    for (unsigned int i = 0; i < primary.entry_count; i++) {
        const uint8_t *entry = primary_entries + i * primary.entry_size;
        uint64_t first_lba;
        uint64_t last_lba;

        if (requested_partition && i + 1 != requested_partition) {
            continue;
        }
        if (buffer_is_zero(entry, 16)) {
            continue;
        }
        first_lba = ldq_le_p(entry + 32);
        last_lba = ldq_le_p(entry + 40);
        if (first_lba < primary.first_usable_lba ||
            last_lba < first_lba || last_lba > primary.last_usable_lba) {
            error_setg(errp, "GPT partition %u has invalid bounds", i + 1);
            return false;
        }
        if (!raspi_media_read(reader, first_lba * MBR_SIZE, MBR_SIZE,
                              sector, errp)) {
            return false;
        }
        if (raspi_is_fat_boot_sector(sector)) {
            *partition_offset = first_lba * MBR_SIZE;
            *partition_size = (last_lba - first_lba + 1) * MBR_SIZE;
            *partition_number = i + 1;
            return true;
        }
    }
    if (requested_partition) {
        error_setg(errp, "GPT partition %u is not a FAT partition",
                   requested_partition);
    } else {
        error_setg(errp, "GPT media has no FAT partition");
    }
    return false;
}

static bool raspi_fat_finish_open(RaspiFatVolume *volume,
                                  const uint8_t sector[MBR_SIZE],
                                  Error **errp)
{
    uint64_t total_sectors;
    uint64_t root_dir_sectors;
    uint64_t non_data_sectors;
    volume->bytes_per_sector = lduw_le_p(sector + 11);
    volume->sectors_per_cluster = sector[13];
    volume->reserved_sectors = lduw_le_p(sector + 14);
    volume->fat_count = sector[16];
    volume->root_entries = lduw_le_p(sector + 17);
    total_sectors = lduw_le_p(sector + 19);
    if (!total_sectors) {
        total_sectors = ldl_le_p(sector + 32);
    }
    volume->fat_sectors = lduw_le_p(sector + 22);
    if (!volume->fat_sectors) {
        volume->fat_sectors = ldl_le_p(sector + 36);
    }
    if (!volume->fat_sectors) {
        error_setg(errp, "FAT table has zero sectors");
        return false;
    }

    root_dir_sectors = DIV_ROUND_UP((uint64_t)volume->root_entries *
                                    FAT_DIRECTORY_ENTRY_SIZE,
                                    volume->bytes_per_sector);
    non_data_sectors = volume->reserved_sectors +
                       (uint64_t)volume->fat_count * volume->fat_sectors +
                       root_dir_sectors;
    if (non_data_sectors >= total_sectors ||
        total_sectors * volume->bytes_per_sector > volume->partition_size) {
        error_setg(errp, "FAT volume geometry exceeds the SD media");
        return false;
    }
    volume->data_start_sector = non_data_sectors;
    volume->cluster_count = (total_sectors - non_data_sectors) /
                            volume->sectors_per_cluster;
    if (volume->cluster_count < 4085) {
        volume->fat_type = 12;
    } else if (volume->cluster_count < 65525) {
        volume->fat_type = 16;
    } else {
        volume->fat_type = 32;
    }
    if (volume->fat_type == 32) {
        volume->root_cluster = ldl_le_p(sector + 44);
        if (volume->root_cluster < 2) {
            error_setg(errp, "FAT32 root cluster is invalid");
            return false;
        }
    } else if (!volume->root_entries) {
        error_setg(errp, "FAT12/16 root directory has zero entries");
        return false;
    }
    if ((uint64_t)volume->bytes_per_sector * volume->sectors_per_cluster >
        FAT_MAX_CLUSTER_SIZE) {
        error_setg(errp, "FAT cluster size is unsupported");
        return false;
    }
    return true;
}

static bool raspi_fat_open_reader_common(
    RaspiFatVolume *volume, const RaspiMediaReader *reader,
    unsigned requested_partition, Error **errp)
{
    uint8_t sector[MBR_SIZE];
    uint64_t partition_offset = 0;
    uint64_t partition_size;

    memset(volume, 0, sizeof(*volume));
    if (!reader->read || reader->size < MBR_SIZE) {
        error_setg(errp, "SD boot media is not present");
        return false;
    }
    if (!raspi_media_read(reader, 0, sizeof(sector), sector, errp)) {
        return false;
    }
    partition_size = reader->size;

    if (!raspi_is_fat_boot_sector(sector)) {
        bool found = false;
        bool protective = false;
        uint32_t extended_base = 0;
        uint32_t extended_sectors = 0;

        if (sector[510] != 0x55 || sector[511] != 0xaa) {
            error_setg(errp, "SD media has no MBR or FAT boot signature");
            return false;
        }
        for (unsigned int i = 0; i < 4; i++) {
            if (sector[446 + i * 16 + 4] == 0xee) {
                protective = true;
                break;
            }
        }
        if (protective) {
            if (!raspi_gpt_find_fat(reader, requested_partition,
                                    &partition_offset, &partition_size,
                                    &volume->partition_number, sector, errp)) {
                return false;
            }
            found = true;
        }
        for (unsigned int i = 0; !found && i < 4; i++) {
            const uint8_t *entry = sector + 446 + i * 16;
            uint32_t first_lba = ldl_le_p(entry + 8);
            uint32_t sectors = ldl_le_p(entry + 12);

            if (raspi_mbr_extended_type(entry[4])) {
                if (extended_base) {
                    error_setg(errp,
                               "MBR has multiple extended partitions");
                    return false;
                }
                extended_base = first_lba;
                extended_sectors = sectors;
            }
            if (requested_partition && i + 1 != requested_partition) {
                continue;
            }
            if (!raspi_mbr_fat_type(entry[4]) || !first_lba || !sectors) {
                continue;
            }
            if (!raspi_mbr_open_fat(reader, first_lba, sectors,
                                    i + 1, &partition_offset,
                                    &partition_size, sector, errp)) {
                return false;
            }
            volume->partition_number = i + 1;
            found = true;
            break;
        }
        if (!found && extended_base &&
            (!requested_partition || requested_partition >= 5)) {
            if (!raspi_mbr_find_logical_fat(
                    reader, extended_base, extended_sectors,
                    requested_partition, &partition_offset, &partition_size,
                    &volume->partition_number, sector, &found, errp)) {
                return false;
            }
        }
        if (!found) {
            if (requested_partition) {
                error_setg(errp, "MBR partition %u is not a FAT partition",
                           requested_partition);
            } else {
                error_setg(errp, "SD media has no FAT partition");
            }
            return false;
        }
    } else if (requested_partition > 1) {
        error_setg(errp, "superfloppy media has no partition %u",
                   requested_partition);
        return false;
    } else {
        volume->partition_number = requested_partition ? 1 : 0;
    }

    volume->blk = reader->blk;
    volume->read = reader->read;
    volume->read_opaque = reader->opaque;
    volume->media_size = reader->size;
    volume->partition_offset = partition_offset;
    volume->partition_size = partition_size;
    return raspi_fat_finish_open(volume, sector, errp);
}

bool raspi_fat_open(RaspiFatVolume *volume, BlockBackend *blk, Error **errp)
{
    return raspi_fat_open_partition(volume, blk, 0, errp);
}

bool raspi_fat_open_partition(RaspiFatVolume *volume, BlockBackend *blk,
                              unsigned requested_partition, Error **errp)
{
    RaspiMediaReader reader;

    if (!blk || !blk_is_inserted(blk)) {
        error_setg(errp, "SD boot media is not present");
        return false;
    }
    reader = (RaspiMediaReader) {
        .blk = blk,
        .read = raspi_block_media_read,
        .opaque = blk,
        .size = blk_getlength(blk),
    };
    return raspi_fat_open_reader_common(
        volume, &reader, requested_partition, errp);
}

bool raspi_fat_open_reader(RaspiFatVolume *volume, RaspiBootMediaRead read,
                           void *opaque, int64_t media_size, Error **errp)
{
    return raspi_fat_open_reader_partition(
        volume, read, opaque, media_size, 0, errp);
}

bool raspi_fat_open_reader_partition(
    RaspiFatVolume *volume, RaspiBootMediaRead read, void *opaque,
    int64_t media_size, unsigned requested_partition, Error **errp)
{
    RaspiMediaReader reader = {
        .read = read,
        .opaque = opaque,
        .size = media_size,
    };

    return raspi_fat_open_reader_common(
        volume, &reader, requested_partition, errp);
}

bool raspi_fat_open_memory(RaspiFatVolume *volume, const uint8_t *memory,
                           size_t memory_size, Error **errp)
{
    memset(volume, 0, sizeof(*volume));
    if (!memory || memory_size < MBR_SIZE || memory_size > INT64_MAX) {
        error_setg(errp, "Raspberry Pi memory boot image is too small");
        return false;
    }
    if (!raspi_is_fat_boot_sector(memory)) {
        error_setg(errp, "Raspberry Pi memory boot image is not FAT");
        return false;
    }
    volume->memory = memory;
    volume->media_size = memory_size;
    volume->partition_size = memory_size;
    return raspi_fat_finish_open(volume, memory, errp);
}

static bool raspi_fat_cluster_offset(const RaspiFatVolume *volume,
                                     uint32_t cluster, int64_t *offset,
                                     Error **errp)
{
    uint64_t sector;
    uint64_t byte_offset;

    if (cluster < 2 || cluster >= volume->cluster_count + 2) {
        error_setg(errp, "FAT cluster %u is outside the data region", cluster);
        return false;
    }
    sector = volume->data_start_sector +
             (uint64_t)(cluster - 2) * volume->sectors_per_cluster;
    byte_offset = volume->partition_offset +
                  sector * volume->bytes_per_sector;
    if (byte_offset > INT64_MAX) {
        error_setg(errp, "FAT cluster offset overflows");
        return false;
    }
    *offset = byte_offset;
    return true;
}

static bool raspi_fat_next_cluster(RaspiFatVolume *volume, uint32_t cluster,
                                   uint32_t *next, bool *end, Error **errp)
{
    uint8_t entry[4] = { 0 };
    uint64_t fat_byte;
    unsigned entry_size;

    if (volume->fat_type == 12) {
        fat_byte = cluster + cluster / 2;
        entry_size = 2;
    } else if (volume->fat_type == 16) {
        fat_byte = (uint64_t)cluster * 2;
        entry_size = 2;
    } else {
        fat_byte = (uint64_t)cluster * 4;
        entry_size = 4;
    }
    if (fat_byte + entry_size >
        (uint64_t)volume->fat_sectors * volume->bytes_per_sector) {
        error_setg(errp, "FAT cluster entry is outside the first FAT");
        return false;
    }
    if (!raspi_volume_read(volume,
                           volume->partition_offset +
                           (uint64_t)volume->reserved_sectors *
                           volume->bytes_per_sector + fat_byte,
                           entry_size, entry, errp)) {
        return false;
    }
    if (volume->fat_type == 12) {
        uint32_t value = lduw_le_p(entry);

        *next = cluster & 1 ? value >> 4 : value & 0xfff;
        *end = *next >= 0xff8;
    } else if (volume->fat_type == 16) {
        *next = lduw_le_p(entry);
        *end = *next >= 0xfff8;
    } else {
        *next = ldl_le_p(entry) & 0x0fffffff;
        *end = *next >= 0x0ffffff8;
    }
    if (!*end && (*next < 2 || *next >= volume->cluster_count + 2)) {
        error_setg(errp, "FAT cluster chain contains invalid entry 0x%x",
                   *next);
        return false;
    }
    return true;
}

static RaspiFatResult raspi_fat_scan_directory(
    RaspiFatVolume *volume, int64_t offset, size_t length,
    const char name[RASPI_FAT_SHORT_NAME_LEN], RaspiFatFile *file,
    Error **errp)
{
    g_autofree uint8_t *directory = g_malloc(length);

    if (!raspi_volume_read(volume, offset, length, directory, errp)) {
        return RASPI_FAT_ERROR;
    }
    for (size_t entry_offset = 0;
         entry_offset + FAT_DIRECTORY_ENTRY_SIZE <= length;
         entry_offset += FAT_DIRECTORY_ENTRY_SIZE) {
        const uint8_t *entry = directory + entry_offset;

        if (!entry[0]) {
            return RASPI_FAT_NOT_FOUND;
        }
        if (entry[0] == 0xe5 || entry[11] == 0x0f ||
            entry[11] & (0x08 | 0x10)) {
            continue;
        }
        if (!memcmp(entry, name, RASPI_FAT_SHORT_NAME_LEN)) {
            file->first_cluster = lduw_le_p(entry + 26) |
                                  (uint32_t)lduw_le_p(entry + 20) << 16;
            file->size = ldl_le_p(entry + 28);
            file->directory_entry_offset = offset + entry_offset;
            file->attributes = entry[11];
            return RASPI_FAT_FOUND;
        }
    }
    return RASPI_FAT_NOT_FOUND;
}

RaspiFatResult raspi_fat_find_file(RaspiFatVolume *volume,
                                   const char name[RASPI_FAT_SHORT_NAME_LEN],
                                   RaspiFatFile *file, Error **errp)
{
    uint64_t root_bytes;
    int64_t offset;
    uint32_t cluster;
    size_t cluster_bytes;

    memset(file, 0, sizeof(*file));
    if (volume->fat_type != 32) {
        root_bytes = (uint64_t)volume->root_entries *
                     FAT_DIRECTORY_ENTRY_SIZE;
        offset = volume->partition_offset +
                 (volume->reserved_sectors +
                  (uint64_t)volume->fat_count * volume->fat_sectors) *
                 volume->bytes_per_sector;
        return raspi_fat_scan_directory(volume, offset, root_bytes, name,
                                        file, errp);
    }

    cluster = volume->root_cluster;
    cluster_bytes = (size_t)volume->bytes_per_sector *
                    volume->sectors_per_cluster;

    for (uint32_t visited = 0; visited <= volume->cluster_count; visited++) {
        RaspiFatResult result;
        uint32_t next;
        bool end;

        if (!raspi_fat_cluster_offset(volume, cluster, &offset, errp)) {
            return RASPI_FAT_ERROR;
        }
        result = raspi_fat_scan_directory(volume, offset, cluster_bytes,
                                          name, file, errp);
        if (result != RASPI_FAT_NOT_FOUND) {
            return result;
        }
        if (!raspi_fat_next_cluster(volume, cluster, &next, &end, errp)) {
            return RASPI_FAT_ERROR;
        }
        if (end) {
            return RASPI_FAT_NOT_FOUND;
        }
        cluster = next;
    }
    error_setg(errp, "FAT root directory cluster chain contains a loop");
    return RASPI_FAT_ERROR;
}

static void raspi_fat_lfn_reset(RaspiFatLfnState *lfn)
{
    memset(lfn, 0, sizeof(*lfn));
    memset(lfn->name, 0xff, sizeof(lfn->name) - sizeof(lfn->name[0]));
    lfn->name[ARRAY_SIZE(lfn->name) - 1] = 0;
}

static uint8_t raspi_fat_short_name_checksum(const uint8_t name[11])
{
    uint8_t checksum = 0;

    for (unsigned int i = 0; i < 11; i++) {
        checksum = ((checksum & 1) << 7) + (checksum >> 1) + name[i];
    }
    return checksum;
}

static bool raspi_fat_lfn_add(RaspiFatLfnState *lfn, const uint8_t *entry)
{
    static const uint8_t offsets[FAT_LFN_CHARS_PER_ENTRY] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
    };
    unsigned int order = entry[0] & 0x1f;

    if (entry[0] & 0x40) {
        raspi_fat_lfn_reset(lfn);
        if (!order || order > FAT_LFN_MAX_ENTRIES) {
            return false;
        }
        lfn->valid = true;
        lfn->expected_order = order;
        lfn->checksum = entry[13];
    }
    if (!lfn->valid || order != lfn->expected_order ||
        entry[13] != lfn->checksum) {
        raspi_fat_lfn_reset(lfn);
        return false;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++) {
        lfn->name[(order - 1) * FAT_LFN_CHARS_PER_ENTRY + i] =
            lduw_le_p(entry + offsets[i]);
    }
    lfn->expected_order--;
    return true;
}

static char *raspi_fat_lfn_name(const RaspiFatLfnState *lfn,
                                const uint8_t short_name[11])
{
    glong length = 0;

    if (!lfn->valid || lfn->expected_order ||
        raspi_fat_short_name_checksum(short_name) != lfn->checksum) {
        return NULL;
    }
    while (length < ARRAY_SIZE(lfn->name) && lfn->name[length] &&
           lfn->name[length] != 0xffff) {
        length++;
    }
    return g_utf16_to_utf8(lfn->name, length, NULL, NULL, NULL);
}

static char *raspi_fat_short_name(const uint8_t entry[32])
{
    GString *name = g_string_sized_new(12);
    bool lower_base = entry[12] & 0x08;
    bool lower_extension = entry[12] & 0x10;

    for (unsigned int i = 0; i < 8 && entry[i] != ' '; i++) {
        uint8_t value = i == 0 && entry[i] == 0x05 ? 0xe5 : entry[i];

        g_string_append_c(name, lower_base ? g_ascii_tolower(value) : value);
    }
    if (entry[8] != ' ') {
        g_string_append_c(name, '.');
        for (unsigned int i = 8; i < 11 && entry[i] != ' '; i++) {
            g_string_append_c(name, lower_extension ?
                                    g_ascii_tolower(entry[i]) : entry[i]);
        }
    }
    return g_string_free(name, false);
}

static RaspiFatResult raspi_fat_scan_path_chunk(
    const uint8_t *directory, size_t length, int64_t directory_offset,
    const char *component, RaspiFatLfnState *lfn, RaspiFatFile *file,
    bool *end)
{
    for (size_t entry_offset = 0;
         entry_offset + FAT_DIRECTORY_ENTRY_SIZE <= length;
         entry_offset += FAT_DIRECTORY_ENTRY_SIZE) {
        const uint8_t *entry = directory + entry_offset;
        g_autofree char *name = NULL;

        if (!entry[0]) {
            *end = true;
            return RASPI_FAT_NOT_FOUND;
        }
        if (entry[0] == 0xe5) {
            raspi_fat_lfn_reset(lfn);
            continue;
        }
        if (entry[11] == FAT_ATTR_LONG_NAME) {
            raspi_fat_lfn_add(lfn, entry);
            continue;
        }
        if (!(entry[11] & FAT_ATTR_VOLUME_ID)) {
            name = raspi_fat_lfn_name(lfn, entry);
            if (!name) {
                name = raspi_fat_short_name(entry);
            }
            if (!g_ascii_strcasecmp(name, component)) {
                file->first_cluster = lduw_le_p(entry + 26) |
                                      (uint32_t)lduw_le_p(entry + 20) << 16;
                file->size = ldl_le_p(entry + 28);
                file->directory_entry_offset =
                    directory_offset + entry_offset;
                file->attributes = entry[11];
                return RASPI_FAT_FOUND;
            }
        }
        raspi_fat_lfn_reset(lfn);
    }
    return RASPI_FAT_NOT_FOUND;
}

static RaspiFatResult raspi_fat_find_in_directory(
    RaspiFatVolume *volume, bool fixed_root, uint32_t first_cluster,
    const char *component, RaspiFatFile *file, Error **errp)
{
    RaspiFatLfnState lfn;
    uint32_t cluster = first_cluster;
    size_t cluster_bytes = (size_t)volume->bytes_per_sector *
                           volume->sectors_per_cluster;

    raspi_fat_lfn_reset(&lfn);
    if (fixed_root) {
        size_t root_bytes = (size_t)volume->root_entries *
                            FAT_DIRECTORY_ENTRY_SIZE;
        int64_t offset = volume->partition_offset +
                         (volume->reserved_sectors +
                          (uint64_t)volume->fat_count * volume->fat_sectors) *
                         volume->bytes_per_sector;
        g_autofree uint8_t *directory = g_malloc(root_bytes);
        bool end = false;

        if (!raspi_volume_read(volume, offset, root_bytes, directory, errp)) {
            return RASPI_FAT_ERROR;
        }
        return raspi_fat_scan_path_chunk(directory, root_bytes, offset,
                                         component, &lfn, file, &end);
    }

    for (uint32_t visited = 0; visited <= volume->cluster_count; visited++) {
        g_autofree uint8_t *directory = g_malloc(cluster_bytes);
        RaspiFatResult result;
        int64_t offset;
        uint32_t next;
        bool chain_end;
        bool directory_end = false;

        if (!raspi_fat_cluster_offset(volume, cluster, &offset, errp) ||
            !raspi_volume_read(volume, offset, cluster_bytes, directory,
                               errp)) {
            return RASPI_FAT_ERROR;
        }
        result = raspi_fat_scan_path_chunk(directory, cluster_bytes, offset,
                                           component, &lfn, file,
                                           &directory_end);
        if (result != RASPI_FAT_NOT_FOUND || directory_end) {
            return result;
        }
        if (!raspi_fat_next_cluster(volume, cluster, &next, &chain_end,
                                    errp)) {
            return RASPI_FAT_ERROR;
        }
        if (chain_end) {
            return RASPI_FAT_NOT_FOUND;
        }
        cluster = next;
    }
    error_setg(errp, "FAT directory cluster chain contains a loop");
    return RASPI_FAT_ERROR;
}

RaspiFatResult raspi_fat_find_path(RaspiFatVolume *volume, const char *path,
                                   RaspiFatFile *file, Error **errp)
{
    g_auto(GStrv) components = NULL;
    const char *relative = path;
    bool fixed_root = volume->fat_type != 32;
    uint32_t directory_cluster = volume->root_cluster;
    unsigned int depth = 0;

    memset(file, 0, sizeof(*file));
    if (!path || !path[0] || strlen(path) > FAT_PATH_MAX_LENGTH ||
        !g_utf8_validate(path, -1, NULL)) {
        error_setg(errp, "FAT path is empty, too long, or invalid UTF-8");
        return RASPI_FAT_ERROR;
    }
    while (*relative == '/') {
        relative++;
    }
    if (!relative[0]) {
        error_setg(errp, "FAT path does not name a file");
        return RASPI_FAT_ERROR;
    }
    components = g_strsplit(relative, "/", -1);

    for (char **component = components; *component; component++) {
        RaspiFatResult result;
        bool last = component[1] == NULL;

        depth++;
        if (depth > FAT_PATH_MAX_DEPTH || !(*component)[0] ||
            !strcmp(*component, ".") || !strcmp(*component, "..") ||
            g_utf8_strlen(*component, -1) > 255) {
            error_setg(errp, "FAT path contains an invalid component");
            return RASPI_FAT_ERROR;
        }
        result = raspi_fat_find_in_directory(
            volume, fixed_root, directory_cluster, *component, file, errp);
        if (result != RASPI_FAT_FOUND) {
            return result;
        }
        if (last) {
            return result;
        }
        if (!(file->attributes & FAT_ATTR_DIRECTORY) ||
            file->first_cluster < 2) {
            return RASPI_FAT_NOT_FOUND;
        }
        fixed_root = false;
        directory_cluster = file->first_cluster;
    }
    g_assert_not_reached();
}

uint8_t *raspi_fat_read_file(RaspiFatVolume *volume,
                             const RaspiFatFile *file, size_t maximum,
                             size_t *length, Error **errp)
{
    g_autofree uint8_t *data = NULL;
    g_autoptr(GHashTable) visited_clusters =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    size_t cluster_bytes = (size_t)volume->bytes_per_sector *
                           volume->sectors_per_cluster;
    size_t done = 0;
    uint32_t cluster = file->first_cluster;

    if (file->size > maximum) {
        error_setg(errp, "FAT file size %u exceeds limit %zu",
                   file->size, maximum);
        return NULL;
    }
    data = g_malloc(MAX(file->size, 1));
    if (!file->size) {
        *length = 0;
        return g_steal_pointer(&data);
    }
    for (uint32_t visited = 0; done < file->size; visited++) {
        int64_t offset;
        size_t chunk = MIN(cluster_bytes, file->size - done);
        uint32_t next;
        bool end;

        if (!g_hash_table_add(visited_clusters,
                              GUINT_TO_POINTER(cluster))) {
            error_setg(errp, "FAT file cluster chain contains a cycle at %u",
                       cluster);
            return NULL;
        }
        if (visited > volume->cluster_count ||
            !raspi_fat_cluster_offset(volume, cluster, &offset, errp) ||
            !raspi_volume_read(volume, offset, chunk, data + done, errp)) {
            return NULL;
        }
        done += chunk;
        if (done == file->size) {
            break;
        }
        if (!raspi_fat_next_cluster(volume, cluster, &next, &end, errp)) {
            return NULL;
        }
        if (end) {
            error_setg(errp, "FAT file cluster chain ends before file size");
            return NULL;
        }
        cluster = next;
    }
    *length = done;
    return g_steal_pointer(&data);
}

bool raspi_fat_rename_file(RaspiFatVolume *volume, const RaspiFatFile *file,
                           const char name[RASPI_FAT_SHORT_NAME_LEN],
                           Error **errp)
{
    if (volume->memory) {
        error_setg(errp, "Raspberry Pi memory boot image is read-only");
        return false;
    }
    if (!volume->blk) {
        error_setg(errp, "Raspberry Pi callback boot image is read-only");
        return false;
    }
    return raspi_media_write(volume->blk, file->directory_entry_offset,
                             RASPI_FAT_SHORT_NAME_LEN, name, errp);
}
