# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal deterministic FAT16 media builder for functional tests."""

from pathlib import Path
import struct


SECTOR_SIZE = 512
TOTAL_SECTORS = 131072
SECTORS_PER_CLUSTER = 4
RESERVED_SECTORS = 1
FAT_COUNT = 2
FAT_SECTORS = 128
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
DATA_SECTOR = RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS + ROOT_SECTORS
CLUSTER_SIZE = SECTOR_SIZE * SECTORS_PER_CLUSTER


def _short_checksum(short_name):
    checksum = 0
    for value in short_name:
        checksum = (((checksum & 1) << 7) +
                    (checksum >> 1) + value) & 0xff
    return checksum


def _lfn_entries(name, short_name):
    encoded = name.encode('utf-16-le')
    units = list(struct.unpack(f'<{len(encoded) // 2}H', encoded))
    units.append(0)
    while len(units) % 13:
        units.append(0xffff)

    offsets = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
    count = len(units) // 13
    checksum = _short_checksum(short_name)
    entries = []
    for sequence in range(count, 0, -1):
        entry = bytearray(b'\xff' * 32)
        entry[0] = sequence | (0x40 if sequence == count else 0)
        entry[11] = 0x0f
        entry[12] = 0
        entry[13] = checksum
        struct.pack_into('<H', entry, 26, 0)
        chunk = units[(sequence - 1) * 13:sequence * 13]
        for offset, value in zip(offsets, chunk):
            struct.pack_into('<H', entry, offset, value)
        entries.append(entry)
    return entries


class Fat16Image:
    """Build a 64 MiB FAT16 superfloppy without host filesystem tools."""

    def __init__(self):
        self.image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
        self.next_cluster = 2
        self.fat = [0xfff8, 0xffff]
        self.directory = []
        self.directories = {}
        self._write_boot_sector()

    def _write_boot_sector(self):
        boot = memoryview(self.image)[:SECTOR_SIZE]
        boot[0:3] = b'\xeb\x3c\x90'
        boot[3:11] = b'QEMURPI '
        struct.pack_into('<H', boot, 11, SECTOR_SIZE)
        boot[13] = SECTORS_PER_CLUSTER
        struct.pack_into('<H', boot, 14, RESERVED_SECTORS)
        boot[16] = FAT_COUNT
        struct.pack_into('<H', boot, 17, ROOT_ENTRIES)
        struct.pack_into('<H', boot, 19, 0)
        boot[21] = 0xf8
        struct.pack_into('<H', boot, 22, FAT_SECTORS)
        struct.pack_into('<H', boot, 24, 32)
        struct.pack_into('<H', boot, 26, 64)
        struct.pack_into('<I', boot, 28, 0)
        struct.pack_into('<I', boot, 32, TOTAL_SECTORS)
        boot[36] = 0x80
        boot[38] = 0x29
        struct.pack_into('<I', boot, 39, 0x52504934)
        boot[43:54] = b'RPIBOOT    '
        boot[54:62] = b'FAT16   '
        boot[510:512] = b'\x55\xaa'

    def _allocate(self, contents):
        clusters = max(1, (len(contents) + CLUSTER_SIZE - 1) // CLUSTER_SIZE)
        first_cluster = self.next_cluster
        for index in range(clusters):
            cluster = self.next_cluster
            self.next_cluster += 1
            if cluster >= 0xfff0:
                raise ValueError('FAT16 image has no free clusters')
            next_cluster = 0xffff if index + 1 == clusters else cluster + 1
            self.fat.append(next_cluster)
            source_offset = index * CLUSTER_SIZE
            chunk = contents[source_offset:source_offset + CLUSTER_SIZE]
            target_offset = ((DATA_SECTOR * SECTOR_SIZE) +
                             (cluster - 2) * CLUSTER_SIZE)
            if target_offset + len(chunk) > len(self.image):
                raise ValueError('FAT16 image has no free data space')
            self.image[target_offset:target_offset + len(chunk)] = chunk
        return first_cluster

    @staticmethod
    def _directory_short_name(name):
        encoded = name.upper().encode('ascii')
        if (not encoded or len(encoded) > 8 or
                any(value not in b'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_'
                    for value in encoded)):
            raise ValueError(f'directory {name!r} has no simple FAT name')
        return encoded.ljust(8) + b'   '

    def _append_entry(self, entries, name, short_name, attributes,
                      first_cluster, size):
        if len(short_name) != 11:
            raise ValueError('FAT short name must contain exactly 11 bytes')

        canonical = short_name[0:8].rstrip().decode('ascii')
        extension = short_name[8:11].rstrip().decode('ascii')
        if extension:
            canonical += '.' + extension
        if canonical.casefold() != name.casefold():
            entries.extend(_lfn_entries(name, short_name))

        entry = bytearray(32)
        entry[0:11] = short_name
        entry[11] = attributes
        struct.pack_into('<H', entry, 26, first_cluster)
        struct.pack_into('<I', entry, 28, size)
        entries.append(entry)

    def _ensure_directory(self, path):
        if not path:
            return self.directory
        parent_path = ''
        parent_entries = self.directory
        for component in path.split('/'):
            current_path = (f'{parent_path}/{component}' if parent_path
                            else component)
            if current_path not in self.directories:
                cluster = self._allocate(b'')
                entries = []
                short_name = self._directory_short_name(component)
                self._append_entry(parent_entries, component, short_name,
                                   0x10, cluster, 0)
                self.directories[current_path] = (cluster, entries)
            _, parent_entries = self.directories[current_path]
            parent_path = current_path
        return parent_entries

    def add_file(self, name, short_name, contents):
        short_name = short_name.encode('ascii')
        directory, separator, basename = name.rpartition('/')
        if not separator:
            basename = name
        entries = self._ensure_directory(directory)
        first_cluster = self._allocate(contents)
        self._append_entry(entries, basename, short_name, 0x20,
                           first_cluster, len(contents))

    def write(self, path):
        if len(self.directory) > ROOT_ENTRIES:
            raise ValueError('FAT root directory is full')

        fat_data = bytearray(FAT_SECTORS * SECTOR_SIZE)
        for index, value in enumerate(self.fat):
            struct.pack_into('<H', fat_data, index * 2, value)
        for number in range(FAT_COUNT):
            offset = (RESERVED_SECTORS + number * FAT_SECTORS) * SECTOR_SIZE
            self.image[offset:offset + len(fat_data)] = fat_data

        root_offset = ((RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS) *
                       SECTOR_SIZE)
        if len(self.directory) > ROOT_ENTRIES:
            raise ValueError('FAT root directory is full')
        for index, entry in enumerate(self.directory):
            offset = root_offset + index * 32
            self.image[offset:offset + 32] = entry
        for cluster, entries in self.directories.values():
            if len(entries) > CLUSTER_SIZE // 32:
                raise ValueError('FAT directory is full')
            directory_offset = ((DATA_SECTOR * SECTOR_SIZE) +
                                (cluster - 2) * CLUSTER_SIZE)
            for index, entry in enumerate(entries):
                offset = directory_offset + index * 32
                self.image[offset:offset + 32] = entry
        Path(path).write_bytes(self.image)


def create_fat16_image(path, files):
    """Create a FAT16 image from ``(name, short_name, bytes)`` tuples."""
    image = Fat16Image()
    for name, short_name, contents in files:
        image.add_file(name, short_name, contents)
    image.write(path)
