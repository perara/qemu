#!/usr/bin/env python3
#
# Functional test that boots a Linux kernel on a Raspberry Pi machine
# and checks the console
#
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import json
import lzma
import os
from pathlib import Path
import select
import shutil
import struct
import subprocess
import sys
import threading
import time

from qemu.machine.machine import AbnormalShutdown
from qemu_test import LinuxKernelTest, Asset
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test import skipIfMissingEnv, skipUnlessOperatingSystem
from qemu_test.fat16 import create_fat16_image


class Aarch64Raspi4Machine(LinuxKernelTest):

    """
    The kernel can be rebuilt using the kernel source referenced
    and following the instructions on the on:
    https://www.raspberrypi.org/documentation/linux/kernel/building.md
    """

    def _require_raw_gadget_modules(self):
        """Load the USB fixture modules or verify already-loaded instances."""
        dummy = Path('/sys/module/dummy_hcd')
        if dummy.is_dir():
            high_speed = (
                dummy / 'parameters/is_high_speed'
            ).read_text(encoding='ascii').strip()
            self.assertIn(high_speed, ('1', 'Y', 'y'),
                          'loaded dummy_hcd is not high-speed')
        else:
            subprocess.run(
                ['sudo', '-n', 'modprobe', 'dummy_hcd', 'is_high_speed=1'],
                check=True)
        if not Path('/sys/module/raw_gadget').is_dir():
            subprocess.run(
                ['sudo', '-n', 'modprobe', 'raw_gadget'], check=True)
    ASSET_KERNEL_20190215 = Asset(
        ('http://archive.raspberrypi.org/debian/'
         'pool/main/r/raspberrypi-firmware/'
         'raspberrypi-kernel_1.20230106-1_arm64.deb'),
        '56d5713c8f6eee8a0d3f0e73600ec11391144fef318b08943e9abd94c0a9baf7')

    ASSET_INITRD = Asset(
        ('https://github.com/groeck/linux-build-test/raw/'
         '86b2be1384d41c8c388e63078a847f1e1c4cb1de/rootfs/'
         'arm64/rootfs.cpio.gz'),
        '7c0b16d1853772f6f4c3ca63e789b3b9ff4936efac9c8a01fb0c98c05c7a7648')

    ASSET_INITRD_ARM32 = Asset(
        ('https://github.com/groeck/linux-build-test/raw/'
         '2eb0a73b5d5a28df3170c546ddaaa9757e1e0848/rootfs/'
         'arm/rootfs-armv7a.cpio.gz'),
        '2c8dbdb16ea7af2dfbcbea96044dde639fb07d09fd3c4fb31f2027ef71e55ddd')

    # These are production Raspberry Pi artifacts pinned at immutable commits.
    # The test places their exact bytes on boot media; it does not convert or
    # patch any of the binaries.
    ASSET_START4 = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/boot/start4.elf'),
        'aae4ed383f769109ce55543e369697cf70831303627f36664842b80b736af3e2')

    ASSET_FIXUP4 = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/boot/fixup4.dat'),
        '7e56642ee71fde5ffe3477dcc855ff5d9bd7e4badc1e6cac8ca1be91773bfea6')

    ASSET_KERNEL7L = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '3a232374735c2bc5b7188ba2dfc0cbba8fa30d97/boot/kernel7l.img'),
        '5074dd9bc3114aef32a348beed4d9d26d18eafe0a55fc77bc9f882063896e946')

    ASSET_BCM2711_DTB_ARM32 = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '3a232374735c2bc5b7188ba2dfc0cbba8fa30d97/'
         'boot/bcm2711-rpi-4-b.dtb'),
        'e0bbf3b7b8340dd812baa95f1b86642fed3db828ffd869915c79b9e9ccc1019b')

    ASSET_BCM2711_CM4_DTB_ARM32 = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '3a232374735c2bc5b7188ba2dfc0cbba8fa30d97/'
         'boot/bcm2711-rpi-cm4.dtb'),
        '915375d08ffe2290ab1346109eff59aa9f9b90b1903868508816c2cebc0d570e')

    ASSET_BCM2711_DTB = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/bcm2711-rpi-4-b.dtb'),
        '75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb')

    ASSET_BCM2711_CM4_DTB_CURRENT = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/bcm2711-rpi-cm4.dtb'),
        'dda5429988864e911304acae4c60c9866628bf8175fb6361ccbaceafedcd1c75')

    ASSET_OVERLAY_GPIO_LED = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/gpio-led.dtbo'),
        'f2a6730dfa98f90f08d46782c74af9e48db506f83ba91807d8ddc96994a83b58')

    ASSET_OVERLAY_I2C_GPIO = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/i2c-gpio.dtbo'),
        '6144f5353fb35faba981ff02b328b58438a760206fa81ed622cde9f6eb4c75b7')

    ASSET_OVERLAY_I2C_RTC = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/i2c-rtc.dtbo'),
        'ee5102827e7d500492918a9695236e8278e90da939ac127ee3cf47cbcf305912')

    ASSET_OVERLAY_MCP2515_CAN0 = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/mcp2515-can0.dtbo'),
        'e3ccb45591e7b61b9113249a8e7a5d43a3be8a6d029b22e99b32fae1b39aea79')

    ASSET_OVERLAY_MINIUART_BT = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/miniuart-bt.dtbo'),
        'c38c12945f14ac0c71b609a48c94566b3c323a9cfd39d8cb286799e87e14197e')

    ASSET_OVERLAY_PWM_2CHAN = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/pwm-2chan.dtbo'),
        '0639648a38cff7deae03a5d14f7dc389ca6c3a96083a8f4aa09202522692d74d')

    ASSET_OVERLAY_SPI0_1CS = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/spi0-1cs.dtbo'),
        '6f5bb9e4a73a7f1bebff621d0cc3132b0b9361bef56bf5bbcdacf047f98ffd4b')

    ASSET_OVERLAY_SPI1_1CS = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/spi1-1cs.dtbo'),
        '240db024743ae18c513d7e3f58390894307b9c28c5166e315ba9b7a089b98012')

    ASSET_OVERLAY_SPI2_1CS = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/spi2-1cs.dtbo'),
        '119dbc707396f96b2755cbdd8d126c67b4ac857aeeec4e30f6e4dcd77ed4c895')

    ASSET_OVERLAY_UART2 = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/uart2.dtbo'),
        '00745a2729b2fcff73b08cc9ad028148ed46648a93d7cfe08c51155f22b35119')

    ASSET_OVERLAY_W1_GPIO = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         '78e81e2cd6e00efeb79c169bff29ec457fa14b11/'
         'boot/overlays/w1-gpio.dtbo'),
        'cb2c36f9ae4a8fca087b40bb0c29fe67fdda2a7f546ceb97c3c189f1fae1ac5c')

    ASSET_PIEEPROM = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/rpi-eeprom/'
         'bade0f69d5e5add2610fc57db9ca42694443c98b/firmware-2711/'
         'default/pieeprom-2026-05-17.bin'),
        'f1da1bda48c8f19d6eccd94f160a2c8da48fd0acdbfa47f0ac58353208d188c3')

    ASSET_RPI_EEPROM_CONFIG = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/rpi-eeprom/'
         'bade0f69d5e5add2610fc57db9ca42694443c98b/'
         'rpi-eeprom-config'),
        '39895792eb724afe5a4ed39e5798db844292efcca4317228aa790c580ddbb70f')

    ASSET_OLD_PIEEPROM = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/rpi-eeprom/'
         'bade0f69d5e5add2610fc57db9ca42694443c98b/firmware-2711/'
         'old/stable/pieeprom-2026-04-14.bin'),
        '92fc67dfe624fb49a839ac1a168c23f20058e62b9f82efb0e27569cec857b2db')

    ASSET_RECOVERY = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/rpi-eeprom/'
         'bade0f69d5e5add2610fc57db9ca42694443c98b/firmware-2711/'
         'default/recovery.bin'),
        '9ec8816886f3938d962a837347d65ccc1d03e811a84bf1ad4b608906b288d995')

    PIEEPROM_BOOTSYS_SHA256 = \
        'a7f55570459356b214ad1e08b7d11b4c8008bb5200cb24da4195d7c21cb8f819'
    PIEEPROM_DEPENDENCIES_SHA256 = \
        'e68aa5392168fecf438527240d5e40b007cca648cc96097219f35f04cd9712f9'
    PIEEPROM_BOOTSYS_KEY_INDEX = 1
    PIEEPROM_DEPENDENCY_COUNT = 13
    PIEEPROM_BUILD_TIMESTAMP = 1779045198
    PIEEPROM_VERSION = '224877da'
    KERNEL8_SHA256 = \
        '55c355d076ffc9cc25ca26556f7a43509566ceed985e9233213c8fdcd9e8ef2b'
    BCM2711_DTB_SHA256 = \
        'e0bbf3b7b8340dd812baa95f1b86642fed3db828ffd869915c79b9e9ccc1019b'
    BCM2711_CM4_DTB_SHA256 = \
        '915375d08ffe2290ab1346109eff59aa9f9b90b1903868508816c2cebc0d570e'
    USBBOOT_GIT = '87d6e032'
    RPIBOOT_SHA256 = \
        'f7af1fd4977707ebaf393ca88390f9903a371e50f689bf2244b875310d0a6f06'
    RPI_IMAGER_VERSION = 'Raspberry Pi Imager v2.0.8'
    RPI_IMAGER_SHA256 = \
        'c7c8a8085212c10cbefb2eb1dce13c44f19c9ee540cea4764be7b0cd5bb3bf65'
    RPIOS_LITE_XZ_SHA256 = \
        'acff736ca7945e3b305f07cda4abdb870910e12634991da69783611756e381b3'
    RPIOS_LITE_RAW_SHA256 = \
        'e235fd24fc5f039c08daba7d3abc04aecc7313f979d16d2a3fdad29dd44c33a9'
    RPIOS_LITE_RAW_SIZE = 2_977_955_840
    RPIOS_LITE_DISK_ID = 0x041BBA91
    CM4_RELEASE_EMMC_SIZE = 4 * 1024 * 1024 * 1024
    DEFAULT_HOST_BOOT_IMAGE_SIZE = 32_505_856
    DEFAULT_HOST_BOOT_IMAGE_SHA256 = \
        '9f26719cc254d701ccc1ae654649e31db3f033f13984dc48bc3c0d9dfc12fe77'
    DEFAULT_HOST_START4_SHA256 = \
        '451e4320cd2390c28e5c0e79e6e5a704f98fa11670ba57d840d16bcae5aeec2e'
    DEFAULT_HOST_FIXUP4_SHA256 = \
        '21e37b0402974a64779c23621787a37de4706877b78931c94e964db864e8565d'
    DEFAULT_HOST_KERNEL_SHA256 = \
        '676004bcc396e217ae7c6a0a9b4a0d32acd0ba4a68714e80a703910fbc4853e9'
    DEFAULT_HOST_DTB_SHA256 = \
        '5bc13e0f663531c374cbf4645e511708e40a0774fc7771ae869c5504dc9e39b3'
    DEFAULT_HOST_INITRAMFS_SHA256 = \
        'ff58621ebc158a31dbd12e0fb6eff2291ee581dc9da6fc5e2f962dc93d67577e'
    RPIBOOT_FILE_SHA256 = {
        'bootcode4.bin':
            '796487521d9834111bf089bf24528c77d6c6f89a49ab9e89c514d70f16cce694',
        'config.txt':
            'f74a9db07cd32f418e25754134840b138486e39cda2a4e536042b86c5c67b687',
        'boot.img':
            'a434c8d53c5ebb8a64ea4991a2b8e22f99516ac9669e892dad7065ea929857c0',
    }

    def _prepare_cm4_production_media(self, emmc_path, eeprom_path,
                                      arm32=False):
        """Create CM4 media around unchanged pinned production artifacts."""
        if arm32:
            kernel_path = self.ASSET_KERNEL7L.fetch()
            dtb_path = self.ASSET_BCM2711_CM4_DTB_ARM32.fetch()
        else:
            kernel_path = self.archive_extract(
                self.ASSET_KERNEL_20190215, member='boot/kernel8.img')
            dtb_path = self.archive_extract(
                self.ASSET_KERNEL_20190215,
                member='boot/bcm2711-rpi-cm4.dtb')
        start4_path = self.ASSET_START4.fetch()
        fixup4_path = self.ASSET_FIXUP4.fetch()
        source_eeprom_path = self.ASSET_PIEEPROM.fetch()
        recovery_path = self.ASSET_RECOVERY.fetch()
        if arm32:
            command_line = (
                'earlycon=pl011,mmio32,0xfe201000 '
                'console=ttyAMA0,115200 panic=-1 noreboot '
                'rdinit=/sbin/init')
            kernel_name = ('kernel7l.img', 'KERNEL7LIMG')
            config = (
                b'[pi4]\nkernel=missing.img\n'
                b'[cm4]\nkernel=kernel7l.img\n'
                b'arm_64bit=0\nauto_initramfs=1\n')
            initramfs = (
                'initramfs7l', 'INITRA~1   ',
                Path(self.ASSET_INITRD_ARM32.fetch()).read_bytes())
        else:
            command_line = (
                self.KERNEL_COMMON_COMMAND_LINE +
                'earlycon=pl011,mmio32,0xfe201000 ' +
                'console=ttyAMA0,115200 panic=-1 noreboot ' +
                'rdinit=/sbin/init dwc_otg.fiq_fsm_enable=0')
            kernel_name = ('kernel8.img', 'KERNEL8 IMG')
            config = (
                b'[pi4]\nkernel=missing.img\n'
                b'[cm4]\nkernel=kernel8.img\n'
                b'arm_64bit=1\nauto_initramfs=1\n')
            initramfs = (
                'initramfs8', 'INITRA~1   ',
                Path(self.ASSET_INITRD.fetch()).read_bytes())

        files = [
            ('start4.elf', 'START4  ELF',
             Path(start4_path).read_bytes()),
            ('fixup4.dat', 'FIXUP4  DAT',
             Path(fixup4_path).read_bytes()),
            (kernel_name[0], kernel_name[1], Path(kernel_path).read_bytes()),
            ('bcm2711-rpi-cm4.dtb', 'BCM271~1DTB',
             Path(dtb_path).read_bytes()),
            # CM4 ROM must not execute recovery.bin from soldered eMMC.
            ('recovery.bin', 'RECOVERYBIN',
             Path(recovery_path).read_bytes()),
            ('config.txt', 'CONFIG  TXT', config),
            ('cmdline.txt', 'CMDLINE TXT', command_line.encode() + b'\n'),
        ]
        files.insert(4, initramfs)
        create_fat16_image(emmc_path, files)
        Path(eeprom_path).write_bytes(Path(source_eeprom_path).read_bytes())
        return command_line

    def _configure_cm4_boot_vm(self, vm, emmc_path, eeprom_path,
                               provision_state=None, no_reboot=True,
                               console_index=0, emmc_boot_path=None,
                               emmc_rpmb_path=None, emmc_cid=None,
                               emmc_data_error=None,
                               emmc_data_error_after=0,
                               emmc_data_error_count=1,
                               provision_recover_stale=False):
        machine = ('raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,'
                   'emmc-drive=emmc')
        if provision_state:
            machine += f',provision-state-file={provision_state}'
        if provision_recover_stale:
            machine += ',provision-recover-stale=on'
        if emmc_boot_path:
            machine += ',emmc-boot-drive=emmcboot'
        if emmc_rpmb_path:
            machine += ',emmc-rpmb-drive=emmcrpmb'
        if emmc_cid:
            machine += f',emmc-cid={emmc_cid}'
        if emmc_data_error:
            machine += (f',emmc-data-error={emmc_data_error},'
                        f'emmc-data-error-after={emmc_data_error_after},'
                        f'emmc-data-error-count={emmc_data_error_count}')
        vm.set_machine(machine)
        vm.set_console(console_index=console_index)
        vm.add_args(
            '-drive', f'if=none,id=pieeprom,format=raw,file={eeprom_path}',
            '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}')
        if emmc_boot_path:
            vm.add_args(
                '-drive', ('if=none,id=emmcboot,format=raw,'
                           f'file={emmc_boot_path}'))
        if emmc_rpmb_path:
            vm.add_args(
                '-drive', ('if=none,id=emmcrpmb,format=raw,'
                           f'file={emmc_rpmb_path}'))
        if no_reboot:
            vm.add_args('-no-reboot')

    @staticmethod
    def _sha256_prefix(path, length):
        digest = hashlib.sha256()
        with Path(path).open('rb') as image:
            remaining = length
            while remaining:
                chunk = image.read(min(4 * 1024 * 1024, remaining))
                if not chunk:
                    raise AssertionError('image ended before expected size')
                digest.update(chunk)
                remaining -= len(chunk)
        return digest.hexdigest()

    @staticmethod
    def _sha256_file(path):
        digest = hashlib.sha256()
        with Path(path).open('rb') as stream:
            while chunk := stream.read(4 * 1024 * 1024):
                digest.update(chunk)
        return digest.hexdigest()

    def _assert_release_image_identities(self, image):
        inspector = (Path(__file__).resolve().parents[3] /
                     'contrib/raspi4/rpi_image.py')
        run = subprocess.run(
            [sys.executable, str(inspector), 'inspect', str(image),
             '--validate-contained-identities'],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(
            run.returncode, 0,
            f'raw release identity validation failed:\n{run.stderr}')
        report = json.loads(run.stdout)
        self.assertTrue(report['identity_validation']['valid'])
        partitions = report['partitions']
        self.assertEqual(
            [partition['partition_uuid'] for partition in partitions],
            ['041bba91-01', '041bba91-02'])
        self.assertEqual(
            [reference['partition_indexes'] for reference in
             report['identity_validation']['references']],
            [[2], [1], [2]])

    @staticmethod
    def _xz_payload_identity(path):
        digest = hashlib.sha256()
        size = 0
        with lzma.open(path, 'rb') as image:
            while chunk := image.read(4 * 1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        return size, digest.hexdigest()

    @staticmethod
    def _mbr_state(path):
        with Path(path).open('rb') as image:
            mbr = image.read(512)
        if len(mbr) != 512 or mbr[510:512] != b'\x55\xaa':
            raise AssertionError('flashed release has no valid MBR')
        disk_id = struct.unpack_from('<I', mbr, 440)[0]
        partition2 = mbr[462:478]
        first_lba, sectors = struct.unpack_from('<II', partition2, 8)
        return disk_id, first_lba, sectors

    @staticmethod
    def _ext4_block_count(path, partition_lba):
        with Path(path).open('rb') as image:
            image.seek(partition_lba * 512 + 1024 + 4)
            value = image.read(4)
        if len(value) != 4:
            raise AssertionError('root filesystem superblock is truncated')
        return struct.unpack('<I', value)[0]

    def _exercise_rpiboot_sigkill(self, raw_gadget, rpiboot, boot_dir,
                                  lifecycle, fault, fault_file=None):
        """Kill Raw Gadget at an exact active-transfer byte boundary."""
        fault_after = 4096
        capture = Path(self.workdir) / f'rpiboot-{fault}-capture'
        capture.mkdir()
        command = [
            'sudo', '-n', str(raw_gadget),
            '--capture-dir', str(capture),
            '--lifecycle', str(lifecycle), '--mass-storage',
            '--fault', fault, '--fault-after', str(fault_after),
        ]
        if fault_file:
            command.extend(['--fault-file', fault_file])
        helper = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        host = None
        output = ''

        def wait_for_output(marker, timeout):
            nonlocal output
            deadline = time.monotonic() + timeout
            while marker not in output and time.monotonic() < deadline:
                if helper.poll() is not None:
                    remainder = helper.communicate()[0]
                    self.fail(
                        f'Raw Gadget exited before {marker!r}:\n'
                        f'{output}{remainder}')
                readable, _, _ = select.select([helper.stdout], [], [], 0.5)
                if readable:
                    line = helper.stdout.readline()
                    if line:
                        output += line
            self.assertIn(marker, output,
                          f'Raw Gadget did not reach {marker!r}:\n{output}')

        try:
            wait_for_output('waiting for unmodified rpiboot', 10)
            host = subprocess.Popen(
                ['sudo', '-n', 'timeout', '30s', str(rpiboot),
                 '-d', str(boot_dir)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, start_new_session=True)
            target = fault_file or 'bootcode4.bin'
            marker = (f'holding RPIBOOT {target} transfer after '
                      f'{fault_after} bytes for external termination')
            wait_for_output(marker, 20)
            self.assertEqual((capture / target).stat().st_size, fault_after)
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-active')

            subprocess.run(
                ['sudo', '-n', 'kill', '-KILL', '--', f'-{helper.pid}'],
                check=True, capture_output=True, text=True, timeout=10)
            output += helper.communicate(timeout=10)[0]
            host_output = host.communicate(timeout=35)[0]
            host_reported_failure = (
                host.returncode != 0 or
                'Failed to write complete file to USB device' in host_output)
            self.assertTrue(
                host_reported_failure,
                f'rpiboot accepted SIGKILL during {fault}:\n{host_output}')
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-active')

            blocked = subprocess.run(
                ['sudo', '-n', str(raw_gadget),
                 '--capture-dir', str(capture / 'unsafe-retry'),
                 '--lifecycle', str(lifecycle), '--mass-storage'],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn("lifecycle state 'rpiboot-active'", blocked.stderr)
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-active')

            recovery = subprocess.run(
                ['sudo', '-n', str(raw_gadget),
                 '--lifecycle', str(lifecycle), '--recover-stale'],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(
                recovery.returncode, 0,
                f'cannot recover killed {fault}:\n'
                f'{recovery.stdout}{recovery.stderr}')
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-failed')
        finally:
            for process in (helper, host):
                if process is not None and process.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{process.pid}'],
                        check=False, capture_output=True, text=True)
                    process.communicate(timeout=10)

    def _exercise_rpiboot_usb_resets(self, raw_gadget, rpiboot, boot_dir,
                                     lifecycle, reset_cycles=4):
        """Reset both active RPIBOOT enumerations and complete one helper."""
        capture = Path(self.workdir) / 'rpiboot-reset-capture'
        capture.mkdir()
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_gadget),
             '--capture-dir', str(capture),
             '--lifecycle', str(lifecycle), '--mass-storage',
             '--fault', 'reset-reenumerate', '--fault-after', '4096',
             '--fault-file', 'boot.img',
             '--fault-count', str(reset_cycles)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        helper_lines = []

        def drain_helper():
            for line in helper.stdout:
                helper_lines.append(line)

        drain = threading.Thread(
            target=drain_helper, name='rpiboot-reset-output-drain',
            daemon=True)
        drain.start()
        hosts = []
        host_outputs = []

        def start_host():
            process = subprocess.Popen(
                ['sudo', '-n', 'timeout', '45s', str(rpiboot),
                 '-d', str(boot_dir)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, start_new_session=True)
            hosts.append(process)
            return process

        host = start_host()

        def wait_for_marker(marker, expected_count, timeout, restart_host):
            nonlocal host
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                output = ''.join(helper_lines)
                if output.count(marker) >= expected_count:
                    return output
                if helper.poll() is not None:
                    self.fail(
                        f'Raw Gadget exited before {marker!r}:\n{output}')
                if host.poll() is not None:
                    host_outputs.append(host.communicate()[0])
                    if restart_host:
                        host = start_host()
                        restart_host = False
                    else:
                        self.fail(
                            f'rpiboot exited before {marker!r}:\n'
                            f'{host_outputs[-1]}')
                time.sleep(0.02)
            self.fail(
                f'Raw Gadget did not reach {marker!r}:\n'
                f'{"".join(helper_lines)}')

        try:
            for reset_index in range(reset_cycles):
                wait_for_marker(
                    'waiting for host USB reset during bootcode4.bin after '
                    '4096 bytes', reset_index + 1, 40,
                    reset_index != 0)
                self.assertEqual(
                    (capture / 'bootcode4.bin').stat().st_size, 4096)
                reset = subprocess.run(
                    ['sudo', '-n', 'usbreset', '0a5c:2711'],
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(
                    reset.returncode, 0,
                    f'ROM-stage USB reset {reset_index + 1} failed:\n'
                    f'{reset.stdout}{reset.stderr}')
                wait_for_marker(
                    'observed host USB reset during bootcode4.bin',
                    reset_index + 1, 10, True)

            for reset_index in range(reset_cycles):
                wait_for_marker(
                    'waiting for host USB reset during boot.img after '
                    '4096 bytes', reset_index + 1, 50, True)
                self.assertEqual((capture / 'boot.img').stat().st_size, 4096)
                reset = subprocess.run(
                    ['sudo', '-n', 'usbreset', '0a5c:2711'],
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(
                    reset.returncode, 0,
                    f'file-server USB reset {reset_index + 1} failed:\n'
                    f'{reset.stdout}{reset.stderr}')
                wait_for_marker(
                    'observed host USB reset during boot.img',
                    reset_index + 1, 10, True)

            wait_for_marker('RPIBOOT transport complete', 1, 60, True)
            helper.wait(timeout=10)
            drain.join(timeout=10)
            self.assertFalse(drain.is_alive())
            helper.stdout.close()
            if host.poll() is None:
                host_outputs.append(host.communicate(timeout=20)[0])
            else:
                host_outputs.append(host.communicate()[0])
            helper_output = ''.join(helper_lines)
            self.assertEqual(helper.returncode, 0, helper_output)
            self.assertEqual(
                helper_output.count(
                    'waiting for host USB reset during bootcode4.bin'),
                reset_cycles)
            self.assertEqual(
                helper_output.count(
                    'observed host USB reset during bootcode4.bin'),
                reset_cycles)
            self.assertEqual(
                helper_output.count(
                    'waiting for host USB reset during boot.img'),
                reset_cycles)
            self.assertEqual(
                helper_output.count(
                    'observed host USB reset during boot.img'),
                reset_cycles)
            self.assertIn(self.USBBOOT_GIT, ''.join(host_outputs))
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-complete')
            for name in self.RPIBOOT_FILE_SHA256:
                self.assertEqual((capture / name).read_bytes(),
                                 (boot_dir / name).read_bytes())
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-KILL', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            helper.wait(timeout=10)
            drain.join(timeout=10)
            if helper.stdout and not helper.stdout.closed:
                helper.stdout.close()
            for process in hosts:
                if process.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{process.pid}'],
                        check=False, capture_output=True, text=True)
                process.communicate(timeout=10)

    def _exercise_rpiboot_control_timeout(self, raw_gadget, rpiboot,
                                          boot_dir, lifecycle, fault,
                                          fault_file=None):
        """Withhold one control response beyond rpiboot's 20 s timeout."""
        capture = Path(self.workdir) / f'rpiboot-{fault}-capture'
        capture.mkdir()
        command = [
            'sudo', '-n', str(raw_gadget),
            '--capture-dir', str(capture),
            '--lifecycle', str(lifecycle), '--mass-storage',
            '--fault', fault,
        ]
        if fault_file:
            command.extend(['--fault-file', fault_file])
        helper = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        host = None
        started = time.monotonic()
        try:
            time.sleep(0.5)
            self.assertIsNone(helper.poll(), 'Raw Gadget timeout helper exited')
            host = subprocess.Popen(
                ['sudo', '-n', 'timeout', '35s', str(rpiboot),
                 '-d', str(boot_dir)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, start_new_session=True)
            helper_output = helper.communicate(timeout=30)[0]
            elapsed = time.monotonic() - started
            self.assertEqual(helper.returncode, 75, helper_output)
            self.assertGreaterEqual(
                elapsed, 20.0,
                f'{fault} did not span the official host read timeout')
            self.assertIn('withholding RPIBOOT', helper_output)
            self.assertIn('control timeout completed', helper_output)
            self.assertNotIn('RPIBOOT transport complete', helper_output)
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-failed')

            if host.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{host.pid}'],
                    check=False, capture_output=True, text=True, timeout=10)
            host_output = host.communicate(timeout=10)[0]
            self.assertIn(
                self.USBBOOT_GIT, host_output,
                f'timeout did not exercise pinned official rpiboot:\n'
                f'{host_output}')
            self.assertEqual((capture / 'bootcode4.bin').read_bytes(),
                             (boot_dir / 'bootcode4.bin').read_bytes())
            if fault_file:
                self.assertEqual((capture / 'config.txt').read_bytes(),
                                 (boot_dir / 'config.txt').read_bytes())
                self.assertFalse((capture / fault_file).exists())
        finally:
            for process in (helper, host):
                if process is not None and process.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{process.pid}'],
                        check=False, capture_output=True, text=True)
                    process.communicate(timeout=10)
    def _exercise_raw_msd_scsi_fault(self, raw_msd, mass_storage,
                                     lifecycle, fault, cdb,
                                     data_out=False):
        """Prove one CDB fails while the raw BOT USB disk stays usable."""
        sg_raw = shutil.which('sg_raw')
        if not sg_raw:
            self.fail('sg_raw from sg3_utils is required for SCSI fault gates')

        safe_name = fault.replace(':', '-').replace('/', '-')
        backend = Path(self.workdir) / f'cm4-raw-msd-{safe_name}.img'
        with backend.open('wb') as target:
            target.truncate(64 * 1024 * 1024)
        stable = Path(
            '/dev/disk/by-id/'
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_'
            '51554d5552504934-0:0')
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_msd), '--image', str(backend),
             '--lifecycle', str(lifecycle), '--fault', fault,
             '--fault-after', '1'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        ready_output = ''
        fault_completed = False
        try:
            while 'CM4 raw BOT target ready' not in ready_output:
                line = helper.stdout.readline()
                if not line:
                    output = helper.communicate(timeout=5)[0]
                    self.fail(
                        'Raw BOT helper exited before readiness:\n'
                        f'{ready_output}{output}')
                ready_output += line
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not stable.exists():
                time.sleep(0.05)
            self.assertTrue(stable.exists(),
                            'raw BOT target did not create stable USB disk')
            block = stable.resolve(strict=True)
            sg_dir = (Path('/sys/class/block') / block.name /
                      'device/scsi_generic')
            deadline = time.monotonic() + 10
            sg_devices = []
            while time.monotonic() < deadline:
                sg_devices = list(sg_dir.iterdir()) if sg_dir.is_dir() else []
                if len(sg_devices) == 1:
                    break
                time.sleep(0.05)
            self.assertEqual(len(sg_devices), 1,
                             'raw BOT target has no unique SCSI generic node')

            competitor = subprocess.run(
                ['sudo', '-n', sys.executable, str(mass_storage),
                 '--lifecycle', str(lifecycle), 'status'],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(
                competitor.returncode, 0,
                'configfs helper raced the active raw BOT owner')
            self.assertIn('owned by another process', competitor.stderr)

            command = ['sudo', '-n', sg_raw, '-v']
            if data_out:
                command += ['-s', '512', '-i', '/dev/zero']
            command += [f'/dev/{sg_devices[0].name}', *cdb]
            result = subprocess.run(
                command, capture_output=True, text=True, timeout=15)
            combined = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0,
                                f'SCSI fault was not reported:\n{combined}')
            self.assertIn('Medium Error', combined)
            self.assertIn('Write error', combined)
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'flash-failed')
            self.assertTrue(stable.exists(),
                            'command-specific SCSI fault disconnected USB')
            readback = subprocess.run(
                ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                 'bs=512', 'count=1', 'status=none'],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(
                readback.returncode, 0,
                'ordinary read failed after command-specific SCSI fault')
            fault_completed = True
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            remaining_output = helper.communicate(timeout=10)[0]
            if fault_completed:
                self.assertIn('(injected)', ready_output + remaining_output)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and stable.exists():
                time.sleep(0.05)
            self.assertFalse(stable.exists(),
                             'raw BOT USB disk remained after helper exit')

    def _exercise_raw_msd_transport_recovery(self, raw_msd, fault,
                                             class_reset_count=0,
                                             reset_storm=False):
        """Recover one BOT transport fault through the host reset path."""
        sg_inq = shutil.which('sg_inq')
        if not sg_inq:
            self.fail('sg_inq from sg3_utils is required for BOT reset gate')
        sg_reset = shutil.which('sg_reset')
        if class_reset_count and not sg_reset:
            self.fail('sg_reset from sg3_utils is required for BOT reset gate')

        fault_name = ('phase error' if fault == 'bot-phase' else
                      'command timeout')
        backend = Path(self.workdir) / f'cm4-raw-msd-{fault}.img'
        with backend.open('wb') as target:
            target.truncate(64 * 1024 * 1024)
        stable = Path(
            '/dev/disk/by-id/'
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_'
            '51554d5552504934-0:0')
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_msd), '--image', str(backend),
             '--fault', fault, '--fault-after', '1'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        ready_output = ''
        recovery_completed = False
        try:
            while 'CM4 raw BOT target ready' not in ready_output:
                line = helper.stdout.readline()
                if not line:
                    output = helper.communicate(timeout=5)[0]
                    self.fail(
                        'Raw BOT helper exited before reset probe:\n'
                        f'{ready_output}{output}')
                ready_output += line

            deadline = time.monotonic() + 45
            while time.monotonic() < deadline and not stable.exists():
                if helper.poll() is not None:
                    break
                time.sleep(0.05)
            self.assertIsNone(helper.poll(),
                              'raw BOT helper exited during USB reset')
            self.assertTrue(
                stable.exists(),
                'stable raw BOT disk did not return after USB reset')
            block = stable.resolve(strict=True)
            sg_dir = (Path('/sys/class/block') / block.name /
                      'device/scsi_generic')
            deadline = time.monotonic() + 10
            sg_devices = []
            while time.monotonic() < deadline:
                sg_devices = list(sg_dir.iterdir()) if sg_dir.is_dir() else []
                if len(sg_devices) == 1:
                    break
                time.sleep(0.05)
            self.assertEqual(len(sg_devices), 1,
                             'reset target has no unique SCSI generic node')
            inquiry = subprocess.run(
                ['sudo', '-n', sg_inq, f'/dev/{sg_devices[0].name}'],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(
                inquiry.returncode, 0,
                f'INQUIRY failed after USB reset:\n'
                f'{inquiry.stdout}{inquiry.stderr}')
            readback = subprocess.run(
                ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                 'bs=4096', 'count=1', 'status=none'],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(readback.returncode, 0,
                             f'block read failed after BOT {fault_name}')
            for reset_index in range(class_reset_count):
                class_reset = subprocess.run(
                    ['sudo', '-n', sg_reset, '--device', '--no-escalate',
                     f'/dev/{sg_devices[0].name}'],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(
                    class_reset.returncode, 0,
                    f'BOT class reset {reset_index + 1} failed:\n'
                    f'{class_reset.stdout}{class_reset.stderr}')
                if not reset_storm:
                    readback = subprocess.run(
                        ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                         'bs=4096', 'count=1', 'status=none'],
                        capture_output=True, text=True, timeout=15)
                    self.assertEqual(
                        readback.returncode, 0,
                        f'block read failed after reset {reset_index + 1}')
            if reset_storm:
                inquiry = subprocess.run(
                    ['sudo', '-n', sg_inq, f'/dev/{sg_devices[0].name}'],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(
                    inquiry.returncode, 0,
                    'INQUIRY failed after BOT class-reset storm')
                readback = subprocess.run(
                    ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                     'bs=4096', 'count=1', 'status=none'],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(
                    readback.returncode, 0,
                    'block read failed after BOT class-reset storm')
            recovery_completed = True
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            remaining_output = helper.communicate(timeout=10)[0]
            output = ready_output + remaining_output
            if recovery_completed:
                self.assertIn(f'injected BOT {fault_name}', output)
                self.assertIn(
                    'USB bus reset completed host reconfiguration', output)
                self.assertIn(
                    f'USB bus reset recovered BOT {fault_name}', output)
                self.assertGreaterEqual(output.count(
                    'received BOT Mass Storage Reset'), class_reset_count)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and stable.exists():
                time.sleep(0.05)
            self.assertFalse(stable.exists(),
                             'reset-probe USB disk remained after helper exit')

    def _exercise_raw_msd_data_reset(self, raw_msd, write=False,
                                     reset_cycles=4):
        """Repeatedly reset active READ/WRITE(16) after one data block."""
        sg_raw = shutil.which('sg_raw')
        sg_inq = shutil.which('sg_inq')
        sg_reset = shutil.which('sg_reset')
        if not sg_raw or not sg_inq or not sg_reset:
            self.fail('sg3_utils raw, inquiry, and reset tools are required')

        direction = 'WRITE' if write else 'READ'
        fault = 'bot-write-reset' if write else 'bot-data-reset'
        fault_bytes = 128 if write else 512
        backend = Path(self.workdir) / (
            f'cm4-raw-msd-{direction.lower()}-reset.img')
        with backend.open('wb') as target:
            target.truncate(64 * 1024 * 1024)
        payload = Path(self.workdir) / 'cm4-raw-msd-write-reset.bin'
        payload_bytes = bytes([0xa5]) * 4096
        if write:
            payload.write_bytes(payload_bytes)
        stable = Path(
            '/dev/disk/by-id/'
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_'
            '51554d5552504934-0:0')
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_msd), '--image', str(backend),
             '--fault', fault, '--fault-after', '1',
             '--fault-count', str(reset_cycles),
             '--fault-bytes', str(fault_bytes)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        command = None
        helper_output = ''
        recovery_completed = False

        def wait_for_sg():
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    block = stable.resolve(strict=True)
                except FileNotFoundError:
                    time.sleep(0.05)
                    continue
                sg_dir = (Path('/sys/class/block') / block.name /
                          'device/scsi_generic')
                sg_devices = list(sg_dir.iterdir()) if sg_dir.is_dir() else []
                if len(sg_devices) == 1:
                    return f'/dev/{sg_devices[0].name}'
                time.sleep(0.05)
            self.fail('data-reset target has no stable SCSI generic node')

        try:
            while 'CM4 raw BOT target ready' not in helper_output:
                line = helper.stdout.readline()
                if not line:
                    output = helper.communicate(timeout=5)[0]
                    self.fail(
                        'Raw BOT helper exited before data-reset probe:\n'
                        f'{helper_output}{output}')
                helper_output += line
            injected = (
                f'injected BOT {direction} data-phase reset after '
                f'{fault_bytes} bytes')
            data_args = (['-s', '4096', '-i', str(payload)] if write else
                         ['-r', '4096', '-o', '/dev/null'])
            opcode = '8a' if write else '88'

            for reset_index in range(reset_cycles):
                sg_path = wait_for_sg()
                lba = reset_index * 8
                cdb = [
                    opcode, '00',
                    *(f'{byte:02x}' for byte in lba.to_bytes(8, 'big')),
                    *(f'{byte:02x}' for byte in (8).to_bytes(4, 'big')),
                    '00', '00',
                ]
                command = subprocess.Popen(
                    ['sudo', '-n', sg_raw, '-t', '30', *data_args,
                     sg_path, *cdb],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, start_new_session=True)
                deadline = time.monotonic() + 10
                while (helper_output.count(injected) < reset_index + 1 and
                       time.monotonic() < deadline):
                    readable, _, _ = select.select(
                        [helper.stdout], [], [], 1)
                    if readable:
                        line = helper.stdout.readline()
                        if not line:
                            break
                        helper_output += line
                    if command.poll() is not None:
                        break
                self.assertGreaterEqual(
                    helper_output.count(injected), reset_index + 1,
                    f'{direction}(16) reset cycle {reset_index + 1} did not '
                    'reach its data boundary')

                reset = subprocess.run(
                    ['sudo', '-n', sg_reset, '--device', '--no-escalate',
                     sg_path], capture_output=True, text=True, timeout=20)
                self.assertEqual(
                    reset.returncode, 0,
                    f'active-command reset {reset_index + 1} failed:\n'
                    f'{reset.stdout}{reset.stderr}')
                command_output = command.communicate(timeout=35)[0]
                self.assertNotEqual(
                    command.returncode, 0,
                    f'reset {direction}(16) cycle {reset_index + 1} '
                    'unexpectedly completed')
                self.assertIn('transport error', command_output)
                command = None

                if write:
                    with backend.open('rb') as media:
                        media.seek(reset_index * len(payload_bytes))
                        partial = media.read(len(payload_bytes))
                    self.assertEqual(
                        partial[:fault_bytes], payload_bytes[:fault_bytes],
                        f'WRITE reset {reset_index + 1} lost its durable '
                        'sub-sector prefix')
                    self.assertEqual(
                        partial[fault_bytes:],
                        bytes(4096 - fault_bytes),
                        f'WRITE reset {reset_index + 1} modified bytes after '
                        'the configured cutoff')

                recovered_sg = wait_for_sg()
                inquiry = subprocess.run(
                    ['sudo', '-n', sg_inq, recovered_sg],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(
                    inquiry.returncode, 0,
                    f'INQUIRY failed after data reset {reset_index + 1}')
                readback = subprocess.run(
                    ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                     'bs=4096', 'count=1', 'status=none'],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(
                    readback.returncode, 0,
                    f'read failed after data reset {reset_index + 1}')
            recovery_completed = True
        finally:
            if command is not None and command.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{command.pid}'],
                    check=False, capture_output=True, text=True)
                command.communicate(timeout=10)
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            remaining_output = helper.communicate(timeout=10)[0]
            helper_output += remaining_output
            if recovery_completed:
                self.assertEqual(
                    helper_output.count(
                        f'recovered BOT {direction} data-phase reset after '
                        f'{fault_bytes} bytes'),
                    reset_cycles)
                self.assertGreaterEqual(
                    helper_output.count('received BOT Mass Storage Reset'),
                    reset_cycles)
                self.assertGreaterEqual(
                    helper_output.count(
                        'USB bus reset completed host reconfiguration'),
                    reset_cycles)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and stable.exists():
                time.sleep(0.05)
            self.assertFalse(stable.exists(),
                             'data-reset USB disk remained after helper exit')

    def _exercise_raw_msd_malformed_cbw(self, raw_msd, bot_probe):
        """Prove all BOT cases plus invalid-CBW ordered Reset Recovery."""
        sg_inq = shutil.which('sg_inq')
        if not sg_inq:
            self.fail('sg_inq from sg3_utils is required for BOT probe')

        backend = Path(self.workdir) / 'cm4-raw-msd-invalid-cbw.img'
        with backend.open('wb') as target:
            target.truncate(64 * 1024 * 1024)
        backend_digest = hashlib.sha256(backend.read_bytes()).digest()
        stable = Path(
            '/dev/disk/by-id/'
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_'
            '51554d5552504934-0:0')
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_msd), '--image', str(backend)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        helper_lines = []
        helper_ready = threading.Event()

        def drain_helper_output():
            for line in helper.stdout:
                helper_lines.append(line)
                if 'CM4 raw BOT target ready' in line:
                    helper_ready.set()

        helper_drain = threading.Thread(
            target=drain_helper_output, name='cm4-bot-output-drain',
            daemon=True)
        helper_drain.start()
        helper_output = ''
        probe_completed = False
        try:
            self.assertTrue(
                helper_ready.wait(10),
                'Raw BOT helper did not announce readiness:\n'
                f'{"".join(helper_lines)}')
            self.assertIsNone(
                helper.poll(), 'Raw BOT helper exited after readiness:\n'
                f'{"".join(helper_lines)}')
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not stable.exists():
                time.sleep(0.05)
            self.assertTrue(stable.exists(),
                            'invalid-CBW target did not enumerate')
            self._assert_raw_msd_official_composite(stable)

            probe = subprocess.run(
                ['sudo', '-n', str(bot_probe)], capture_output=True,
                text=True, timeout=30)
            probe_stdout, probe_stderr = probe.stdout, probe.stderr
            if probe.returncode:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
                helper.wait(timeout=10)
                helper_drain.join(timeout=10)
                helper_output = ''.join(helper_lines)
                self.fail(
                    f'invalid-CBW probe failed:\n{probe_stdout}{probe_stderr}'
                    f'Raw BOT target output:\n{helper_output}')
            self.assertIn('ordered Reset Recovery', probe_stdout)
            self.assertIn('all 13 BOT cases', probe_stdout)
            self.assertIn('5 valid-but-meaningless CBWs', probe_stdout)
            self.assertIn(
                'fuzz-seed=0x52504934 cases=128 '
                'fnv64=2e5f4d8d3041b5d7', probe_stdout)
            self.assertIn(
                '128 deterministic randomized CBW/SCSI cases', probe_stdout)
            self.assertEqual(
                hashlib.sha256(backend.read_bytes()).digest(),
                backend_digest,
                'non-mutating BOT corpus changed the eMMC backend')

            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and not stable.exists():
                time.sleep(0.05)
            self.assertTrue(stable.exists(),
                            'usb-storage did not reattach after BOT probe')
            self._assert_raw_msd_official_composite(stable)
            block = stable.resolve(strict=True)
            sg_dir = (Path('/sys/class/block') / block.name /
                      'device/scsi_generic')
            deadline = time.monotonic() + 10
            sg_devices = []
            while time.monotonic() < deadline:
                sg_devices = list(sg_dir.iterdir()) if sg_dir.is_dir() else []
                if len(sg_devices) == 1:
                    break
                time.sleep(0.05)
            self.assertEqual(len(sg_devices), 1,
                             'invalid-CBW target has no SCSI generic node')
            inquiry = subprocess.run(
                ['sudo', '-n', sg_inq, f'/dev/{sg_devices[0].name}'],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(inquiry.returncode, 0,
                             'kernel INQUIRY failed after invalid-CBW probe')
            readback = subprocess.run(
                ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                 'bs=4096', 'count=1', 'status=none'],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(readback.returncode, 0,
                             'kernel read failed after invalid-CBW probe')
            probe_completed = True
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            helper.wait(timeout=10)
            helper_drain.join(timeout=10)
            self.assertFalse(helper_drain.is_alive(),
                             'Raw BOT output-drain thread did not stop')
            helper.stdout.close()
            helper_output = ''.join(helper_lines)
            if probe_completed:
                self.assertEqual(helper_output.count('invalid 31-byte CBW'), 1)
                self.assertEqual(helper_output.count('invalid 30-byte CBW'), 1)
                self.assertEqual(
                    helper_output.count(
                        'invalid CBW accepted Mass Storage Reset'), 2)
                self.assertGreaterEqual(
                    helper_output.count('received BOT Mass Storage Reset'), 2)
                for case in (2, 3, 4, 5, 7, 8, 9, 10, 11, 13):
                    self.assertIn(
                        f'BOT thirteen-case {case} opcode=', helper_output)
                for case in (2, 3, 7, 8, 10, 13):
                    self.assertIn(
                        f'recovered BOT thirteen-case {case}', helper_output)
                self.assertEqual(
                    helper_output.count('valid but not meaningful CBW'), 5)
                self.assertGreaterEqual(
                    helper_output.count('SCSI opcode 0x'), 128)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and stable.exists():
                time.sleep(0.05)
            self.assertFalse(stable.exists(),
                             'invalid-CBW USB disk remained after helper exit')

    def _assert_raw_msd_official_composite(self, stable):
        """Prove the pinned boot.img identity and both composite functions."""
        import termios

        lsusb = shutil.which('lsusb')
        if not lsusb:
            self.fail('lsusb is required for the composite identity gate')
        descriptor = subprocess.run(
            [lsusb, '-v', '-d', '0a5c:0104'],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(descriptor.returncode, 0, descriptor.stderr)
        for expected in (
                'iManufacturer           1 Raspberry Pi',
                'iProduct                2 Raspberry Pi multi-function USB '
                'device',
                'iSerial                 3 51554d5552504934',
                'bNumInterfaces          3',
                'bmAttributes         0x80',
                'MaxPower              500mA',
                'bInterfaceClass         8 Mass Storage',
                'bInterfaceClass         2 Communications',
                'bInterfaceClass        10 CDC Data',
                'bEndpointAddress     0x81  EP 1 IN',
                'bEndpointAddress     0x01  EP 1 OUT',
                'bEndpointAddress     0x82  EP 2 IN',
                'bEndpointAddress     0x02  EP 2 OUT',
                'bEndpointAddress     0x83  EP 3 IN'):
            self.assertIn(expected, descriptor.stdout)

        self.assertIn(
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_',
            stable.name)
        tty = None
        for candidate in Path('/sys/class/tty').glob('ttyACM*'):
            resolved = candidate.resolve()
            for parent in (resolved, *resolved.parents):
                try:
                    identity = (
                        (parent / 'idVendor').read_text(
                            encoding='ascii').strip(),
                        (parent / 'idProduct').read_text(
                            encoding='ascii').strip(),
                        (parent / 'serial').read_text(encoding='ascii').strip(),
                    )
                except OSError:
                    continue
                if identity == ('0a5c', '0104', '51554d5552504934'):
                    tty = Path('/dev') / candidate.name
                    break
            if tty is not None:
                break
        self.assertIsNotNone(tty, 'official composite has no CDC ACM tty')

        fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            attributes = termios.tcgetattr(fd)
            attributes[0] = 0
            attributes[1] = 0
            attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attributes[3] = 0
            attributes[4] = termios.B115200
            attributes[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attributes)
            termios.tcflush(fd, termios.TCIOFLUSH)
            payload = b'acm-composite-proof\n'
            os.write(fd, payload)
            observed = bytearray()
            deadline = time.monotonic() + 5
            while len(observed) < len(payload) and time.monotonic() < deadline:
                ready, _, _ = select.select([fd], [], [], 0.25)
                if ready:
                    observed.extend(os.read(fd, len(payload) - len(observed)))
            self.assertEqual(bytes(observed), payload)
        finally:
            os.close(fd)

    def _exercise_raw_msd_imager_flush_fault(self, raw_msd, imager,
                                             lifecycle):
        """Require unchanged Imager to reject a failed cache flush CDB."""
        source = Path(self.workdir) / 'cm4-raw-msd-imager-source.img'
        backend = Path(self.workdir) / 'cm4-raw-msd-imager-target.img'
        with source.open('wb') as image:
            image.truncate(32 * 1024 * 1024)
        with backend.open('wb') as image:
            image.truncate(64 * 1024 * 1024)
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        stable = Path(
            '/dev/disk/by-id/'
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_'
            '51554d5552504934-0:0')
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_msd), '--image', str(backend),
             '--lifecycle', str(lifecycle), '--fault', 'synchronize-cache',
             '--fault-after', '1'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        ready_output = ''
        imager_failed = False
        try:
            while 'CM4 raw BOT target ready' not in ready_output:
                line = helper.stdout.readline()
                if not line:
                    output = helper.communicate(timeout=5)[0]
                    self.fail(
                        'Raw BOT helper exited before Imager probe:\n'
                        f'{ready_output}{output}')
                ready_output += line
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not stable.exists():
                time.sleep(0.05)
            self.assertTrue(stable.exists(),
                            'raw BOT Imager target did not enumerate')
            imager_run = subprocess.run(
                ['sudo', '-n', str(imager), '--disable-eject',
                 '--sha256', digest, str(source),
                 str(stable.resolve(strict=True))],
                capture_output=True, text=True, timeout=30)
            combined = imager_run.stdout + imager_run.stderr
            self.assertNotEqual(
                imager_run.returncode, 0,
                'Imager unexpectedly accepted failed SYNCHRONIZE CACHE')
            self.assertIn('Error flushing data to storage device', combined)
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'flash-failed')
            self.assertTrue(stable.exists(),
                            'failed Imager flush disconnected raw BOT USB')
            imager_failed = True
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            remaining_output = helper.communicate(timeout=10)[0]
            if imager_failed:
                self.assertIn('(injected)', ready_output + remaining_output)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and stable.exists():
                time.sleep(0.05)
            self.assertFalse(stable.exists(),
                             'raw BOT Imager target remained after exit')

    def _assert_cm4_production_boot(self, vm, command_line):
        def machine_property(name):
            return vm.cmd('qom-get', path='/machine', property=name)

        self.assertEqual(machine_property('boot-state'),
                         'arm-handoff-ready')
        self.assertEqual(machine_property('boot-source'), 'emmc')
        self.assertEqual(machine_property('recovery-status'), 'none')
        self.assertEqual(machine_property('otp-board-revision'), 0xb03140)
        self.assertEqual(machine_property('firmware-device-tree-file'),
                         'bcm2711-rpi-cm4.dtb')
        self.assertEqual(machine_property('firmware-sha256'),
                         self.ASSET_START4.hash)
        self.assertEqual(machine_property('firmware-kernel-sha256'),
                         self.KERNEL8_SHA256)
        self.assertEqual(machine_property('firmware-device-tree-sha256'),
                         self.BCM2711_CM4_DTB_SHA256)
        self.assertTrue(
            machine_property('boot-eeprom-build-timestamp-valid'))
        self.assertEqual(
            machine_property('boot-eeprom-build-timestamp'),
            self.PIEEPROM_BUILD_TIMESTAMP)
        self.assertEqual(machine_property('boot-eeprom-version'),
                         self.PIEEPROM_VERSION)
        self.assertTrue(
            machine_property('boot-eeprom-capabilities-valid'))
        self.assertEqual(
            machine_property('boot-eeprom-capabilities'), 0x7f)
        self.assertEqual(machine_property('bootloader-signed'), 0)
        self.wait_for_console_pattern(command_line, vm=vm)
        vm.cmd('qom-set', path='/machine', property='boot-health',
               value='kernel-started')
        self.wait_for_console_pattern('Boot successful.', vm=vm)
        vm.cmd('qom-set', path='/machine', property='boot-health',
               value='userspace-ready')
        self.assertEqual(machine_property('boot-health'), 'userspace-ready')
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/thermal/thermal_zone0/temp', '24823', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/tty/ttyS0/dev', '4:64', vm=vm)
        exec_command_and_wait_for_pattern(
            self,
            ('test -f /proc/device-tree/chosen/bootloader/pm_rsts && '
             'test ! -e /proc/device-tree/chosen/bootloader/rsts && '
             "printf 'PM_RSTS_OK\\n'"),
            'PM_RSTS_OK', vm=vm)

    def test_arm_raspi4(self):
        kernel_path = self.archive_extract(self.ASSET_KERNEL_20190215,
                                           member='boot/kernel8.img')
        dtb_path = self.archive_extract(self.ASSET_KERNEL_20190215,
                                        member='boot/bcm2711-rpi-4-b.dtb')
        self.set_machine('raspi4b')
        self.vm.set_console()
        kernel_command_line = (self.KERNEL_COMMON_COMMAND_LINE +
                               'earlycon=pl011,mmio32,0xfe201000 ' +
                               'console=ttyAMA0,115200 ' +
                               'root=/dev/mmcblk1p2 rootwait ' +
                               'dwc_otg.fiq_fsm_enable=0')
        self.vm.add_args('-kernel', kernel_path,
                         '-dtb', dtb_path,
                         '-append', kernel_command_line)
        self.vm.launch()
        console_pattern = 'Kernel command line: %s' % kernel_command_line
        self.wait_for_console_pattern(console_pattern)
        console_pattern = 'Waiting for root device'
        self.wait_for_console_pattern(console_pattern)


    def test_arm_raspi4_initrd(self):
        kernel_path = self.archive_extract(self.ASSET_KERNEL_20190215,
                                           member='boot/kernel8.img')
        dtb_path = self.archive_extract(self.ASSET_KERNEL_20190215,
                                        member='boot/bcm2711-rpi-4-b.dtb')
        initrd_path = self.uncompress(self.ASSET_INITRD)

        self.set_machine('raspi4b')
        self.vm.set_console()
        kernel_command_line = (self.KERNEL_COMMON_COMMAND_LINE +
                               'earlycon=pl011,mmio32,0xfe201000 ' +
                               'console=ttyAMA0,115200 ' +
                               'panic=-1 noreboot ' +
                               'dwc_otg.fiq_fsm_enable=0')
        self.vm.add_args('-kernel', kernel_path,
                         '-dtb', dtb_path,
                         '-initrd', initrd_path,
                         '-append', kernel_command_line,
                         '-no-reboot')
        # When PCI is supported we can add a USB controller:
        #                '-device', 'qemu-xhci,bus=pcie.1,id=xhci',
        #                '-device', 'usb-kbd,bus=xhci.0',
        self.vm.launch()
        self.wait_for_console_pattern('Boot successful.')

        exec_command_and_wait_for_pattern(self, 'cat /proc/cpuinfo',
                                                'BCM2835')
        exec_command_and_wait_for_pattern(self, 'cat /proc/iomem',
                                                'cprman@7e101000')
        exec_command_and_wait_for_pattern(self, 'halt', 'reboot: System halted')
        # TODO: Raspberry Pi4 doesn't shut down properly with recent kernels
        # Wait for VM to shut down gracefully
        #self.vm.wait()

    def _prepare_pi4_production_boot_media(self, image_name,
                                           mini_uart=False,
                                           extra_files=None,
                                           config_override=None,
                                           include_default_overlays=True,
                                           dtb_override=None,
                                           initramfs_override=None):
        """Build one FAT image around byte-unchanged pinned Pi artifacts."""
        kernel_path = self.archive_extract(self.ASSET_KERNEL_20190215,
                                           member='boot/kernel8.img')
        dtb_path = (dtb_override or
                    self.archive_extract(
                        self.ASSET_KERNEL_20190215,
                        member='boot/bcm2711-rpi-4-b.dtb'))
        start4_path = self.ASSET_START4.fetch()
        fixup4_path = self.ASSET_FIXUP4.fetch()
        eeprom_path = self.ASSET_PIEEPROM.fetch()
        initramfs_path = initramfs_override or self.ASSET_INITRD.fetch()
        image_path = os.path.join(self.workdir, image_name)
        writable_eeprom_path = os.path.join(
            self.workdir, f'{image_name}.pieeprom.bin')
        if mini_uart:
            media_command_line = (
                self.KERNEL_COMMON_COMMAND_LINE +
                'earlycon=bcm2835aux,mmio32,0xfe215040,115200 ' +
                'console=serial0,115200 panic=-1 noreboot ' +
                'rdinit=/sbin/init dwc_otg.fiq_fsm_enable=0')
            command_line = media_command_line.replace(
                'console=serial0,', 'console=ttyS0,')
        else:
            media_command_line = (
                self.KERNEL_COMMON_COMMAND_LINE +
                'earlycon=pl011,mmio32,0xfe201000 ' +
                'console=ttyAMA0,115200 panic=-1 noreboot ' +
                'rdinit=/sbin/init dwc_otg.fiq_fsm_enable=0')
            command_line = media_command_line

        files = [
            ('start4.elf', 'START4  ELF', Path(start4_path).read_bytes()),
            ('fixup4.dat', 'FIXUP4  DAT', Path(fixup4_path).read_bytes()),
            ('kernel8.img', 'KERNEL8 IMG', Path(kernel_path).read_bytes()),
            ('bcm2711-rpi-4-b.dtb', 'BCM271~1DTB',
             Path(dtb_path).read_bytes()),
            ('initramfs8', 'INITRA~1   ', Path(initramfs_path).read_bytes()),
            ('config.txt', 'CONFIG  TXT',
             config_override if config_override is not None else
             (b'arm_64bit=1\n'
              b'auto_initramfs=1\n'
              b'dtparam=i2c_arm=on\n'
              b'dtoverlay=i2c-gpio,i2c_gpio_sda=17,'
              b'i2c_gpio_scl=27,bus=9\n'
              b'dtoverlay=pwm-2chan\n')),
            ('cmdline.txt', 'CMDLINE TXT',
             media_command_line.encode() + b'\n'),
        ]
        if include_default_overlays:
            overlay_path = self.archive_extract(
                self.ASSET_KERNEL_20190215,
                member='boot/overlays/i2c-gpio.dtbo')
            pwm_overlay_path = self.archive_extract(
                self.ASSET_KERNEL_20190215,
                member='boot/overlays/pwm-2chan.dtbo')
            files[5:5] = [
                ('overlays/i2c-gpio.dtbo', 'I2CGPI~1DTB',
                 Path(overlay_path).read_bytes()),
                ('overlays/pwm-2chan.dtbo', 'PWM2CH~1DTB',
                 Path(pwm_overlay_path).read_bytes()),
            ]
        if extra_files:
            files.extend(extra_files)
        create_fat16_image(image_path, files)
        Path(writable_eeprom_path).write_bytes(Path(eeprom_path).read_bytes())

        return image_path, writable_eeprom_path, command_line

    def _prepare_aux_spi_initramfs(self):
        """Add byte-unchanged production modules to the test rootfs."""
        root = Path(self.workdir) / 'aux-spi-initramfs-root'
        output = Path(self.workdir) / 'aux-spi-initramfs.cpio.gz'
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir()

        package = self.ASSET_KERNEL_20190215
        release = '5.15.84-v8+'
        module_root = root / 'lib/modules' / release
        module_root.mkdir(parents=True)
        for name in (
                'modules.alias', 'modules.alias.bin', 'modules.builtin',
                'modules.builtin.alias.bin', 'modules.builtin.bin',
                'modules.builtin.modinfo', 'modules.dep', 'modules.dep.bin',
                'modules.devname', 'modules.order', 'modules.softdep',
                'modules.symbols', 'modules.symbols.bin'):
            source = self.archive_extract(
                package, member=f'lib/modules/{release}/{name}')
            shutil.copyfile(source, module_root / name)
        for name in ('spi-bcm2835aux.ko.xz', 'spidev.ko.xz'):
            relative = f'kernel/drivers/spi/{name}'
            destination = module_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            source = self.archive_extract(
                package, member=f'lib/modules/{release}/{relative}')
            shutil.copyfile(source, destination)

        find = subprocess.Popen(
            ['find', '.', '-print0'], cwd=root, stdout=subprocess.PIPE)
        cpio = subprocess.run(
            ['cpio', '--null', '-o', '--quiet', '--format=newc'],
            cwd=root, stdin=find.stdout, stdout=subprocess.PIPE, check=True)
        find.stdout.close()
        self.assertEqual(find.wait(), 0)
        base = subprocess.run(
            ['gzip', '-dc', self.ASSET_INITRD.fetch()],
            stdout=subprocess.PIPE, check=True)
        with open(output, 'wb') as destination:
            gzip = subprocess.run(
                ['gzip', '-n', '-9'], input=base.stdout + cpio.stdout,
                stdout=destination,
                check=True)
        self.assertEqual(gzip.returncode, 0)
        return output

    def _assert_pi4_production_boot(self, vm, command_line, source,
                                    network_backend=False,
                                    pcie_endpoint='vl805'):
        def machine_property(name):
            return vm.cmd('qom-get', path='/machine', property=name)

        self.assertEqual(machine_property('boot-state'),
                         'arm-handoff-ready')
        self.assertEqual(machine_property('boot-source'), source)
        self.assertEqual(machine_property('arm-handoff-status'), 'ready')
        self.assertEqual(machine_property('arm-handoff-core-mask'), 0xf)
        self.assertEqual(machine_property('firmware-sha256'),
                         self.ASSET_START4.hash)
        self.assertEqual(machine_property('firmware-kernel-sha256'),
                         self.KERNEL8_SHA256)
        self.assertEqual(machine_property('firmware-device-tree-sha256'),
                         self.BCM2711_DTB_SHA256)
        self.assertTrue(
            machine_property('boot-eeprom-build-timestamp-valid'))
        self.assertEqual(
            machine_property('boot-eeprom-build-timestamp'),
            self.PIEEPROM_BUILD_TIMESTAMP)
        self.assertEqual(machine_property('boot-eeprom-version'),
                         self.PIEEPROM_VERSION)
        self.assertTrue(
            machine_property('boot-eeprom-capabilities-valid'))
        self.assertEqual(
            machine_property('boot-eeprom-capabilities'), 0x7f)
        self.assertEqual(machine_property('bootloader-signed'), 0)
        self.assertEqual(machine_property('firmware-overlay-applied'), 2)
        self.assertEqual(machine_property('firmware-dtparam-applied'), 4)
        self.assertEqual(machine_property('firmware-overlay-file'),
                         'overlays/pwm-2chan.dtbo')
        self.assertEqual(machine_property('wireless-status'),
                         'excluded-unmodeled')
        self.assertEqual(
            machine_property('peripheral-model-policy'),
            'preserve-disabled-unmodeled-v1')
        self.assertEqual(
            machine_property('peripheral-exclusions'),
            ('brcm,brcm2711-dvp;brcm,bcm2835-mmc;'
             'brcm,bcm43438-bt'))
        self.assertEqual(machine_property('videocore-execution-mode'),
                         'behavioral-replacement-v1')
        self.assertEqual(
            machine_property('videocore-artifact-policy'),
            'exact-input-bytes-not-instruction-executed')
        self.assertEqual(machine_property('videocore-boundary-version'), 1)
        self.assertTrue(
            machine_property('boot-reboot-on-fatal-error'))
        self.wait_for_console_pattern(command_line, vm=vm)
        vm.cmd('qom-set', path='/machine', property='boot-health',
               value='kernel-started')
        self.wait_for_console_pattern(
            'bcmgenet fd580000.ethernet: GENET 5.0', vm=vm)
        self.wait_for_console_pattern('Boot successful.', vm=vm)
        vm.cmd('qom-set', path='/machine', property='boot-health',
               value='userspace-ready')
        self.assertEqual(machine_property('boot-health'), 'userspace-ready')

        exec_command_and_wait_for_pattern(self, 'cat /proc/cpuinfo',
                                          'BCM2835', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /proc/device-tree/i2c@9/compatible', 'i2c-gpio', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/thermal/thermal_zone0/temp', '24823', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/tty/ttyS0/dev', '4:64', vm=vm)
        exec_command_and_wait_for_pattern(
            self,
            ('test -f /proc/device-tree/chosen/bootloader/pm_rsts && '
             'test ! -e /proc/device-tree/chosen/bootloader/rsts && '
             "printf 'PM_RSTS_OK\\n'"),
            'PM_RSTS_OK', vm=vm)
        exec_command_and_wait_for_pattern(
            self,
            "tr -d '\\000' </proc/device-tree/soc/mmc@7e300000/status",
            'disabled', vm=vm)
        exec_command_and_wait_for_pattern(
            self,
            ("tr -d '\\000' </proc/device-tree/soc/"
             "serial@7e201000/bluetooth/status"),
            'disabled', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/bus/pci/devices/0000:00:00.0/vendor',
            '0x14e4', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/bus/pci/devices/0000:00:00.0/device',
            '0x2711', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/bus/pci/devices/0000:00:00.0/revision',
            '0x20', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/bus/pci/devices/0000:00:00.0/class',
            '0x060400', vm=vm)
        if pcie_endpoint == 'vl805':
            vendor, device, driver = '0x1106', '0x3483', 'xhci_hcd'
        else:
            self.assertEqual(pcie_endpoint, 'nvme')
            vendor, device, driver = '0x1b36', '0x0010', 'nvme'
        exec_command_and_wait_for_pattern(
            self,
            ("test -n \"$(ls /sys/bus/pci/devices/"
             "0000:01:00.0/msi_irqs)\" && printf '\\115\\123\\111\\n'"),
            'MSI', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/bus/pci/devices/0000:01:00.0/vendor',
            vendor, vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/bus/pci/devices/0000:01:00.0/device',
            device, vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'readlink /sys/bus/pci/devices/0000:01:00.0/driver',
            driver, vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/net/eth0/operstate',
            'up' if network_backend else 'down', vm=vm)
        if network_backend:
            exec_command_and_wait_for_pattern(
                self, 'ip -4 addr show dev eth0', '10.0.2.', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /proc/device-tree/soc/pwm@7e20c000/status',
            'okay', vm=vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /proc/device-tree/soc/pwm@7e20c000/compatible',
            'brcm,bcm2835-pwm', vm=vm)

    def test_arm_raspi4_production_firmware_boot(self):
        """Boot via unchanged production EEPROM and SD files."""
        image_path, writable_eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-boot.img')

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={writable_eeprom_path}'),
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-nic', 'user,model=bcm2711-genet',
            '-no-reboot')
        self.vm.launch()

        self._assert_pi4_production_boot(
            self.vm, command_line, 'sd-card', network_backend=True)

    def test_arm_raspi4_production_eeprom_config_append(self):
        """Apply EEPROM config with the official tool, then boot exact files."""
        image_path, eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-eeprom-config-append.img')
        tool = self.ASSET_RPI_EEPROM_CONFIG.fetch()
        current_config = Path(self.workdir) / 'current-boot.conf'
        updated_config = Path(self.workdir) / 'appended-boot.conf'
        configured_eeprom = Path(self.workdir) / \
            'raspi4-production-configured-pieeprom.bin'
        appendix = b'sdram_freq=3000\n'

        subprocess.run(
            [sys.executable, tool, eeprom_path,
             '--out', str(current_config)],
            check=True)
        config = current_config.read_bytes()
        if not config.endswith(b'\n'):
            config += b'\n'
        self.assertNotIn(b'[config.txt]', config)
        updated_config.write_bytes(
            config + b'[all]\nBOOT_UART=1\n'
            b'[config.txt]\n' + appendix)
        subprocess.run(
            [sys.executable, tool, eeprom_path,
             '--config', str(updated_config),
             '--out', str(configured_eeprom)],
            check=True)
        self.assertEqual(configured_eeprom.stat().st_size, 512 * 1024)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={configured_eeprom}'),
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-nic', 'user,model=bcm2711-genet',
            '-no-reboot')
        self.vm.launch()

        self._assert_pi4_production_boot(
            self.vm, command_line, 'sd-card', network_backend=True)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-eeprom-config-append-size'),
            len(appendix))
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-eeprom-config-append-sha256'),
            hashlib.sha256(appendix).hexdigest())
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='firmware-sdram-frequency-requested-mhz'),
            3000)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='firmware-sdram-frequency-mhz'),
            3200)
        self.assertTrue(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-uart-enabled'))
        self.assertFalse(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-uart-active'))
        self.assertGreater(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-uart-bytes'),
            0)
        self.assertGreaterEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-uart-lines'),
            3)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-uart-format'),
            'primary-uart0-115200-8n1-cleanroom-v1')

    def test_arm_cm4_production_vl805_usb_boot(self):
        """Boot exact production files through EEPROM-enabled external VL805."""
        cm4_dtb = self.ASSET_BCM2711_CM4_DTB_CURRENT.fetch()
        media_config = (
            b'arm_64bit=1\n'
            b'auto_initramfs=1\n'
            b'device_tree=bcm2711-rpi-cm4.dtb\n')
        image_path, eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'cm4-production-vl805-usb.img',
                config_override=media_config,
                include_default_overlays=False,
                extra_files=[
                    ('bcm2711-rpi-cm4.dtb', 'BCM271~2DTB',
                     Path(cm4_dtb).read_bytes()),
                ])
        tool = self.ASSET_RPI_EEPROM_CONFIG.fetch()
        current_config = Path(self.workdir) / 'cm4-vl805-current.conf'
        updated_config = Path(self.workdir) / 'cm4-vl805-enabled.conf'
        configured_eeprom = Path(self.workdir) / \
            'cm4-vl805-configured-pieeprom.bin'

        subprocess.run(
            [sys.executable, tool, eeprom_path,
             '--out', str(current_config)],
            check=True)
        config = current_config.read_bytes()
        if not config.endswith(b'\n'):
            config += b'\n'
        updated_config.write_bytes(
            config + b'[all]\nBOOT_ORDER=0x4\nVL805=1\n')
        subprocess.run(
            [sys.executable, tool, eeprom_path,
             '--config', str(updated_config),
             '--out', str(configured_eeprom)],
            check=True)
        self.assertEqual(configured_eeprom.stat().st_size, 512 * 1024)

        self.set_machine('raspi-cm4')
        self.vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,'
            'usb-boot-drive=usbboot,usb-boot-controller=xhci')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={configured_eeprom}'),
            '-drive', f'if=none,id=usbboot,format=raw,file={image_path}',
            '-nic', 'user,model=bcm2711-genet',
            '-no-reboot')
        self.vm.launch()

        def machine_property(name):
            return self.vm.cmd(
                'qom-get', path='/machine', property=name)

        self.assertEqual(machine_property('boot-state'),
                         'arm-handoff-ready')
        self.assertEqual(machine_property('boot-source'), 'usb-msd')
        self.assertTrue(machine_property('boot-vl805-enabled'))
        self.assertTrue(machine_property('boot-vl805-initialized'))
        self.assertEqual(machine_property('boot-vl805-status'),
                         'initialized')
        self.assertEqual(
            machine_property('usb-boot-transport'),
            'vl805-xhci-host-bot-scsi-read10-v1')
        self.assertTrue(machine_property('usb-boot-identity-valid'))
        self.assertEqual(machine_property('usb-boot-version'), 3)
        self.assertEqual(machine_property('usb-boot-route-string'), 0)
        self.assertEqual(machine_property('usb-boot-root-hub-port'), 1)
        self.assertEqual(machine_property('usb-boot-selected-lun'), 0)
        self.assertEqual(machine_property('firmware-sha256'),
                         self.ASSET_START4.hash)
        self.assertEqual(
            machine_property('firmware-device-tree-sha256'),
            self.ASSET_BCM2711_CM4_DTB_CURRENT.hash)
        self.wait_for_console_pattern(command_line, vm=self.vm)
        self.wait_for_console_pattern('Boot successful.', vm=self.vm)

    def test_arm_raspi4_production_aux_spi_drivers(self):
        """Bind and transfer through both stock Linux AUX SPI drivers."""
        spi1_overlay = self.ASSET_OVERLAY_SPI1_1CS.fetch()
        spi2_overlay = self.ASSET_OVERLAY_SPI2_1CS.fetch()
        initramfs = self._prepare_aux_spi_initramfs()
        config = (b'arm_64bit=1\n'
                  b'auto_initramfs=1\n'
                  b'dtoverlay=spi1-1cs\n'
                  b'dtoverlay=spi2-1cs\n')
        image_path, writable_eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-aux-spi.img',
                extra_files=[
                    ('overlays/spi1-1cs.dtbo', 'SPI11C~1DTB',
                     Path(spi1_overlay).read_bytes()),
                    ('overlays/spi2-1cs.dtbo', 'SPI21C~1DTB',
                     Path(spi2_overlay).read_bytes()),
                ],
                config_override=config,
                include_default_overlays=False,
                initramfs_override=initramfs)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={writable_eeprom_path}'),
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-device', 'w25q80bl,bus=aux-spi1,cs=0',
            '-device', 'w25q80bl,bus=aux-spi2,cs=0',
            '-nic', 'none',
            '-no-reboot')
        self.vm.launch()

        self.wait_for_console_pattern(command_line)
        self.wait_for_console_pattern('Boot successful.')
        exec_command_and_wait_for_pattern(
            self, 'modprobe spi-bcm2835aux && echo AUX_DRIVER_OK',
            'AUX_DRIVER_OK')
        exec_command_and_wait_for_pattern(
            self, 'modprobe spidev && echo SPIDEV_DRIVER_OK',
            'SPIDEV_DRIVER_OK')
        exec_command_and_wait_for_pattern(
            self,
            ('test -e /sys/bus/platform/drivers/spi-bcm2835aux/'
             'fe215080.spi && '
             'test -e /sys/bus/platform/drivers/spi-bcm2835aux/'
             'fe2150c0.spi && echo AUX_BIND_OK'),
            'AUX_BIND_OK')
        exec_command_and_wait_for_pattern(
            self,
            ('test -c /dev/spidev1.0 && test -c /dev/spidev2.0 && '
             'echo SPIDEV_NODES_OK'),
            'SPIDEV_NODES_OK')
        exec_command_and_wait_for_pattern(
            self,
            ("printf '\\237\\000\\000\\000' > /dev/spidev1.0 && "
             "printf '\\237\\000\\000\\000' > /dev/spidev2.0 && "
             'echo AUX_TRANSFER_OK'),
            'AUX_TRANSFER_OK')

    def test_arm_raspi4_production_overlay_corpus(self):
        """Apply a hash-pinned current production overlay corpus."""
        overlay_assets = [
            ('gpio-led.dtbo', 'GPIOLE~1DTB', self.ASSET_OVERLAY_GPIO_LED,
             b'gpio=26,active_low=1,label=corpus'),
            ('i2c-gpio.dtbo', 'I2CGPI~1DTB', self.ASSET_OVERLAY_I2C_GPIO,
             b'i2c_gpio_sda=17,i2c_gpio_scl=27,bus=9'),
            ('i2c-rtc.dtbo', 'I2CRTC~1DTB', self.ASSET_OVERLAY_I2C_RTC,
             b'ds3231,wakeup-source'),
            ('mcp2515-can0.dtbo', 'MCP251~1DTB',
             self.ASSET_OVERLAY_MCP2515_CAN0,
             b'oscillator=16000000,interrupt=25,spimaxfrequency=10000000'),
            ('miniuart-bt.dtbo', 'MINIUA~1DTB',
             self.ASSET_OVERLAY_MINIUART_BT, b''),
            ('pwm-2chan.dtbo', 'PWM2CH~1DTB',
             self.ASSET_OVERLAY_PWM_2CHAN,
             b'pin=18,func=2,pin2=19,func2=2'),
            ('spi0-1cs.dtbo', 'SPI01C~1DTB',
             self.ASSET_OVERLAY_SPI0_1CS, b'cs0_pin=8'),
            ('uart2.dtbo', 'UART2   DTB', self.ASSET_OVERLAY_UART2, b''),
            ('w1-gpio.dtbo', 'W1GPIO~1DTB', self.ASSET_OVERLAY_W1_GPIO,
             b'gpiopin=4,pullup=on'),
        ]
        extra_files = [
            (f'overlays/{name}', short_name,
             Path(asset.fetch()).read_bytes())
            for name, short_name, asset, _ in overlay_assets
        ]
        base_dtb = self.ASSET_BCM2711_DTB.fetch()
        for name, _, _, parameters in overlay_assets:
            with self.subTest(overlay=name):
                config = (b'arm_64bit=1\nauto_initramfs=1\n'
                          b'dtoverlay=' + name[:-5].encode())
                if parameters:
                    config += b',' + parameters
                config += b'\n'
                image_path, writable_eeprom_path, _ = \
                    self._prepare_pi4_production_boot_media(
                        f'raspi4-production-{name}.img',
                        extra_files=extra_files,
                        config_override=config,
                        include_default_overlays=False,
                        dtb_override=base_dtb)

                vm = self.get_vm(name=f'overlay-{name[:-5]}')
                vm.set_machine(
                    'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
                vm.add_args(
                    '-drive',
                    ('if=none,id=pieeprom,format=raw,snapshot=on,'
                     f'file={writable_eeprom_path}'),
                    '-drive', f'if=sd,format=raw,file={image_path}',
                    '-seed', '0x52504934',
                    '-nic', 'none',
                    '-no-reboot')
                vm.launch()

                def machine_property(property_name):
                    return vm.cmd(
                        'qom-get', path='/machine',
                        property=property_name)

                self.assertEqual(machine_property('arm-handoff-status'),
                                 'ready')
                self.assertEqual(machine_property('boot-state'),
                                 'arm-handoff-ready')
                self.assertEqual(
                    machine_property('firmware-device-tree-sha256'),
                    self.ASSET_BCM2711_DTB.hash)
                self.assertEqual(
                    machine_property('firmware-overlay-applied'), 1)
                self.assertEqual(
                    machine_property('firmware-overlay-file'),
                    f'overlays/{name}')
                # The tree that comes back out is serialised by libfdt, and
                # its exact bytes are not stable across libfdt releases:
                # 1.6, 1.7 and 1.8 disagree on overlay application for some
                # of these inputs.  What this test owns is the input tree,
                # asserted exactly above, and which overlay was applied to
                # it; the digest of the result is only checked for shape.
                self.assertRegex(
                    machine_property('firmware-final-device-tree-sha256'),
                    '^[0-9a-f]{64}$')

                # Each overlay is verified independently, so release the
                # machine before preparing the next one.  Leaving the whole
                # corpus running needs one host process per overlay and
                # exhausts locked memory on hosts with a small
                # RLIMIT_MEMLOCK when QEMU is built with io_uring.
                vm.shutdown()

        combined_config = b'arm_64bit=1\nauto_initramfs=1\n'
        for name, _, _, parameters in overlay_assets:
            combined_config += b'dtoverlay=' + name[:-5].encode()
            if parameters:
                combined_config += b',' + parameters
            combined_config += b'\n'
        image_path, writable_eeprom_path, _ = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-overlay-corpus-combined.img',
                extra_files=extra_files,
                config_override=combined_config,
                include_default_overlays=False,
                dtb_override=base_dtb)
        vm = self.get_vm(name='overlay-combined')
        vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        vm.add_args(
            '-drive',
            ('if=none,id=pieeprom,format=raw,snapshot=on,'
             f'file={writable_eeprom_path}'),
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-seed', '0x52504934',
            '-nic', 'none',
            '-no-reboot')
        vm.launch()
        self.assertEqual(
            vm.cmd('qom-get', path='/machine',
                   property='arm-handoff-status'),
            'ready')
        self.assertEqual(
            vm.cmd('qom-get', path='/machine',
                   property='firmware-overlay-applied'),
            len(overlay_assets))
        self.assertEqual(
            vm.cmd('qom-get', path='/machine',
                   property='firmware-overlay-file'),
            'overlays/w1-gpio.dtbo')
        # As above: the merged tree is libfdt's output, not ours.  The
        # count and the last overlay applied are what this case is for.
        self.assertRegex(
            vm.cmd('qom-get', path='/machine',
                   property='firmware-final-device-tree-sha256'),
            '^[0-9a-f]{64}$')

    def test_arm_raspi4_production_arm32_firmware_boot(self):
        """Boot unchanged production kernel7l.img on all four CPUs."""
        start4_path = self.ASSET_START4.fetch()
        fixup4_path = self.ASSET_FIXUP4.fetch()
        kernel_path = self.ASSET_KERNEL7L.fetch()
        dtb_path = self.ASSET_BCM2711_DTB_ARM32.fetch()
        initramfs_path = self.ASSET_INITRD_ARM32.fetch()
        eeprom_path = self.ASSET_PIEEPROM.fetch()
        image_path = Path(self.workdir) / 'raspi4-production-arm32.img'
        writable_eeprom_path = (
            Path(self.workdir) / 'raspi4-production-arm32.pieeprom.bin')
        command_line = (
            'earlycon=pl011,mmio32,0xfe201000 '
            'console=ttyAMA0,115200 panic=-1 noreboot rdinit=/sbin/init')

        create_fat16_image(image_path, [
            ('start4.elf', 'START4  ELF',
             Path(start4_path).read_bytes()),
            ('fixup4.dat', 'FIXUP4  DAT',
             Path(fixup4_path).read_bytes()),
            ('kernel7l.img', 'KERNEL7LIMG',
             Path(kernel_path).read_bytes()),
            ('bcm2711-rpi-4-b.dtb', 'BCM271~1DTB',
             Path(dtb_path).read_bytes()),
            ('initramfs7l', 'INITRA~1   ',
             Path(initramfs_path).read_bytes()),
            ('config.txt', 'CONFIG  TXT',
             b'arm_64bit=0\nauto_initramfs=1\n'),
            ('cmdline.txt', 'CMDLINE TXT',
             command_line.encode() + b'\n'),
        ])
        writable_eeprom_path.write_bytes(Path(eeprom_path).read_bytes())

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        self.vm.set_console()
        self.vm.add_args(
            '-m', '2G',
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={writable_eeprom_path}'),
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-nic', 'none',
            '-no-reboot')
        self.vm.launch()

        def machine_property(name):
            return self.vm.cmd(
                'qom-get', path='/machine', property=name)

        self.assertEqual(machine_property('boot-state'),
                         'arm-handoff-ready')
        self.assertEqual(machine_property('firmware-kernel-file'),
                         'kernel7l.img')
        self.assertFalse(machine_property('firmware-arm-64bit'))
        self.assertEqual(machine_property('arm-handoff-architecture'),
                         'aarch32')
        self.assertEqual(machine_property('arm-handoff-kernel-address'),
                         0x8000)
        self.assertEqual(machine_property('firmware-sha256'),
                         self.ASSET_START4.hash)
        self.assertEqual(machine_property('firmware-kernel-sha256'),
                         self.ASSET_KERNEL7L.hash)
        self.assertEqual(machine_property('firmware-device-tree-sha256'),
                         self.ASSET_BCM2711_DTB_ARM32.hash)
        self.assertEqual(machine_property('firmware-initramfs-sha256'),
                         self.ASSET_INITRD_ARM32.hash)
        self.wait_for_console_pattern(
            'Linux version 5.15.84-v7l+', vm=self.vm)
        self.wait_for_console_pattern(
            'CPU3: thread -1, cpu 3', vm=self.vm)
        self.wait_for_console_pattern(
            'smp: Brought up 1 node, 4 CPUs', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='kernel-started')
        self.wait_for_console_pattern('Boot successful.', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='userspace-ready')
        self.assertEqual(machine_property('boot-health'), 'userspace-ready')
        exec_command_and_wait_for_pattern(
            self, 'uname -m', 'armv7l', vm=self.vm)

    def test_arm_cm4_production_arm32_emmc_boot(self):
        """Boot unchanged production kernel7l.img through CM4 eMMC."""
        emmc_path = Path(self.workdir) / 'cm4-production-arm32.img'
        eeprom_path = Path(
            self.workdir) / 'cm4-production-arm32.pieeprom.bin'
        self._prepare_cm4_production_media(
            emmc_path, eeprom_path, arm32=True)

        self._configure_cm4_boot_vm(self.vm, emmc_path, eeprom_path)
        self.vm.launch()

        def machine_property(name):
            return self.vm.cmd(
                'qom-get', path='/machine', property=name)

        self.assertEqual(machine_property('boot-state'),
                         'arm-handoff-ready')
        self.assertEqual(machine_property('boot-source'), 'emmc')
        self.assertEqual(machine_property('otp-board-revision'), 0xb03140)
        self.assertEqual(machine_property('firmware-kernel-file'),
                         'kernel7l.img')
        self.assertEqual(machine_property('firmware-device-tree-file'),
                         'bcm2711-rpi-cm4.dtb')
        self.assertFalse(machine_property('firmware-arm-64bit'))
        self.assertEqual(machine_property('arm-handoff-architecture'),
                         'aarch32')
        self.assertEqual(machine_property('arm-handoff-kernel-address'),
                         0x8000)
        self.assertEqual(machine_property('firmware-sha256'),
                         self.ASSET_START4.hash)
        self.assertEqual(machine_property('firmware-kernel-sha256'),
                         self.ASSET_KERNEL7L.hash)
        self.assertEqual(machine_property('firmware-device-tree-sha256'),
                         self.ASSET_BCM2711_CM4_DTB_ARM32.hash)
        self.assertEqual(machine_property('firmware-initramfs-sha256'),
                         self.ASSET_INITRD_ARM32.hash)
        self.wait_for_console_pattern(
            'Linux version 5.15.84-v7l+', vm=self.vm)
        self.wait_for_console_pattern(
            'CPU3: thread -1, cpu 3', vm=self.vm)
        self.wait_for_console_pattern(
            'smp: Brought up 1 node, 4 CPUs', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='kernel-started')
        self.wait_for_console_pattern('Boot successful.', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='userspace-ready')
        self.assertEqual(machine_property('boot-health'), 'userspace-ready')
        exec_command_and_wait_for_pattern(
            self, 'uname -m', 'armv7l', vm=self.vm)

    def test_arm_raspi4_production_nvme_enumeration(self):
        """Boot production SD firmware and bind the modeled PCIe NVMe."""
        image_path, writable_eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-nvme.img')
        nvme_path = os.path.join(self.workdir, 'raspi4-nvme.img')
        shutil.copyfile(image_path, nvme_path)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,'
            'nvme-drive=nvme0')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={writable_eeprom_path}'),
            '-drive', f'if=none,id=nvme0,format=raw,file={nvme_path}',
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-nic', 'user,model=bcm2711-genet',
            '-no-reboot')
        self.vm.launch()

        self._assert_pi4_production_boot(
            self.vm, command_line, 'sd-card', network_backend=True,
            pcie_endpoint='nvme')
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/block/nvme0n1/size', '131072', vm=self.vm)
        with open(nvme_path, 'rb') as nvme_file:
            first_sector_sha256 = hashlib.sha256(
                nvme_file.read(512)).hexdigest()
        exec_command_and_wait_for_pattern(
            self,
            'dd if=/dev/nvme0n1 bs=512 count=1 2>/dev/null | sha256sum',
            first_sector_sha256, vm=self.vm)
        write_sector = (b'QEMU-RPI-NVME\n' * 37)[:512]
        write_sector_sha256 = hashlib.sha256(write_sector).hexdigest()
        exec_command_and_wait_for_pattern(
            self,
            ('yes QEMU-RPI-NVME | head -c 512 '
             '>/tmp/qemu-rpi-nvme-sector && '
             'dd if=/tmp/qemu-rpi-nvme-sector of=/dev/nvme0n1 '
             'bs=512 seek=131071 count=1 conv=fsync 2>/dev/null && '
             'rm /tmp/qemu-rpi-nvme-sector && echo NVME_WRITE_OK'),
            'NVME_WRITE_OK', vm=self.vm)
        exec_command_and_wait_for_pattern(
            self,
            ('dd if=/dev/nvme0n1 bs=512 skip=131071 count=1 '
             '2>/dev/null | sha256sum'),
            write_sector_sha256, vm=self.vm)
        with open(nvme_path, 'rb') as nvme_file:
            nvme_file.seek(131071 * 512)
            self.assertEqual(nvme_file.read(512), write_sector)
        reset_path = '/sys/bus/pci/devices/0000:01:00.0/reset'
        exec_command_and_wait_for_pattern(
            self, f'test -w {reset_path} && echo NVME_RESET_READY',
            'NVME_RESET_READY', vm=self.vm)
        exec_command_and_wait_for_pattern(
            self, f'echo 1 > {reset_path} && echo NVME_RESET_DONE',
            'NVME_RESET_DONE', vm=self.vm)
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/block/nvme0n1/size', '131072', vm=self.vm)

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_NETWORK_EEPROM',
                      'QEMU_RPI_NETWORK_TFTP_ROOT',
                      'QEMU_RPI_NETWORK_MANIFEST')
    def test_arm_raspi4_exact_network_boot(self):
        """Boot the manifested production corpus through real DHCP/TFTP."""
        eeprom = Path(
            os.environ['QEMU_RPI_NETWORK_EEPROM']).resolve(strict=True)
        tftp_root = Path(
            os.environ['QEMU_RPI_NETWORK_TFTP_ROOT']).resolve(strict=True)
        manifest = Path(
            os.environ['QEMU_RPI_NETWORK_MANIFEST']).resolve(strict=True)
        manifest_data = json.loads(manifest.read_text(encoding='utf-8'))
        files = manifest_data.get('tftp_files', {})
        required_hashes = {
            'start4.elf': self.ASSET_START4.hash,
            'fixup4.dat': self.ASSET_FIXUP4.hash,
            'kernel8.img': self.KERNEL8_SHA256,
            'bcm2711-rpi-4-b.dtb': self.BCM2711_DTB_SHA256,
            'initramfs8': self.ASSET_INITRD.hash,
        }
        self.assertEqual(manifest_data.get('schema'),
                         'qemu-rpi-network-boot-manifest-v2')
        for name, expected_hash in required_hashes.items():
            self.assertEqual(files.get(name), expected_hash,
                             f'manifest does not pin official {name}')

        repo_root = Path(__file__).resolve().parents[3]
        gate = repo_root / 'contrib/raspi4/network_boot_gate.py'
        report_path = Path(self.workdir) / 'network-boot-gate.json'
        pcap_path = Path(self.workdir) / 'network-boot.pcap'
        cadence_path = Path(self.workdir) / 'network-cadence.json'
        physical_pcap_name = os.environ.get(
            'QEMU_RPI_NETWORK_PHYSICAL_PCAP')
        physical_mac = os.environ.get('QEMU_RPI_NETWORK_PHYSICAL_MAC')
        self.assertEqual(
            physical_pcap_name is None, physical_mac is None,
            'set both QEMU_RPI_NETWORK_PHYSICAL_PCAP and '
            'QEMU_RPI_NETWORK_PHYSICAL_MAC, or neither')
        command = [
            sys.executable, str(gate), '--qemu', str(self.qemu_bin),
            '--eeprom', str(eeprom), '--tftp-root', str(tftp_root),
            '--manifest', str(manifest), '--output', str(report_path),
            '--pcap-output', str(pcap_path), '--timeout', '60',
        ]
        eeprom_update = manifest_data.get('eeprom_update')
        if eeprom_update is not None:
            self.assertEqual(
                eeprom_update.get('expected_result'), 'success',
                'the exact network handoff test requires a successful '
                'EEPROM update expectation')
            command.append('--allow-eeprom-update')
        if physical_pcap_name is not None:
            physical_pcap = Path(physical_pcap_name).resolve(strict=True)
            command.extend([
                '--cadence-reference', str(physical_pcap),
                '--cadence-reference-client-mac', physical_mac,
                '--cadence-output', str(cadence_path),
            ])
        run = subprocess.run(
            command,
            capture_output=True, text=True, timeout=90)
        self.assertEqual(
            run.returncode, 0,
            f'network gate failed:\n{run.stdout}\n{run.stderr}')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        self.assertEqual(report['inputs_unchanged'],
                         eeprom_update is None)
        transition = report['eeprom']['transition']
        self.assertTrue(transition['verified'])
        if eeprom_update is None:
            self.assertFalse(transition['changed'])
        else:
            self.assertEqual(report['eeprom']['sha256'],
                             eeprom_update['sha256'])
            self.assertEqual(report['qmp']['boot-self-update-status'],
                             'up-to-date')
            self.assertTrue(report['eeprom_update_trace']['verified'])
            self.assertTrue(report['eeprom_update_wire']['verified'])
        self.assertEqual(report['qmp']['boot-source'], 'network')
        self.assertEqual(report['qmp']['arm-handoff-status'], 'ready')
        self.assertEqual(report['qmp']['firmware-sha256'],
                         self.ASSET_START4.hash)
        self.assertEqual(report['qmp']['firmware-fixup-sha256'],
                         self.ASSET_FIXUP4.hash)
        self.assertEqual(report['qmp']['firmware-kernel-sha256'],
                         self.KERNEL8_SHA256)
        self.assertEqual(report['qmp']['firmware-device-tree-sha256'],
                         self.BCM2711_DTB_SHA256)
        self.assertEqual(report['qmp']['firmware-initramfs-sha256'],
                         self.ASSET_INITRD.hash)
        capture = report['capture']
        self.assertEqual(Path(capture['path']), pcap_path)
        self.assertEqual(capture['sha256'], self._sha256_file(pcap_path))
        self.assertGreater(capture['packets'], 0)
        self.assertGreater(capture['captured_bytes'], 0)
        self.assertEqual(capture['link_type'], 'ethernet')
        if physical_pcap_name is None:
            self.assertIsNone(report['cadence'])
        else:
            cadence = report['cadence']
            self.assertTrue(cadence['match'])
            self.assertEqual(cadence['schema'],
                             'qemu-rpi-network-cadence-v1')
            self.assertEqual(cadence['candidate']['sha256'],
                             capture['sha256'])
            self.assertEqual(cadence,
                             json.loads(cadence_path.read_text(
                                 encoding='utf-8')))

    @skipIfMissingEnv('QEMU_RPI_DEFAULT_HOST_LIVE')
    def test_arm_raspi4_default_host_live(self):
        """Boot the pinned official signed installer through live HTTPS."""
        eeprom = Path(self.ASSET_PIEEPROM.fetch()).resolve(strict=True)
        self.assertEqual(self._sha256_file(eeprom),
                         self.ASSET_PIEEPROM.hash)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,'
            'network-boot-wire=on,net-install-requested=on')
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,snapshot=on,'
                       f'file={eeprom}'),
            '-nic', 'user,model=bcm2711-genet',
            '-no-reboot')
        self.vm.launch()

        deadline = time.monotonic() + 90
        state = None
        while time.monotonic() < deadline:
            state = self.vm.cmd(
                'qom-get', path='/machine', property='boot-state')
            if state == 'arm-handoff-ready':
                break
            time.sleep(0.05)
        self.assertEqual(
            state, 'arm-handoff-ready',
            'live default-host HTTPS did not reach ARM handoff')

        def machine_property(name):
            return self.vm.cmd(
                'qom-get', path='/machine', property=name)

        self.assertEqual(machine_property('boot-source'), 'http')
        self.assertEqual(machine_property('secure-boot-status'),
                         'image-verified')
        self.assertEqual(machine_property('arm-handoff-status'), 'ready')
        self.assertEqual(machine_property('boot-http-host'),
                         'fw-download-alias1.raspberrypi.com')
        self.assertEqual(machine_property('boot-http-port'), 443)
        self.assertEqual(machine_property('http-tls-creds'), '')
        self.assertEqual(machine_property('secure-boot-image-size'),
                         self.DEFAULT_HOST_BOOT_IMAGE_SIZE)
        self.assertEqual(machine_property('secure-boot-image-sha256'),
                         self.DEFAULT_HOST_BOOT_IMAGE_SHA256)
        self.assertEqual(machine_property('firmware-file'), 'start4.elf')
        self.assertEqual(machine_property('firmware-sha256'),
                         self.DEFAULT_HOST_START4_SHA256)
        self.assertEqual(machine_property('firmware-fixup-file'),
                         'fixup4.dat')
        self.assertEqual(machine_property('firmware-fixup-sha256'),
                         self.DEFAULT_HOST_FIXUP4_SHA256)
        self.assertEqual(machine_property('firmware-kernel-file'),
                         'Image.gz')
        self.assertEqual(machine_property('firmware-kernel-sha256'),
                         self.DEFAULT_HOST_KERNEL_SHA256)
        self.assertEqual(machine_property('firmware-device-tree-file'),
                         'bcm2711-rpi-4-b.dtb')
        self.assertEqual(machine_property('firmware-device-tree-sha256'),
                         self.DEFAULT_HOST_DTB_SHA256)
        self.assertEqual(machine_property('firmware-initramfs-file'),
                         'rootfs.cpio.zst')
        self.assertEqual(machine_property('firmware-initramfs-sha256'),
                         self.DEFAULT_HOST_INITRAMFS_SHA256)

    def test_arm_raspi4_production_usb_boot(self):
        """Select an unchanged production FAT image through USB BOOT_ORDER."""
        image_path, writable_eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-usb-boot.img')

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,'
            'usb-boot-drive=usbboot')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={writable_eeprom_path}'),
            '-drive', f'if=none,id=usbboot,format=raw,file={image_path}',
            '-nic', 'none',
            '-no-reboot')
        self.vm.launch()

        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-attempt-count'), 2)
        self.assertTrue(
            self.vm.cmd('qom-get', path='/machine',
                        property='usb-boot-identity-valid'))
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='usb-boot-version'), 3)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='usb-boot-route-string'), 0)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='usb-boot-root-hub-port'), 1)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='usb-boot-selected-lun'), 0)
        self._assert_pi4_production_boot(self.vm, command_line, 'usb-msd')

    def test_arm_raspi4_production_mini_uart_boot(self):
        """Boot unchanged production artifacts through official serial0."""
        image_path, writable_eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-mini-uart.img', mini_uart=True)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        self.vm.set_console(console_index=1)
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={writable_eeprom_path}'),
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-nic', 'none',
            '-no-reboot')
        self.vm.launch()

        self._assert_pi4_production_boot(self.vm, command_line, 'sd-card')

    @skipIfMissingEnv('QEMU_RPI_RELEASE_RAW_IMAGE')
    def test_arm_raspi4_exact_release_sd_first_boot(self):
        """Boot an exact pinned Raspberry Pi OS image from virtual SD."""
        source = Path(
            os.environ['QEMU_RPI_RELEASE_RAW_IMAGE']).resolve(strict=True)
        self.assertEqual(source.stat().st_size, self.RPIOS_LITE_RAW_SIZE)
        self.assertEqual(
            self._sha256_prefix(source, self.RPIOS_LITE_RAW_SIZE),
            self.RPIOS_LITE_RAW_SHA256,
            'Raspberry Pi OS raw release payload differs')
        self._assert_release_image_identities(source)

        image_path = Path(self.workdir) / 'raspi4-exact-release.img'
        eeprom_path = Path(self.workdir) / 'raspi4-exact-pieeprom.bin'
        shutil.copyfile(source, image_path)
        shutil.copyfile(self.ASSET_PIEEPROM.fetch(), eeprom_path)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom')
        self.vm.set_console(console_index=1)
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={eeprom_path}'),
            '-drive', f'if=sd,format=raw,file={image_path}')
        self.vm.launch()

        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-state'),
            'arm-handoff-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-source'),
            'sd-card')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='firmware-overlay-file'),
            'overlays/vc4-kms-v3d-pi4.dtbo')
        self.wait_for_console_pattern('raspberrypi login:', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='kernel-started')
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='userspace-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-health'),
            'userspace-ready')
        console = Path(self.log_file('console.log')).read_text(
            encoding='utf-8', errors='replace')
        self.assertIn(
            'bcmgenet fd580000.ethernet: GENET 5.0', console,
            'exact release did not enumerate the native GENET controller')
        self.assertNotIn(
            'raspberrypi-exp-gpio soc:firmware:gpio: Failed', console)

        # The release's own first-boot userspace changes the disk identifier.
        # This guest-originated durable write proves execution beyond handoff;
        # it must not be substituted by a host-side test mutation.
        deadline = time.monotonic() + 180
        observed = None
        while time.monotonic() < deadline:
            observed = self._mbr_state(image_path)
            if observed[0] != self.RPIOS_LITE_DISK_ID:
                break
            time.sleep(0.05)
        self.assertIsNotNone(observed)
        self.assertNotEqual(observed[0], self.RPIOS_LITE_DISK_ID)
        self.assertEqual(observed[1], 1_064_960)
        self.assertEqual(observed[2], 4_751_360)
        self.assertTrue(self.vm.is_running(),
                        'release did not remain alive after first-boot reset')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-state'),
            'arm-handoff-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-source'),
            'sd-card')

    @skipIfMissingEnv('QEMU_RPI_RELEASE_RAW_IMAGE')
    def test_arm_raspi4_exact_release_usb_first_boot(self):
        """Boot the exact pinned Raspberry Pi OS image through USB-MSD."""
        source = Path(
            os.environ['QEMU_RPI_RELEASE_RAW_IMAGE']).resolve(strict=True)
        self.assertEqual(source.stat().st_size, self.RPIOS_LITE_RAW_SIZE)
        self.assertEqual(
            self._sha256_prefix(source, self.RPIOS_LITE_RAW_SIZE),
            self.RPIOS_LITE_RAW_SHA256,
            'Raspberry Pi OS raw release payload differs')
        self._assert_release_image_identities(source)

        image_path = Path(self.workdir) / 'raspi4-exact-release-usb.img'
        eeprom_path = Path(self.workdir) / 'raspi4-exact-usb-pieeprom.bin'
        shutil.copyfile(source, image_path)
        shutil.copyfile(self.ASSET_PIEEPROM.fetch(), eeprom_path)
        self.assertEqual(
            self._sha256_prefix(image_path, self.RPIOS_LITE_RAW_SIZE),
            self.RPIOS_LITE_RAW_SHA256,
            'virtual USB-MSD changed the release payload before boot')

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,'
            'usb-boot-drive=usbboot')
        self.vm.set_console(console_index=1)
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={eeprom_path}'),
            '-drive', f'if=none,id=usbboot,format=raw,file={image_path}')
        self.vm.launch()

        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-state'),
            'arm-handoff-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-source'),
            'usb-msd')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-attempt-count'), 2)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='usb-boot-transport'),
            'vl805-xhci-host-bot-scsi-read10-v1')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='firmware-overlay-file'),
            'overlays/vc4-kms-v3d-pi4.dtbo')
        self.wait_for_console_pattern('raspberrypi login:', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='kernel-started')
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='userspace-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-health'),
            'userspace-ready')
        console = Path(self.log_file('console.log')).read_text(
            encoding='utf-8', errors='replace')
        self.assertIn('USB Mass Storage device detected', console)
        self.assertNotIn(
            'raspberrypi-exp-gpio soc:firmware:gpio: Failed', console)

        # First-boot userspace must update and expand the same image exposed
        # through VL805/xHCI, proving rootfs writes beyond firmware handoff.
        deadline = time.monotonic() + 180
        observed = None
        while time.monotonic() < deadline:
            observed = self._mbr_state(image_path)
            if observed[0] != self.RPIOS_LITE_DISK_ID:
                break
            time.sleep(0.05)
        self.assertIsNotNone(observed)
        self.assertNotEqual(observed[0], self.RPIOS_LITE_DISK_ID)
        self.assertEqual(observed[1], 1_064_960)
        self.assertEqual(observed[2], 4_751_360)
        self.assertTrue(self.vm.is_running(),
                        'USB release did not remain alive after first boot')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-state'),
            'arm-handoff-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-source'),
            'usb-msd')

    @skipIfMissingEnv('QEMU_RPI_RELEASE_RAW_IMAGE')
    def test_arm_cm4_exact_release_emmc_first_boot(self):
        """Boot the exact pinned Raspberry Pi OS image from CM4 eMMC."""
        source = Path(
            os.environ['QEMU_RPI_RELEASE_RAW_IMAGE']).resolve(strict=True)
        self.assertEqual(source.stat().st_size, self.RPIOS_LITE_RAW_SIZE)
        self.assertEqual(
            self._sha256_prefix(source, self.RPIOS_LITE_RAW_SIZE),
            self.RPIOS_LITE_RAW_SHA256,
            'Raspberry Pi OS raw release payload differs')
        self._assert_release_image_identities(source)

        emmc_path = Path(self.workdir) / 'cm4-exact-release-emmc.img'
        eeprom_path = Path(self.workdir) / 'cm4-exact-pieeprom.bin'
        shutil.copyfile(source, emmc_path)
        os.truncate(emmc_path, self.CM4_RELEASE_EMMC_SIZE)
        shutil.copyfile(self.ASSET_PIEEPROM.fetch(), eeprom_path)
        self.assertEqual(emmc_path.stat().st_size,
                         self.CM4_RELEASE_EMMC_SIZE)
        self.assertEqual(
            self._sha256_prefix(emmc_path, self.RPIOS_LITE_RAW_SIZE),
            self.RPIOS_LITE_RAW_SHA256,
            'virtual eMMC changed the release payload')

        self.set_machine('raspi-cm4')
        self.vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,'
            'emmc-drive=emmc')
        self.vm.set_console(console_index=1)
        self.vm.add_args(
            '-drive', ('if=none,id=pieeprom,format=raw,'
                       f'file={eeprom_path}'),
            '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}')
        self.vm.launch()

        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-state'),
            'arm-handoff-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-source'),
            'emmc')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='firmware-device-tree-file'),
            'bcm2711-rpi-cm4.dtb')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='firmware-overlay-file'),
            'overlays/vc4-kms-v3d-pi4.dtbo')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='recovery-status'),
            'none')
        self.wait_for_console_pattern('raspberrypi login:', vm=self.vm)
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='kernel-started')
        self.vm.cmd('qom-set', path='/machine', property='boot-health',
                    value='userspace-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-health'),
            'userspace-ready')
        console = Path(self.log_file('console.log')).read_text(
            encoding='utf-8', errors='replace')
        self.assertIn('mmc0: new high speed MMC card', console)
        self.assertIn('mmcblk0: mmc0:0001 QEMU!! 4.00 GiB', console)
        self.assertNotIn('Got data interrupt', console)
        self.assertNotIn('I/O error, dev mmcblk0', console)
        self.assertNotIn(
            'raspberrypi-exp-gpio soc:firmware:gpio: Failed', console)

        expected_sectors = (self.CM4_RELEASE_EMMC_SIZE // 512 -
                            1_064_960)
        deadline = time.monotonic() + 180
        observed = None
        while time.monotonic() < deadline:
            disk_id, partition_lba, partition_sectors = self._mbr_state(
                emmc_path)
            blocks = self._ext4_block_count(emmc_path, partition_lba)
            observed = (disk_id, partition_lba, partition_sectors, blocks)
            if (disk_id != self.RPIOS_LITE_DISK_ID and
                    partition_lba == 1_064_960 and
                    partition_sectors == expected_sectors and
                    blocks == expected_sectors * 512 // 4096):
                break
            time.sleep(0.05)
        self.assertIsNotNone(observed)
        self.assertNotEqual(observed[0], self.RPIOS_LITE_DISK_ID)
        self.assertEqual(observed[1], 1_064_960)
        self.assertEqual(observed[2], expected_sectors)
        self.assertEqual(observed[3], expected_sectors * 512 // 4096)
        self.assertTrue(self.vm.is_running(),
                        'CM4 release stopped after first-boot expansion')

    def test_arm_raspi4_production_eeprom_recovery(self):
        """Update official EEPROM, auto-reboot, and boot the same SD."""
        update = Path(self.ASSET_PIEEPROM.fetch()).read_bytes()
        initial = Path(self.ASSET_OLD_PIEEPROM.fetch()).read_bytes()
        recovery = Path(self.ASSET_RECOVERY.fetch()).read_bytes()
        signature = (self.ASSET_PIEEPROM.hash + '\nts: 1\n').encode()

        image_path, eeprom_path, command_line = \
            self._prepare_pi4_production_boot_media(
                'raspi4-production-recovery.img', extra_files=[
                    ('recovery.bin', 'RECOVERYBIN', recovery),
                    ('pieeprom.upd', 'PIEEPROMUPD', update),
                    ('pieeprom.sig', 'PIEEPROMSIG', signature),
                ])
        Path(eeprom_path).write_bytes(initial)

        self.set_machine('raspi4b')
        self.vm.set_machine(
            'raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,'
            f'recovery-trusted-sha256={self.ASSET_RECOVERY.hash}')
        self.vm.set_console()
        self.vm.add_args(
            '-drive', f'if=none,id=pieeprom,format=raw,file={eeprom_path}',
            '-drive', f'if=sd,format=raw,file={image_path}',
            '-nic', 'none')
        self.vm.launch()

        deadline = time.monotonic() + 10
        state = None
        while time.monotonic() < deadline:
            state = self.vm.cmd('qom-get', path='/machine',
                                property='boot-state')
            if state == 'arm-handoff-ready':
                break
            time.sleep(0.01)
        self.assertEqual(state, 'arm-handoff-ready',
                         'recovery did not automatically reboot into SD')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='recovery-status'),
            'none')
        self._assert_pi4_production_boot(
            self.vm, command_line, 'sd-card')
        self.vm.shutdown()
        self.assertEqual(Path(eeprom_path).read_bytes(), update)
        self.assertIn(b'RECOVERY000', Path(image_path).read_bytes())

    def test_arm_cm4_production_emmc_boot(self):
        """Boot an exact user image with separate eMMC hardware areas."""
        emmc_path = os.path.join(self.workdir, 'cm4-production-emmc.img')
        eeprom_path = os.path.join(self.workdir, 'cm4-pieeprom.bin')
        boot_path = Path(self.workdir) / 'cm4-emmc-boot-partitions.bin'
        rpmb_path = Path(self.workdir) / 'cm4-emmc-rpmb.bin'
        command_line = self._prepare_cm4_production_media(emmc_path,
                                                           eeprom_path)
        boot_path.write_bytes(bytes(512 * 1024))
        rpmb_path.write_bytes(bytes(128 * 1024))
        user_digest = hashlib.sha256(Path(emmc_path).read_bytes()).digest()

        self.set_machine('raspi-cm4')
        self._configure_cm4_boot_vm(
            self.vm, emmc_path, eeprom_path,
            emmc_boot_path=boot_path, emmc_rpmb_path=rpmb_path,
            emmc_cid='aa0151434d34454d5510123456782900')
        self.vm.launch()
        self._assert_cm4_production_boot(self.vm, command_line)
        exec_command_and_wait_for_pattern(self, 'cat /proc/cpuinfo',
                                          'BCM2835')
        exec_command_and_wait_for_pattern(
            self, 'echo BOOT0=$(cat /sys/class/block/mmcblk0boot0/size)',
            'BOOT0=512')
        exec_command_and_wait_for_pattern(
            self, 'echo BOOT1=$(cat /sys/class/block/mmcblk0boot1/size)',
            'BOOT1=512')
        exec_command_and_wait_for_pattern(
            self, 'test -e /dev/mmcblk0rpmb && echo RPMB=present',
            'RPMB=present')
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/block/mmcblk0/device/cid',
            'aa0151434d34454d5510123456782900')
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/class/block/mmcblk0/device/name', 'CM4EMU')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='emmc-boot-drive'), 'emmcboot')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='emmc-rpmb-drive'), 'emmcrpmb')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='emmc-cid'),
            'aa0151434d34454d5510123456782900')
        self.assertEqual(hashlib.sha256(Path(emmc_path).read_bytes()).digest(),
                         user_digest,
                         'hidden eMMC areas changed exact user image bytes')

    def test_arm_cm4_emmc_controller_timeout_recovery(self):
        """Expose a deterministic SDHCI data timeout to Linux and recover."""
        emmc_path = os.path.join(self.workdir, 'cm4-timeout-emmc.img')
        eeprom_path = os.path.join(self.workdir, 'cm4-timeout-pieeprom.bin')
        command_line = self._prepare_cm4_production_media(emmc_path,
                                                           eeprom_path)

        self.set_machine('raspi-cm4')
        self._configure_cm4_boot_vm(
            self.vm, emmc_path, eeprom_path,
            emmc_data_error='timeout', emmc_data_error_count=1)
        self.vm.launch()
        self._assert_cm4_production_boot(self.vm, command_line)
        console = Path(self.log_file('console.log')).read_text(
            encoding='utf-8', errors='replace')
        self.assertIn('mmc0: error -110 whilst initialising MMC card', console)
        self.assertIn('mmc0: new high speed MMC card', console)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='emmc-data-error'), 'timeout')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='emmc-data-error-count'), 1)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine/soc/peripherals/emmc2',
                        property='data-errors-injected'), 1)

    def test_arm_cm4_emmc_controller_crc_recovery(self):
        """Expose a deterministic SDHCI data CRC error to Linux and recover."""
        emmc_path = os.path.join(self.workdir, 'cm4-crc-emmc.img')
        eeprom_path = os.path.join(self.workdir, 'cm4-crc-pieeprom.bin')
        command_line = self._prepare_cm4_production_media(emmc_path,
                                                           eeprom_path)

        self.set_machine('raspi-cm4')
        self._configure_cm4_boot_vm(
            self.vm, emmc_path, eeprom_path,
            emmc_data_error='crc', emmc_data_error_count=1)
        self.vm.launch()
        self._assert_cm4_production_boot(self.vm, command_line)
        console = Path(self.log_file('console.log')).read_text(
            encoding='utf-8', errors='replace')
        self.assertIn('mmc0: error -84 whilst initialising MMC card', console)
        self.assertIn('mmc0: new high speed MMC card', console)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='emmc-data-error'), 'crc')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine/soc/peripherals/emmc2',
                        property='data-errors-injected'), 1)

    def test_arm_cm4_stale_qemu_owner_recovery(self):
        """Recover only an unlocked stale QEMU owner, with explicit opt-in."""
        emmc_path = os.path.join(self.workdir, 'cm4-stale-owner-emmc.img')
        eeprom_path = os.path.join(self.workdir,
                                   'cm4-stale-owner-pieeprom.bin')
        lifecycle = Path(self.workdir) / 'cm4-stale-owner.state'
        command_line = self._prepare_cm4_production_media(emmc_path,
                                                           eeprom_path)
        lifecycle.write_text('qemu-owned\n', encoding='ascii')
        machine = ('raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,'
                   f'emmc-drive=emmc,provision-state-file={lifecycle}')
        rejected = subprocess.run(
            [self.qemu_bin, '-M', machine,
             '-drive', ('if=none,id=pieeprom,format=raw,'
                        f'file={eeprom_path}'),
             '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}',
             '-display', 'none'],
            capture_output=True, text=True, timeout=10)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("found 'qemu-owned'", rejected.stderr)
        self.assertEqual(lifecycle.read_text(encoding='ascii'),
                         'qemu-owned\n')

        lifecycle.write_text('flash-failed\n', encoding='ascii')
        rejected_flash = subprocess.run(
            [self.qemu_bin, '-M', machine + ',provision-recover-stale=on',
             '-drive', ('if=none,id=pieeprom,format=raw,'
                        f'file={eeprom_path}'),
             '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}',
             '-display', 'none'],
            capture_output=True, text=True, timeout=10)
        self.assertNotEqual(rejected_flash.returncode, 0)
        self.assertIn("found 'flash-failed'", rejected_flash.stderr)
        self.assertEqual(lifecycle.read_text(encoding='ascii'),
                         'flash-failed\n')
        lifecycle.write_text('qemu-owned\n', encoding='ascii')

        self.set_machine('raspi-cm4')
        self._configure_cm4_boot_vm(
            self.vm, emmc_path, eeprom_path, provision_state=lifecycle,
            provision_recover_stale=True)
        self.vm.launch()
        self._assert_cm4_production_boot(self.vm, command_line)
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='provision-state'), 'qemu-owned')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='provision-recovery'), 'stale-qemu-owned')

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR')
    def test_arm_cm4_inprocess_rpiboot_usb(self):
        """Drive the in-process BCM2711 ROM with unchanged official rpiboot."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        proxy_value = os.environ.get('QEMU_RPI_DWC2_PROXY')
        proxy = (Path(proxy_value).resolve(strict=True)
                 if proxy_value else
                 Path(self.qemu_bin).resolve().with_name(
                     'qemu-rpi-dwc2-raw-gadget-proxy'))
        # AF_UNIX paths are limited to 107 bytes on Linux; functional-test
        # scratch directories can be substantially longer than that.
        socket_path = Path(
            f'/tmp/qemu-rpi-dwc2-{os.getpid()}-{id(self):x}.sock')

        for executable in (rpiboot, proxy):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertTrue(artifact.is_file(),
                            f'missing official RPIBOOT file: {artifact}')
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        sudo_check = subprocess.run(
            ['sudo', '-n', 'true'], capture_output=True, text=True)
        if sudo_check.returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')

        self._require_raw_gadget_modules()
        emmc_path = Path(self.workdir) / 'cm4-inprocess-rpiboot-emmc.img'
        with emmc_path.open('wb') as target:
            target.truncate(64 * 1024 * 1024)
        self.set_machine('raspi-cm4')
        self.vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
            'emmc-drive=emmc,'
            'rpiboot-bootcode-trusted-sha256='
            f"{self.RPIBOOT_FILE_SHA256['bootcode4.bin']}")
        self.vm.set_console()
        self.vm.add_args(
            '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}',
            '-chardev',
            f'socket,id=dwc2dev,path={socket_path},server=on,wait=off',
            '-global', 'dwc2-usb.device-chardev=dwc2dev')
        self.vm.launch()

        helper = subprocess.Popen(
            ['sudo', '-n', str(proxy), '--dwc2-socket', str(socket_path)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(0.5)
            if helper.poll() is not None:
                output = helper.communicate()[0]
                self.fail(f'DWC2 USB proxy exited before rpiboot:\n{output}')
            host = subprocess.run(
                ['sudo', '-n', str(rpiboot), '-v', '-d', str(boot_dir)],
                capture_output=True, text=True, timeout=90)
            proxy_output = helper.communicate(timeout=30)[0]
            self.assertEqual(
                host.returncode, 0,
                f'unchanged rpiboot failed:\n{host.stdout}{host.stderr}\n'
                f'{proxy_output}')
            self.assertEqual(
                helper.returncode, 0,
                f'DWC2 USB proxy failed:\n{proxy_output}')
            self.assertIn(self.USBBOOT_GIT, host.stdout)
            self.assertIn('Second stage boot server done', host.stdout)
            self.assertIn(
                'unchanged rpiboot completed through emulated DWC2',
                proxy_output)
        finally:
            if helper.poll() is None:
                subprocess.run(['sudo', '-n', 'kill', str(helper.pid)],
                               check=False)
                helper.communicate(timeout=10)

        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine', property='boot-state'),
            'arm-handoff-ready')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='boot-source'), 'rpiboot')
        self.assertEqual(
            self.vm.cmd('qom-get', path='/machine',
                        property='arm-handoff-status'), 'ready')
        for name, prefix in (
                ('bootcode4.bin', 'rpiboot-bootcode'),
                ('config.txt', 'rpiboot-config'),
                ('boot.img', 'rpiboot-boot-img')):
            artifact = boot_dir / name
            self.assertEqual(
                self.vm.cmd('qom-get', path='/machine',
                            property=f'{prefix}-size'),
                artifact.stat().st_size)
            self.assertEqual(
                self.vm.cmd('qom-get', path='/machine',
                            property=f'{prefix}-sha256'),
                self.RPIBOOT_FILE_SHA256[name])

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR')
    def test_arm_cm4_inprocess_rpiboot_usb_resets(self):
        """Run repeated active ROM/file reset campaigns through DWC2."""
        reset_count = 4
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        proxy_value = os.environ.get('QEMU_RPI_DWC2_PROXY')
        proxy = (Path(proxy_value).resolve(strict=True)
                 if proxy_value else
                 Path(self.qemu_bin).resolve().with_name(
                     'qemu-rpi-dwc2-raw-gadget-proxy'))
        socket_path = Path(
            f'/tmp/qemu-rpi-reset-{os.getpid()}-{id(self):x}.sock')

        for executable in (rpiboot, proxy):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        if subprocess.run(['sudo', '-n', 'true'],
                          capture_output=True).returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')
        self._require_raw_gadget_modules()

        self.set_machine('raspi-cm4')
        self.vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
            'rpiboot-bootcode-trusted-sha256='
            f"{self.RPIBOOT_FILE_SHA256['bootcode4.bin']}")
        self.vm.add_args(
            '-chardev',
            f'socket,id=dwc2dev,path={socket_path},server=on,wait=off',
            '-global', 'dwc2-usb.device-chardev=dwc2dev')
        self.vm.launch()
        helper = subprocess.Popen(
            ['sudo', '-n', str(proxy), '--dwc2-socket', str(socket_path),
             '--reset-after', '4096',
             '--reset-count', str(reset_count)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            start_new_session=True)
        helper_lines = []

        def drain_helper():
            for line in helper.stdout:
                helper_lines.append(line)

        drain = threading.Thread(target=drain_helper, daemon=True)
        drain.start()
        hosts = []

        def start_host():
            process = subprocess.Popen(
                ['sudo', '-n', 'timeout', '90s', str(rpiboot),
                 '-v', '-d', str(boot_dir)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                start_new_session=True)
            hosts.append(process)
            return process

        host = start_host()

        def wait_for_marker(marker, timeout):
            nonlocal host
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                output = ''.join(helper_lines)
                if marker in output:
                    return
                if helper.poll() is not None:
                    self.fail(f'proxy exited before {marker!r}:\n{output}')
                if host.poll() is not None:
                    host.communicate()
                    host = start_host()
                time.sleep(0.01)
            self.fail(f'proxy did not reach {marker!r}:\n'
                      f'{"".join(helper_lines)}')

        try:
            for stage in ('ROM', 'file-server'):
                for cycle in range(1, reset_count + 1):
                    wait_for_marker(
                        f'waiting for host USB reset during {stage} after '
                        f'4096 bytes (cycle {cycle}/{reset_count})', 60)
                    reset = subprocess.run(
                        ['sudo', '-n', 'usbreset', '0a5c:2711'],
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(
                        reset.returncode, 0,
                        f'{stage} USB reset cycle {cycle} failed:\n'
                        f'{reset.stdout}{reset.stderr}')
                    wait_for_marker(
                        f'observed host USB reset during {stage} '
                        f'(cycle {cycle}/{reset_count})', 15)
            wait_for_marker(
                'unchanged rpiboot completed through emulated DWC2', 90)
            helper.wait(timeout=90)
            drain.join(timeout=10)
            self.assertEqual(helper.returncode, 0,
                             ''.join(helper_lines))
            self.assertIn(
                'unchanged rpiboot completed through emulated DWC2',
                ''.join(helper_lines))
            if host.poll() is None:
                host.communicate(timeout=30)
            self.assertEqual(
                self.vm.cmd('qom-get', path='/machine',
                            property='boot-state'),
                'rpiboot-complete')
            for name, prefix in (
                    ('bootcode4.bin', 'rpiboot-bootcode'),
                    ('config.txt', 'rpiboot-config'),
                    ('boot.img', 'rpiboot-boot-img')):
                artifact = boot_dir / name
                self.assertEqual(
                    self.vm.cmd('qom-get', path='/machine',
                                property=f'{prefix}-size'),
                    artifact.stat().st_size)
                self.assertEqual(
                    self.vm.cmd('qom-get', path='/machine',
                                property=f'{prefix}-sha256'),
                    self.RPIBOOT_FILE_SHA256[name])
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-KILL', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            helper.wait(timeout=10)
            drain.join(timeout=10)
            if helper.stdout and not helper.stdout.closed:
                helper.stdout.close()
            for process in hosts:
                if process.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{process.pid}'],
                        check=False, capture_output=True, text=True)
                process.communicate(timeout=10)

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR')
    def test_arm_cm4_inprocess_rpiboot_disconnect_retry(self):
        """Recover both stages after disconnect and proxy SIGKILL."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        proxy_value = os.environ.get('QEMU_RPI_DWC2_PROXY')
        proxy = (Path(proxy_value).resolve(strict=True)
                 if proxy_value else
                 Path(self.qemu_bin).resolve().with_name(
                     'qemu-rpi-dwc2-raw-gadget-proxy'))

        for executable in (rpiboot, proxy):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        if subprocess.run(['sudo', '-n', 'true'],
                          capture_output=True).returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')
        self._require_raw_gadget_modules()

        for fault_kind, stage in (
                ('disconnect', 'rom'),
                ('disconnect', 'file-server'),
                ('sigkill', 'rom'),
                ('sigkill', 'file-server')):
            socket_path = Path(
                f'/tmp/qemu-rpi-{fault_kind[:4]}-{stage[0]}-{os.getpid()}-'
                f'{id(self):x}.sock')
            lifecycle = (
                Path(self.workdir) /
                f'inprocess-{fault_kind}-{stage}.state')
            vm = self.get_vm(
                name=f'cm4-rpiboot-{fault_kind}-{stage}')
            vm.set_machine(
                'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
                'rpiboot-bootcode-trusted-sha256='
                f"{self.RPIBOOT_FILE_SHA256['bootcode4.bin']},"
                f'provision-state-file={lifecycle}')
            vm.add_args(
                '-chardev',
                f'socket,id=dwc2dev,path={socket_path},server=on,wait=off',
                '-global', 'dwc2-usb.device-chardev=dwc2dev')
            vm.launch()
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'qemu-rpiboot-wait')
            boundary_args = (
                ['--disconnect-after', '4096',
                 '--disconnect-stage', stage]
                if fault_kind == 'disconnect' else
                ['--hold-after', '4096', '--hold-stage', stage])
            fault = subprocess.Popen(
                ['sudo', '-n', str(proxy),
                 '--dwc2-socket', str(socket_path)] + boundary_args,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                start_new_session=True)
            active_deadline = time.monotonic() + 5
            while (lifecycle.read_text(encoding='ascii').strip() !=
                   'rpiboot-active' and
                   time.monotonic() < active_deadline):
                time.sleep(0.01)
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'rpiboot-active')
            host = subprocess.Popen(
                ['sudo', '-n', 'timeout', '90s', str(rpiboot),
                 '-v', '-d', str(boot_dir)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                start_new_session=True)
            try:
                if fault_kind == 'disconnect':
                    fault_output = fault.communicate(timeout=60)[0]
                    self.assertEqual(
                        fault.returncode, 75,
                        f'{stage} proxy did not inject disconnect:\n'
                        f'{fault_output}')
                    expected = (
                        f'injected disconnect during '
                        f'{"ROM" if stage == "rom" else "file-server"} '
                        'after 4096 bytes')
                    self.assertIn(expected, fault_output)
                else:
                    expected = (
                        f'holding '
                        f'{"ROM" if stage == "rom" else "file-server"} '
                        'bulk transfer after 4096 bytes for external '
                        'termination')
                    fault_output = ''
                    deadline = time.monotonic() + 60
                    while expected not in fault_output:
                        if fault.poll() is not None:
                            fault_output += fault.communicate()[0]
                            self.fail(
                                f'{stage} proxy exited before SIGKILL '
                                f'boundary:\n{fault_output}')
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            self.fail(
                                f'{stage} proxy did not reach SIGKILL '
                                f'boundary:\n{fault_output}')
                        readable, _, _ = select.select(
                            [fault.stdout], [], [], min(0.5, remaining))
                        if readable:
                            fault_output += fault.stdout.readline()
                    self.assertEqual(
                        vm.cmd('qom-get', path='/machine',
                               property='rpiboot-transfer-received'),
                        4096)
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{fault.pid}'],
                        check=True, capture_output=True, text=True,
                        timeout=10)
                    fault_output += fault.communicate(timeout=10)[0]
                    self.assertNotEqual(fault.returncode, 0)
            finally:
                if fault.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{fault.pid}'], check=False,
                        capture_output=True, text=True)
                    fault.communicate(timeout=10)

            failed_deadline = time.monotonic() + 5
            while (lifecycle.read_text(encoding='ascii').strip() !=
                   'rpiboot-failed' and
                   time.monotonic() < failed_deadline):
                time.sleep(0.01)
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'rpiboot-failed')
            rollback_deadline = time.monotonic() + 5
            while (vm.cmd('qom-get', path='/machine',
                          property='rpiboot-transfer-received') != 0 and
                   time.monotonic() < rollback_deadline):
                time.sleep(0.01)
            self.assertEqual(
                vm.cmd('qom-get', path='/machine',
                       property='rpiboot-transfer-received'),
                0, f'{fault_kind} did not roll back partial {stage} data')

            fault_host_output = ''
            if stage == 'file-server':
                fault_host_output = host.communicate(timeout=20)[0]

            retry_args = [
                'sudo', '-n', str(proxy), '--dwc2-socket',
                str(socket_path)]
            if stage == 'file-server':
                retry_args.append('--second-stage-only')
            retry = subprocess.Popen(
                retry_args, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            try:
                time.sleep(0.5)
                if stage == 'file-server' or host.poll() is not None:
                    if stage != 'file-server':
                        fault_host_output = host.communicate()[0]
                    host = subprocess.Popen(
                        ['sudo', '-n', 'timeout', '90s', str(rpiboot),
                         '-v', '-d', str(boot_dir)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True)
                else:
                    pass
                clean_output = host.communicate(timeout=90)[0]
                if host.returncode and retry.poll() is None:
                    fault_host_output += clean_output
                    clean_host = subprocess.run(
                        ['sudo', '-n', str(rpiboot), '-v',
                         '-d', str(boot_dir)],
                        capture_output=True, text=True, timeout=90)
                    clean_output = clean_host.stdout + clean_host.stderr
                    clean_returncode = clean_host.returncode
                else:
                    clean_returncode = host.returncode
                retry_output = retry.communicate(timeout=30)[0]
                self.assertEqual(
                    clean_returncode, 0,
                    f'{fault_kind} {stage} clean retry failed:\n'
                    f'{fault_host_output}\n{clean_output}\n{retry_output}')
                self.assertEqual(retry.returncode, 0, retry_output)
                self.assertIn(self.USBBOOT_GIT,
                              fault_host_output + clean_output)
                self.assertIn(
                    'unchanged rpiboot completed through emulated DWC2',
                    retry_output)
            finally:
                if retry.poll() is None:
                    subprocess.run(['sudo', '-n', 'kill', str(retry.pid)],
                                   check=False)
                    retry.communicate(timeout=10)
                if host.poll() is None:
                    subprocess.run(['sudo', '-n', 'kill', str(host.pid)],
                                   check=False)
                    host.communicate(timeout=10)

            self.assertEqual(
                vm.cmd('qom-get', path='/machine', property='boot-state'),
                'rpiboot-complete')
            self.assertEqual(
                vm.cmd('qom-get', path='/machine',
                       property='provision-state'),
                'rpiboot-complete')
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'rpiboot-complete')
            for name, prefix in (
                    ('bootcode4.bin', 'rpiboot-bootcode'),
                    ('config.txt', 'rpiboot-config'),
                    ('boot.img', 'rpiboot-boot-img')):
                artifact = boot_dir / name
                self.assertEqual(
                    vm.cmd('qom-get', path='/machine',
                           property=f'{prefix}-size'),
                    artifact.stat().st_size)
                self.assertEqual(
                    vm.cmd('qom-get', path='/machine',
                           property=f'{prefix}-sha256'),
                    self.RPIBOOT_FILE_SHA256[name])
            vm.shutdown()
            self.assertEqual(
                lifecycle.read_text(encoding='ascii').strip(),
                'rpiboot-complete')

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR')
    def test_arm_cm4_inprocess_rpiboot_timeout_retry(self):
        """Cross official control deadlines and complete same-VM retries."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        proxy_value = os.environ.get('QEMU_RPI_DWC2_PROXY')
        proxy = (Path(proxy_value).resolve(strict=True)
                 if proxy_value else
                 Path(self.qemu_bin).resolve().with_name(
                     'qemu-rpi-dwc2-raw-gadget-proxy'))

        for executable in (rpiboot, proxy):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        if subprocess.run(['sudo', '-n', 'true'],
                          capture_output=True).returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')
        self._require_raw_gadget_modules()

        scenarios = (
            ('rom-status', False,
             ['--timeout', 'rom-status', '--timeout-ms', '20001'],
             'ROM-status control timeout'),
            ('file-request', True,
             ['--timeout', 'file-request', '--timeout-ms', '20001'],
             'file-server control timeout'),
            ('rom-bulk', False,
             ['--bulk-timeout-after', '4096',
              '--bulk-timeout-stage', 'rom',
              '--bulk-timeout-ms', '5001'],
             'ROM bulk timeout'),
            ('file-bulk', True,
             ['--bulk-timeout-after', '4096',
              '--bulk-timeout-stage', 'file-server',
              '--bulk-timeout-ms', '5001'],
             'file-server bulk timeout'),
        )
        for (timeout_name, second_stage, fault_args,
             expected_marker) in scenarios:
            socket_path = Path(
                f'/tmp/qemu-rpi-time-{timeout_name[0]}-{os.getpid()}-'
                f'{id(self):x}.sock')
            vm = self.get_vm(name=f'cm4-rpiboot-timeout-{timeout_name}')
            vm.set_machine(
                'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
                'rpiboot-bootcode-trusted-sha256='
                f"{self.RPIBOOT_FILE_SHA256['bootcode4.bin']}")
            vm.add_args(
                '-chardev',
                f'socket,id=dwc2dev,path={socket_path},server=on,wait=off',
                '-global', 'dwc2-usb.device-chardev=dwc2dev')
            vm.launch()
            fault = subprocess.Popen(
                ['sudo', '-n', str(proxy),
                 '--dwc2-socket', str(socket_path)] + fault_args,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                start_new_session=True)
            host = subprocess.Popen(
                ['sudo', '-n', 'timeout', '90s', str(rpiboot),
                 '-v', '-d', str(boot_dir)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                start_new_session=True)
            try:
                fault_output = fault.communicate(timeout=50)[0]
                self.assertEqual(
                    fault.returncode, 75,
                    f'{timeout_name} fault was not injected:\n'
                    f'{fault_output}')
                self.assertIn(
                    f'injected {expected_marker}', fault_output)
            finally:
                if fault.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-KILL', '--',
                         f'-{fault.pid}'], check=False,
                        capture_output=True, text=True)
                    fault.communicate(timeout=10)

            fault_host_output = ''
            if second_stage:
                fault_host_output = host.communicate(timeout=20)[0]
            retry_args = [
                'sudo', '-n', str(proxy), '--dwc2-socket',
                str(socket_path)]
            if second_stage:
                retry_args.append('--second-stage-only')
            retry = subprocess.Popen(
                retry_args, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            try:
                time.sleep(0.5)
                if second_stage or host.poll() is not None:
                    if not second_stage:
                        fault_host_output = host.communicate()[0]
                    host = subprocess.Popen(
                        ['sudo', '-n', 'timeout', '90s', str(rpiboot),
                         '-v', '-d', str(boot_dir)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True)
                clean_output = host.communicate(timeout=90)[0]
                if host.returncode and retry.poll() is None:
                    fault_host_output += clean_output
                    clean_host = subprocess.run(
                        ['sudo', '-n', str(rpiboot), '-v',
                         '-d', str(boot_dir)],
                        capture_output=True, text=True, timeout=90)
                    clean_output = clean_host.stdout + clean_host.stderr
                    clean_returncode = clean_host.returncode
                else:
                    clean_returncode = host.returncode
                retry_output = retry.communicate(timeout=30)[0]
                self.assertEqual(
                    clean_returncode, 0,
                    f'{timeout_name} clean retry failed:\n'
                    f'{fault_host_output}\n{clean_output}\n{retry_output}')
                self.assertEqual(retry.returncode, 0, retry_output)
                self.assertIn(self.USBBOOT_GIT,
                              fault_host_output + clean_output)
            finally:
                if retry.poll() is None:
                    subprocess.run(['sudo', '-n', 'kill', str(retry.pid)],
                                   check=False)
                    retry.communicate(timeout=10)
                if host.poll() is None:
                    subprocess.run(['sudo', '-n', 'kill', str(host.pid)],
                                   check=False)
                    host.communicate(timeout=10)

            self.assertEqual(
                vm.cmd('qom-get', path='/machine', property='boot-state'),
                'rpiboot-complete')
            for name, prefix in (
                    ('bootcode4.bin', 'rpiboot-bootcode'),
                    ('config.txt', 'rpiboot-config'),
                    ('boot.img', 'rpiboot-boot-img')):
                artifact = boot_dir / name
                self.assertEqual(
                    vm.cmd('qom-get', path='/machine',
                           property=f'{prefix}-size'),
                    artifact.stat().st_size)
                self.assertEqual(
                    vm.cmd('qom-get', path='/machine',
                           property=f'{prefix}-sha256'),
                    self.RPIBOOT_FILE_SHA256[name])
            vm.shutdown()

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR',
                      'QEMU_RPI_RAW_GADGET')
    def test_arm_cm4_rpiboot_sigkill_recovery(self):
        """Fail closed after SIGKILL in both BCM2711 USB enumerations."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        raw_gadget = Path(
            os.environ['QEMU_RPI_RAW_GADGET']).resolve(strict=True)
        for executable in (rpiboot, raw_gadget):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertTrue(artifact.is_file(),
                            f'missing official RPIBOOT file: {artifact}')
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        sudo_check = subprocess.run(
            ['sudo', '-n', 'true'], capture_output=True, text=True)
        if sudo_check.returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')

        emmc_path = Path(self.workdir) / 'cm4-rpiboot-kill-emmc.img'
        eeprom_path = Path(self.workdir) / 'cm4-rpiboot-kill-eeprom.bin'
        lifecycle = Path(self.workdir) / 'cm4-rpiboot-kill.state'
        self._prepare_cm4_production_media(emmc_path, eeprom_path)

        self.set_machine('raspi-cm4')
        wait_vm = self.get_vm(name='cm4-rpiboot-kill-wait')
        wait_vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
            'eeprom-drive=pieeprom,emmc-drive=emmc,'
            f'provision-state-file={lifecycle}')
        wait_vm.add_args(
            '-drive', f'if=none,id=pieeprom,format=raw,file={eeprom_path}',
            '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}')
        wait_vm.launch()
        self.assertEqual(
            wait_vm.cmd('qom-get', path='/machine', property='boot-state'),
            'rpiboot-wait')
        wait_vm.shutdown()
        self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                         'rpiboot-host-ready')

        self._require_raw_gadget_modules()
        self._exercise_rpiboot_sigkill(
            raw_gadget, rpiboot, boot_dir, lifecycle, 'bootcode-hold')
        self._exercise_rpiboot_sigkill(
            raw_gadget, rpiboot, boot_dir, lifecycle, 'file-hold', 'boot.img')

        capture = Path(self.workdir) / 'rpiboot-kill-clean-retry'
        capture.mkdir()
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_gadget),
             '--capture-dir', str(capture), '--lifecycle', str(lifecycle),
             '--mass-storage'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(0.5)
            self.assertIsNone(helper.poll(), 'clean Raw Gadget retry exited')
            host = subprocess.run(
                ['sudo', '-n', str(rpiboot), '-d', str(boot_dir)],
                capture_output=True, text=True, timeout=90)
            helper_output = helper.communicate(timeout=30)[0]
            self.assertEqual(
                host.returncode, 0,
                f'clean rpiboot retry failed:\n{host.stdout}{host.stderr}')
            self.assertEqual(
                helper.returncode, 0,
                f'clean Raw Gadget retry failed:\n{helper_output}')
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-complete')
            for name in self.RPIBOOT_FILE_SHA256:
                self.assertEqual((capture / name).read_bytes(),
                                 (boot_dir / name).read_bytes())
        finally:
            if helper.poll() is None:
                subprocess.run(['sudo', '-n', 'kill', str(helper.pid)],
                               check=False)
                helper.communicate(timeout=10)

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR',
                      'QEMU_RPI_RAW_GADGET')
    def test_arm_cm4_rpiboot_usb_reset_reenumeration(self):
        """Reset active ROM/file transfers and re-enumerate to completion."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        raw_gadget = Path(
            os.environ['QEMU_RPI_RAW_GADGET']).resolve(strict=True)
        for executable in (rpiboot, raw_gadget):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertTrue(artifact.is_file(),
                            f'missing official RPIBOOT file: {artifact}')
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        if not shutil.which('usbreset'):
            self.skipTest('usbreset is required for the RPIBOOT reset gate')
        sudo_check = subprocess.run(
            ['sudo', '-n', 'true'], capture_output=True, text=True)
        if sudo_check.returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')

        emmc_path = Path(self.workdir) / 'cm4-rpiboot-reset-emmc.img'
        eeprom_path = Path(self.workdir) / 'cm4-rpiboot-reset-eeprom.bin'
        lifecycle = Path(self.workdir) / 'cm4-rpiboot-reset.state'
        self._prepare_cm4_production_media(emmc_path, eeprom_path)

        self.set_machine('raspi-cm4')
        wait_vm = self.get_vm(name='cm4-rpiboot-reset-wait')
        wait_vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
            'eeprom-drive=pieeprom,emmc-drive=emmc,'
            f'provision-state-file={lifecycle}')
        wait_vm.add_args(
            '-drive', f'if=none,id=pieeprom,format=raw,file={eeprom_path}',
            '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}')
        wait_vm.launch()
        self.assertEqual(
            wait_vm.cmd('qom-get', path='/machine', property='boot-state'),
            'rpiboot-wait')
        wait_vm.shutdown()
        self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                         'rpiboot-host-ready')

        self._require_raw_gadget_modules()
        self._exercise_rpiboot_usb_resets(
            raw_gadget, rpiboot, boot_dir, lifecycle)

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR',
                      'QEMU_RPI_RAW_GADGET')
    def test_arm_cm4_rpiboot_control_timeouts(self):
        """Reach official host timeouts in both RPIBOOT enumerations."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        raw_gadget = Path(
            os.environ['QEMU_RPI_RAW_GADGET']).resolve(strict=True)
        for executable in (rpiboot, raw_gadget):
            self.assertTrue(os.access(executable, os.X_OK),
                            f'fixture is not executable: {executable}')
        for name, expected in self.RPIBOOT_FILE_SHA256.items():
            artifact = boot_dir / name
            self.assertTrue(artifact.is_file(),
                            f'missing official RPIBOOT file: {artifact}')
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(),
                             expected, f'unpinned RPIBOOT input: {name}')
        sudo_check = subprocess.run(
            ['sudo', '-n', 'true'], capture_output=True, text=True)
        if sudo_check.returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')

        emmc_path = Path(self.workdir) / 'cm4-rpiboot-timeout-emmc.img'
        eeprom_path = Path(self.workdir) / 'cm4-rpiboot-timeout-eeprom.bin'
        lifecycle = Path(self.workdir) / 'cm4-rpiboot-timeout.state'
        self._prepare_cm4_production_media(emmc_path, eeprom_path)

        self.set_machine('raspi-cm4')
        wait_vm = self.get_vm(name='cm4-rpiboot-timeout-wait')
        wait_vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
            'eeprom-drive=pieeprom,emmc-drive=emmc,'
            f'provision-state-file={lifecycle}')
        wait_vm.add_args(
            '-drive', f'if=none,id=pieeprom,format=raw,file={eeprom_path}',
            '-drive', f'if=none,id=emmc,format=raw,file={emmc_path}')
        wait_vm.launch()
        self.assertEqual(
            wait_vm.cmd('qom-get', path='/machine', property='boot-state'),
            'rpiboot-wait')
        wait_vm.shutdown()
        self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                         'rpiboot-host-ready')

        self._require_raw_gadget_modules()
        self._exercise_rpiboot_control_timeout(
            raw_gadget, rpiboot, boot_dir, lifecycle, 'rom-status-timeout')
        self._exercise_rpiboot_control_timeout(
            raw_gadget, rpiboot, boot_dir, lifecycle,
            'file-request-timeout', 'boot.img')

        capture = Path(self.workdir) / 'rpiboot-timeout-clean-retry'
        capture.mkdir()
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_gadget),
             '--capture-dir', str(capture), '--lifecycle', str(lifecycle),
             '--mass-storage'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(0.5)
            self.assertIsNone(helper.poll(), 'clean Raw Gadget retry exited')
            host = subprocess.run(
                ['sudo', '-n', str(rpiboot), '-d', str(boot_dir)],
                capture_output=True, text=True, timeout=90)
            helper_output = helper.communicate(timeout=30)[0]
            self.assertEqual(
                host.returncode, 0,
                f'clean rpiboot retry failed:\n{host.stdout}{host.stderr}')
            self.assertEqual(helper.returncode, 0, helper_output)
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'rpiboot-complete')
            for name in self.RPIBOOT_FILE_SHA256:
                self.assertEqual((capture / name).read_bytes(),
                                 (boot_dir / name).read_bytes())
        finally:
            if helper.poll() is None:
                subprocess.run(['sudo', '-n', 'kill', str(helper.pid)],
                               check=False)
                helper.communicate(timeout=10)

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RAW_MSD')
    def test_arm_cm4_raw_msd_official_composite(self):
        """Enumerate the pinned second-stage ACM+MSD identity and both paths."""
        raw_msd = Path(
            os.environ['QEMU_RPI_RAW_MSD']).resolve(strict=True)
        self._require_raw_gadget_modules()
        backend = Path(self.workdir) / 'cm4-official-composite.img'
        with backend.open('wb') as target:
            target.truncate(64 * 1024 * 1024)
        stable = Path(
            '/dev/disk/by-id/'
            'usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_'
            '51554d5552504934-0:0')
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_msd), '--image', str(backend)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        output = ''
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                line = helper.stdout.readline()
                if not line:
                    break
                output += line
                if 'CM4 raw BOT target ready' in line:
                    break
            self.assertIn('CM4 raw BOT target ready', output)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not stable.exists():
                time.sleep(0.05)
            self.assertTrue(stable.exists(),
                            'official composite storage did not enumerate')
            self._assert_raw_msd_official_composite(stable)
            readback = subprocess.run(
                ['sudo', '-n', 'dd', f'if={stable}', 'of=/dev/null',
                 'bs=4096', 'count=1', 'status=none'],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(readback.returncode, 0, readback.stderr)
        finally:
            if helper.poll() is None:
                subprocess.run(
                    ['sudo', '-n', 'kill', '-TERM', '--', f'-{helper.pid}'],
                    check=False, capture_output=True, text=True)
            output += helper.communicate(timeout=10)[0]
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and stable.exists():
                time.sleep(0.05)
            self.assertFalse(stable.exists(),
                             'official composite remained after owner exit')

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR',
                      'QEMU_RPI_RAW_GADGET')
    def test_arm_cm4_unmodified_rpiboot_usb_flash_boot(self):
        """Run nRPIBOOT, official rpiboot, USB flash, and exact-media boot."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        raw_gadget = Path(
            os.environ['QEMU_RPI_RAW_GADGET']).resolve(strict=True)
        repo_root = Path(__file__).resolve().parents[3]
        mass_storage = repo_root / 'contrib/raspi4/cm4_mass_storage.py'
        raw_msd_value = os.environ.get('QEMU_RPI_RAW_MSD')
        raw_msd = (Path(raw_msd_value).resolve(strict=True)
                   if raw_msd_value else
                   Path(self.qemu_bin).resolve().with_name(
                       'qemu-rpi-cm4-msd'))
        bot_probe_value = os.environ.get('QEMU_RPI_BOT_PROBE')
        bot_probe = (Path(bot_probe_value).resolve(strict=True)
                     if bot_probe_value else
                     raw_msd.with_name('qemu-rpi-cm4-bot-probe'))
        imager_value = os.environ.get('QEMU_RPI_IMAGER')
        imager = (Path(imager_value).resolve(strict=True)
                  if imager_value else None)
        release_value = os.environ.get('QEMU_RPI_RELEASE_IMAGE')
        release_image = (Path(release_value).resolve(strict=True)
                         if release_value else None)
        required_files = ('bootcode4.bin', 'config.txt', 'boot.img')

        if not os.access(rpiboot, os.X_OK):
            self.fail(f'rpiboot is not executable: {rpiboot}')
        if not os.access(raw_gadget, os.X_OK):
            self.fail(f'Raw Gadget bridge is not executable: {raw_gadget}')
        if not raw_msd.is_file() or not os.access(raw_msd, os.X_OK):
            self.fail(
                f'Raw BOT mass-storage bridge is not executable: {raw_msd}')
        if not bot_probe.is_file() or not os.access(bot_probe, os.X_OK):
            self.fail(f'Raw BOT host probe is not executable: {bot_probe}')
        if imager:
            if not os.access(imager, os.X_OK):
                self.fail(f'Raspberry Pi Imager is not executable: {imager}')
            digest = hashlib.sha256(imager.read_bytes()).hexdigest()
            self.assertEqual(digest, self.RPI_IMAGER_SHA256,
                             'Raspberry Pi Imager release hash differs')
        if release_image:
            if not imager:
                self.fail('QEMU_RPI_RELEASE_IMAGE requires QEMU_RPI_IMAGER')
            compressed_digest = self._sha256_prefix(
                release_image, release_image.stat().st_size)
            self.assertEqual(compressed_digest, self.RPIOS_LITE_XZ_SHA256,
                             'Raspberry Pi OS release archive differs')
            raw_size, raw_digest = self._xz_payload_identity(release_image)
            self.assertEqual(raw_size, self.RPIOS_LITE_RAW_SIZE)
            self.assertEqual(raw_digest, self.RPIOS_LITE_RAW_SHA256,
                             'Raspberry Pi OS release payload differs')
        for name in required_files:
            if not (boot_dir / name).is_file():
                self.fail(f'missing official RPIBOOT file: {boot_dir / name}')
            digest = hashlib.sha256((boot_dir / name).read_bytes()).hexdigest()
            self.assertEqual(digest, self.RPIBOOT_FILE_SHA256[name],
                             f'unpinned RPIBOOT input: {name}')
        sudo_check = subprocess.run(
            ['sudo', '-n', 'true'], capture_output=True, text=True)
        if sudo_check.returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')

        source_emmc = Path(self.workdir) / 'cm4-usb-source.img'
        target_emmc = Path(self.workdir) / 'cm4-usb-target.img'
        eeprom_path = Path(self.workdir) / 'cm4-usb-pieeprom.bin'
        capture_dir = Path(self.workdir) / 'rpiboot-capture'
        lifecycle = Path(self.workdir) / 'cm4-provision.state'
        command_line = self._prepare_cm4_production_media(
            source_emmc, eeprom_path)
        image_source = release_image or source_emmc
        target_size = (self.CM4_RELEASE_EMMC_SIZE if release_image else
                       source_emmc.stat().st_size)
        with target_emmc.open('wb') as target:
            target.truncate(target_size)

        # Model the powered CM4 with GPIO40/nRPIBOOT asserted.  Power the VM
        # off before the host fixture takes exclusive ownership of eMMC.
        self.set_machine('raspi-cm4')
        wait_vm = self.get_vm(name='cm4-rpiboot-wait')
        wait_vm.set_machine(
            'raspi-cm4,boot-mode=behavioral,nrpiboot=on,'
            'eeprom-drive=pieeprom,emmc-drive=emmc,'
            f'provision-state-file={lifecycle}')
        wait_vm.add_args(
            '-drive', f'if=none,id=pieeprom,format=raw,file={eeprom_path}',
            '-drive', f'if=none,id=emmc,format=raw,file={target_emmc}')
        wait_vm.launch()
        self.assertEqual(
            wait_vm.cmd('qom-get', path='/machine', property='boot-state'),
            'rpiboot-wait')
        self.assertEqual(
            wait_vm.cmd('qom-get', path='/machine', property='boot-source'),
            'rpiboot')
        self.assertEqual(
            wait_vm.cmd('qom-get', path='/machine',
                        property='provision-state'),
            'qemu-rpiboot-wait')
        wait_vm.shutdown()
        self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                         'rpiboot-host-ready')

        self._require_raw_gadget_modules()

        # Cut the unchanged official rpiboot transfer at an exact byte
        # boundary.  Closing Raw Gadget is the modeled cable/power loss; a
        # clean retry below must start from the ROM enumeration again.
        fault_capture = Path(self.workdir) / 'rpiboot-fault-capture'
        fault_capture.mkdir()
        fault_helper = subprocess.Popen(
            ['sudo', '-n', str(raw_gadget),
             '--capture-dir', str(fault_capture),
             '--lifecycle', str(lifecycle), '--mass-storage',
             '--fault', 'bootcode-disconnect', '--fault-after', '4096'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(0.5)
            fault_rpiboot = subprocess.run(
                ['sudo', '-n', 'timeout', '15s', str(rpiboot),
                 '-d', str(boot_dir)],
                capture_output=True, text=True, timeout=20)
            fault_output = fault_helper.communicate(timeout=10)[0]
            self.assertEqual(
                fault_helper.returncode, 75,
                'Raw Gadget did not report its injected fault:\n'
                f'{fault_output}')
            self.assertNotEqual(
                fault_rpiboot.returncode, 0,
                'rpiboot unexpectedly accepted a truncated bootcode transfer')
            self.assertIn('injected disconnect', fault_output)
            self.assertEqual(
                (fault_capture / 'bootcode4.bin').stat().st_size, 4096)
        finally:
            if fault_helper.poll() is None:
                subprocess.run(['sudo', '-n', 'kill', str(fault_helper.pid)],
                               check=False)
                fault_helper.wait(timeout=10)

        capture_dir.mkdir()
        helper = subprocess.Popen(
            ['sudo', '-n', str(raw_gadget),
             '--capture-dir', str(capture_dir),
             '--lifecycle', str(lifecycle), '--mass-storage'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(0.5)
            if helper.poll() is not None:
                output = helper.communicate()[0]
                self.fail(f'Raw Gadget bridge exited before rpiboot:\n{output}')
            rpiboot_run = subprocess.run(
                ['sudo', '-n', str(rpiboot), '-d', str(boot_dir)],
                capture_output=True, text=True, timeout=90)
            helper_output = helper.communicate(timeout=30)[0]
            if rpiboot_run.returncode or helper.returncode:
                self.fail(
                    f'rpiboot rc={rpiboot_run.returncode}:\n'
                    f'{rpiboot_run.stdout}\n{rpiboot_run.stderr}\n'
                    f'Raw Gadget rc={helper.returncode}:\n{helper_output}')
            self.assertIn(self.USBBOOT_GIT, rpiboot_run.stdout,
                          'rpiboot binary is not the pinned official build')
        finally:
            if helper.poll() is None:
                subprocess.run(['sudo', '-n', 'kill', str(helper.pid)],
                               check=False)
                helper.wait(timeout=10)

        for name in required_files:
            served = hashlib.sha256((boot_dir / name).read_bytes()).digest()
            received = hashlib.sha256(
                (capture_dir / name).read_bytes()).digest()
            self.assertEqual(received, served, f'RPIBOOT changed {name}')

        gadget_active = False
        gadget_owner = None
        stable_target = Path(
            '/dev/disk/by-id/usb-Linux_File-Stor_Gadget_'
            '51554d5552504934-0:0')

        self._exercise_raw_msd_transport_recovery(
            raw_msd, 'bot-phase', class_reset_count=8, reset_storm=True)
        self._exercise_raw_msd_transport_recovery(raw_msd, 'bot-timeout')
        self._exercise_raw_msd_data_reset(raw_msd)
        self._exercise_raw_msd_data_reset(raw_msd, write=True)
        self._exercise_raw_msd_malformed_cbw(raw_msd, bot_probe)
        self._exercise_raw_msd_scsi_fault(
            raw_msd, mass_storage, lifecycle, 'synchronize-cache',
            ['35', '00', '00', '00', '00', '00', '00', '00', '00', '00'])
        self._exercise_raw_msd_scsi_fault(
            raw_msd, mass_storage, lifecycle, 'write-fua',
            ['2a', '08', '00', '00', '00', '10', '00', '00', '01', '00'],
            data_out=True)
        if imager:
            self._exercise_raw_msd_imager_flush_fault(
                raw_msd, imager, lifecycle)

        if release_image and imager:
            undersized_emmc = Path(self.workdir) / 'cm4-usb-full-fault.img'
            with undersized_emmc.open('wb') as target:
                target.truncate(64 * 1024 * 1024)
            fault_gadget_active = False
            try:
                subprocess.run(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'start', str(undersized_emmc)],
                    check=True, capture_output=True, text=True, timeout=30)
                fault_gadget_active = True
                full_run = subprocess.run(
                    ['sudo', '-n', str(imager), '--disable-eject',
                     '--sha256', self.RPIOS_LITE_RAW_SHA256,
                     str(release_image),
                     str(stable_target.resolve(strict=True))],
                    capture_output=True, text=True, timeout=60)
                self.assertNotEqual(
                    full_run.returncode, 0,
                    'Imager unexpectedly accepted an undersized virtual eMMC')
            finally:
                if fault_gadget_active:
                    subprocess.run(
                        ['sudo', '-n', sys.executable, str(mass_storage),
                         '--lifecycle', str(lifecycle),
                         'stop', '--result', 'failed'],
                        check=True, capture_output=True, text=True, timeout=30)

            eject_emmc = Path(self.workdir) / 'cm4-usb-scsi-fault.img'
            with eject_emmc.open('wb') as target:
                target.truncate(self.CM4_RELEASE_EMMC_SIZE)
            eject_gadget_active = False
            eject_run = None
            eject_watch = None
            try:
                subprocess.run(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'start', str(eject_emmc)],
                    check=True, capture_output=True, text=True, timeout=30)
                eject_gadget_active = True
                eject_watch = subprocess.Popen(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'fault-eject', '--after-sectors', '8192',
                     '--timeout', '30'],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, start_new_session=True)
                eject_ready = eject_watch.stdout.readline()
                self.assertIn('Watching CM4 eMMC host writes', eject_ready)
                eject_run = subprocess.Popen(
                    ['sudo', '-n', 'timeout', '90s', str(imager),
                     '--disable-eject', '--sha256',
                     self.RPIOS_LITE_RAW_SHA256, str(release_image),
                     str(stable_target.resolve(strict=True))],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, start_new_session=True)
                eject_output = eject_watch.communicate(timeout=35)[0]
                self.assertEqual(
                    eject_watch.returncode, 0,
                    'SCSI fault watcher failed:\n'
                    f'{eject_ready}{eject_output}')
                imager_eject_output = eject_run.communicate(timeout=30)[0]
                self.assertNotEqual(
                    eject_run.returncode, 0,
                    'Imager unexpectedly accepted SCSI medium removal:\n'
                    f'{imager_eject_output}')
                self.assertIn('Injected CM4 eMMC SCSI medium removal',
                              eject_output)
                self.assertTrue(stable_target.exists(),
                                'SCSI fault disconnected the USB device')
                scsi_read = subprocess.run(
                    ['sudo', '-n', 'dd', f'if={stable_target}',
                     'of=/dev/null', 'bs=512', 'count=1', 'status=none'],
                    capture_output=True, text=True, timeout=30)
                self.assertNotEqual(
                    scsi_read.returncode, 0,
                    'force-ejected SCSI medium remained readable')
                self.assertGreater(eject_emmc.stat().st_blocks, 0)
                subprocess.run(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'stop', '--result', 'failed'],
                    check=True, capture_output=True, text=True, timeout=30)
                eject_gadget_active = False
            finally:
                if eject_gadget_active:
                    subprocess.run(
                        ['sudo', '-n', sys.executable, str(mass_storage),
                         '--lifecycle', str(lifecycle),
                         'stop', '--result', 'failed'],
                        check=False, capture_output=True, text=True, timeout=30)
                if eject_run and eject_run.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-TERM', '--',
                         f'-{eject_run.pid}'],
                        check=False, capture_output=True, text=True)
                    eject_run.wait(timeout=10)
                if eject_watch and eject_watch.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-TERM', '--',
                         f'-{eject_watch.pid}'],
                        check=False, capture_output=True, text=True)
                    eject_watch.wait(timeout=10)

            flush_emmc = Path(self.workdir) / 'cm4-usb-flush-fault.img'
            with flush_emmc.open('wb') as target:
                target.truncate(64 * 1024 * 1024)
            flush_gadget_active = False
            flush_watch = None
            try:
                subprocess.run(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'start', str(flush_emmc)],
                    check=True, capture_output=True, text=True, timeout=30)
                flush_gadget_active = True
                flush_watch = subprocess.Popen(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'fault-flush-eject', '--after-flushes', '1',
                     '--timeout', '30'],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, start_new_session=True)
                flush_ready = flush_watch.stdout.readline()
                self.assertIn('Watching CM4 eMMC host flushes', flush_ready)
                subprocess.run(
                    ['sudo', '-n', 'dd', 'if=/dev/zero',
                     f'of={stable_target.resolve(strict=True)}', 'bs=1M',
                     'count=8', 'conv=fsync', 'status=none'],
                    check=True, capture_output=True, text=True, timeout=30)
                flush_output = flush_watch.communicate(timeout=30)[0]
                self.assertEqual(
                    flush_watch.returncode, 0,
                    'Flush-boundary fault watcher failed:\n'
                    f'{flush_ready}{flush_output}')
                self.assertIn(
                    'Injected CM4 eMMC SCSI medium removal after host flush',
                    flush_output)
                self.assertTrue(stable_target.exists(),
                                'flush-boundary fault disconnected USB')
                flush_read = subprocess.run(
                    ['sudo', '-n', 'dd', f'if={stable_target}',
                     'of=/dev/null', 'bs=512', 'count=1', 'status=none'],
                    capture_output=True, text=True, timeout=30)
                self.assertNotEqual(
                    flush_read.returncode, 0,
                    'post-flush forced eject remained readable')
                subprocess.run(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'stop', '--result', 'failed'],
                    check=True, capture_output=True, text=True, timeout=30)
                flush_gadget_active = False
            finally:
                if flush_gadget_active:
                    subprocess.run(
                        ['sudo', '-n', sys.executable, str(mass_storage),
                         '--lifecycle', str(lifecycle),
                         'stop', '--result', 'failed'],
                        check=False, capture_output=True, text=True, timeout=30)
                if flush_watch and flush_watch.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-TERM', '--',
                         f'-{flush_watch.pid}'],
                        check=False, capture_output=True, text=True)
                    flush_watch.wait(timeout=10)

            disconnect_emmc = (Path(self.workdir) /
                               'cm4-usb-disconnect-fault.img')
            with disconnect_emmc.open('wb') as target:
                target.truncate(self.CM4_RELEASE_EMMC_SIZE)
            disconnect_gadget_active = False
            disconnect_run = None
            disconnect_watch = None
            try:
                subprocess.run(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'start', str(disconnect_emmc)],
                    check=True, capture_output=True, text=True, timeout=30)
                disconnect_gadget_active = True
                disconnect_watch = subprocess.Popen(
                    ['sudo', '-n', sys.executable, str(mass_storage),
                     '--lifecycle', str(lifecycle),
                     'fault-disconnect', '--after-sectors', '8192',
                     '--timeout', '30'],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, start_new_session=True)
                watch_ready = disconnect_watch.stdout.readline()
                self.assertIn('Watching CM4 eMMC host writes', watch_ready)
                disconnect_run = subprocess.Popen(
                    ['sudo', '-n', 'timeout', '90s', str(imager),
                     '--disable-eject', '--sha256',
                     self.RPIOS_LITE_RAW_SHA256, str(release_image),
                     str(stable_target.resolve(strict=True))],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, start_new_session=True)
                watch_output = disconnect_watch.communicate(timeout=35)[0]
                self.assertEqual(
                    disconnect_watch.returncode, 0,
                    'Mass-storage fault watcher failed:\n'
                    f'{watch_ready}{watch_output}')
                disconnect_gadget_active = False
                disconnect_output = disconnect_run.communicate(timeout=30)[0]
                self.assertNotEqual(
                    disconnect_run.returncode, 0,
                    'Imager unexpectedly accepted a mid-write USB disconnect:\n'
                    f'{disconnect_output}')
                self.assertIn('Injected CM4 eMMC USB disconnect',
                              watch_output)
                self.assertGreater(disconnect_emmc.stat().st_blocks, 0)
            finally:
                if disconnect_gadget_active:
                    subprocess.run(
                        ['sudo', '-n', sys.executable, str(mass_storage),
                         '--lifecycle', str(lifecycle),
                         'stop', '--result', 'failed'],
                        check=False, capture_output=True, text=True, timeout=30)
                if disconnect_run and disconnect_run.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-TERM', '--',
                         f'-{disconnect_run.pid}'],
                        check=False, capture_output=True, text=True)
                    disconnect_run.wait(timeout=10)
                if disconnect_watch and disconnect_watch.poll() is None:
                    subprocess.run(
                        ['sudo', '-n', 'kill', '-TERM', '--',
                         f'-{disconnect_watch.pid}'],
                        check=False, capture_output=True, text=True)
                    disconnect_watch.wait(timeout=10)

        try:
            gadget_owner = subprocess.Popen(
                ['sudo', '-n', sys.executable, str(mass_storage),
                 '--lifecycle', str(lifecycle), 'serve', str(target_emmc)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            ready_output = ''
            for _ in range(4):
                ready_output += gadget_owner.stdout.readline()
            self.assertIn('CM4 mass-storage owner ready', ready_output)
            if gadget_owner.poll() is not None:
                self.fail(
                    'CM4 mass-storage owner exited before imaging:\n'
                    f'{ready_output}{gadget_owner.communicate()[0]}')
            gadget_active = True
            self.assertTrue(stable_target.exists())

            # The foreground owner must retain the sibling advisory lock for
            # the whole Imager session, not merely during configfs setup.
            contender = subprocess.run(
                ['sudo', '-n', sys.executable, str(mass_storage),
                 '--lifecycle', str(lifecycle), 'status'],
                capture_output=True, text=True, timeout=30)
            self.assertNotEqual(contender.returncode, 0)
            self.assertIn('owned by another process', contender.stderr)

            imager_target = stable_target.resolve(strict=True)
            if imager:
                imager_version = subprocess.run(
                    [str(imager), '--version'], check=True,
                    capture_output=True, text=True, timeout=30)
                self.assertIn(self.RPI_IMAGER_VERSION,
                              imager_version.stdout)
                if release_image:
                    source_digest = self.RPIOS_LITE_RAW_SHA256
                else:
                    source_digest = hashlib.sha256(
                        image_source.read_bytes()).hexdigest()
                subprocess.run(
                    ['sudo', '-n', str(imager),
                     '--disable-eject', '--sha256', source_digest,
                     str(image_source), str(imager_target)],
                    check=True, timeout=180)
            else:
                subprocess.run(
                    ['sudo', '-n', 'dd', f'if={image_source}',
                     f'of={stable_target}', 'bs=4M', 'conv=fsync',
                     'status=none'], check=True, timeout=90)
            subprocess.run(['sudo', '-n', 'blockdev', '--flushbufs',
                            str(stable_target)], check=True, timeout=30)

            gadget_owner.stdin.write('complete\n')
            gadget_owner.stdin.flush()
            owner_output = gadget_owner.communicate(timeout=30)[0]
            self.assertEqual(
                gadget_owner.returncode, 0,
                'CM4 mass-storage owner failed to release eMMC:\n'
                f'{ready_output}{owner_output}')
            gadget_active = False
        finally:
            if gadget_active and gadget_owner and gadget_owner.poll() is None:
                try:
                    gadget_owner.stdin.write('failed\n')
                    gadget_owner.stdin.flush()
                    gadget_owner.communicate(timeout=30)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    subprocess.run(
                        ['sudo', '-n', 'kill', str(gadget_owner.pid)],
                        check=False, capture_output=True, text=True)
                    gadget_owner.wait(timeout=10)

        if release_image:
            target_hash = self._sha256_prefix(target_emmc,
                                              self.RPIOS_LITE_RAW_SIZE)
            self.assertEqual(target_hash, self.RPIOS_LITE_RAW_SHA256,
                             'Imager changed the release payload')
            disk_id, partition2_lba, partition2_sectors = self._mbr_state(
                target_emmc)
            self.assertEqual(disk_id, self.RPIOS_LITE_DISK_ID)
            self.assertEqual(partition2_lba, 1_064_960)
            self.assertEqual(partition2_sectors, 4_751_360)
        else:
            source_hash = hashlib.sha256(source_emmc.read_bytes()).digest()
            target_hash = hashlib.sha256(target_emmc.read_bytes()).digest()
            self.assertEqual(target_hash, source_hash,
                             'USB-flashed eMMC differs from source image')

        # Corrupt only the lifecycle token to prove QEMU will not open
        # correctly flashed bytes while the host still owns or failed them.
        lifecycle.write_text('flash-failed\n', encoding='ascii')
        try:
            rejected_boot = subprocess.run(
                [self.qemu_bin,
                 '-M', ('raspi-cm4,boot-mode=behavioral,'
                        'eeprom-drive=pieeprom,emmc-drive=emmc,'
                        f'provision-state-file={lifecycle}'),
                 '-drive', ('if=none,id=pieeprom,format=raw,'
                            f'file={eeprom_path}'),
                 '-drive', f'if=none,id=emmc,format=raw,file={target_emmc}',
                 '-display', 'none'],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(rejected_boot.returncode, 0)
            self.assertIn('ownership state must be boot-ready',
                          rejected_boot.stderr)
        finally:
            lifecycle.write_text('boot-ready\n', encoding='ascii')

        # GPIO40 is deasserted by the modeled power cycle.  Boot the exact
        # bytes just written through the host-visible USB block device.
        boot_vm = self.get_vm(name='cm4-post-usb-boot')
        self._configure_cm4_boot_vm(boot_vm, target_emmc, eeprom_path,
                                   lifecycle, no_reboot=not release_image,
                                   console_index=1 if release_image else 0)
        boot_vm.launch()
        if release_image:
            self.assertEqual(
                boot_vm.cmd('qom-get', path='/machine',
                            property='boot-state'),
                'arm-handoff-ready')
            self.assertEqual(
                boot_vm.cmd('qom-get', path='/machine',
                            property='boot-source'), 'emmc')
            self.assertEqual(
                boot_vm.cmd('qom-get', path='/machine',
                            property='firmware-overlay-file'),
                'overlays/vc4-kms-v3d-pi4.dtbo')
            self.assertEqual(
                boot_vm.cmd('qom-get', path='/machine',
                            property='provision-state'),
                'qemu-owned')
            expected_sectors = (self.CM4_RELEASE_EMMC_SIZE // 512 -
                                1_064_960)
            ready = False
            observed = None
            console_tail = bytearray()
            boot_vm.console_socket.setblocking(False)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                while True:
                    try:
                        console_data = boot_vm.console_socket.recv(65536)
                    except BlockingIOError:
                        break
                    if not console_data:
                        break
                    console_tail.extend(console_data)
                    if len(console_tail) > 256 * 1024:
                        del console_tail[:-128 * 1024]
                try:
                    disk_id, partition2_lba, partition2_sectors = \
                        self._mbr_state(target_emmc)
                    blocks = self._ext4_block_count(target_emmc,
                                                    partition2_lba)
                    observed = (disk_id, partition2_lba,
                                partition2_sectors, blocks)
                    media_ready = (
                        disk_id != self.RPIOS_LITE_DISK_ID and
                        partition2_lba == 1_064_960 and
                        partition2_sectors == expected_sectors and
                        blocks == expected_sectors * 512 // 4096
                    )
                    login_ready = b'raspberrypi login:' in console_tail
                    ready = media_ready and login_ready
                except (AssertionError, OSError, struct.error):
                    pass
                if ready or not boot_vm.is_running():
                    break
                time.sleep(0.05)
            self.assertTrue(
                ready,
                'first-boot media/login transition did not complete: '
                f'{observed}\nconsole tail:\n'
                f'{console_tail[-4096:].decode(errors="replace")}')
            self.assertNotIn(
                b'raspberrypi-exp-gpio soc:firmware:gpio: Failed',
                console_tail)
            try:
                if boot_vm.is_running():
                    boot_vm.shutdown()
                else:
                    boot_vm.wait(timeout=1)
            except AbnormalShutdown:
                # First-boot initramfs resets after resizing.  With the
                # test's -no-reboot option, that can race the QMP quit.
                if boot_vm.exitcode() != 0:
                    raise
            self.assertEqual(lifecycle.read_text(encoding='ascii').strip(),
                             'qemu-stopped')

            disk_id, partition2_lba, partition2_sectors = self._mbr_state(
                target_emmc)
            self.assertNotEqual(disk_id, self.RPIOS_LITE_DISK_ID)
            self.assertEqual(partition2_lba, 1_064_960)
            self.assertEqual(partition2_sectors, expected_sectors)
            self.assertEqual(
                self._ext4_block_count(target_emmc, partition2_lba),
                partition2_sectors * 512 // 4096)
        else:
            self._assert_cm4_production_boot(boot_vm, command_line)

    @skipUnlessOperatingSystem('Linux')
    @skipIfMissingEnv('QEMU_RPI_RPIBOOT', 'QEMU_RPI_USBBOOT_DIR',
                      'QEMU_RPI_RAW_GADGET', 'QEMU_RPI_IMAGER',
                      'QEMU_RPI_RELEASE_IMAGE')
    def test_arm_cm4_supervised_provision(self):
        """Run the clean exact CM4 path through the single supervisor."""
        rpiboot = Path(os.environ['QEMU_RPI_RPIBOOT']).resolve(strict=True)
        boot_dir = Path(os.environ['QEMU_RPI_USBBOOT_DIR']).resolve(strict=True)
        raw_gadget = Path(
            os.environ['QEMU_RPI_RAW_GADGET']).resolve(strict=True)
        imager = Path(os.environ['QEMU_RPI_IMAGER']).resolve(strict=True)
        release_image = Path(
            os.environ['QEMU_RPI_RELEASE_IMAGE']).resolve(strict=True)
        repo_root = Path(__file__).resolve().parents[3]
        supervisor = repo_root / 'contrib/raspi4/cm4_provision.py'
        eeprom = Path(self.ASSET_PIEEPROM.fetch()).resolve(strict=True)
        workdir = Path(self.workdir) / 'supervised-provision'
        emmc = workdir / 'cm4-emmc.img'
        manifest_path = Path(self.workdir) / 'cm4-provision-manifest.json'
        manifest = {
            'schema': 'qemu-rpi-cm4-provision-v1',
            'artifacts': {
                'eeprom': {
                    'sha256': self.ASSET_PIEEPROM.hash,
                    'bootsys_sha256': self.PIEEPROM_BOOTSYS_SHA256,
                    'bootsys_key_index': self.PIEEPROM_BOOTSYS_KEY_INDEX,
                    'dependencies_sha256':
                        self.PIEEPROM_DEPENDENCIES_SHA256,
                },
                'rpiboot': {'sha256': self.RPIBOOT_SHA256},
                'bootcode4.bin': {
                    'sha256': self.RPIBOOT_FILE_SHA256['bootcode4.bin']},
                'config.txt': {
                    'sha256': self.RPIBOOT_FILE_SHA256['config.txt']},
                'boot.img': {
                    'sha256': self.RPIBOOT_FILE_SHA256['boot.img']},
                'imager': {'sha256': self.RPI_IMAGER_SHA256},
                'image': {
                    'sha256': self.RPIOS_LITE_XZ_SHA256,
                    'payload_sha256': self.RPIOS_LITE_RAW_SHA256,
                },
            },
        }
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8')

        sudo_check = subprocess.run(
            ['sudo', '-n', 'true'], capture_output=True, text=True)
        if sudo_check.returncode:
            self.skipTest('passwordless sudo is required for the USB fixture')

        run = subprocess.run(
            [sys.executable, str(supervisor),
             '--qemu', str(self.qemu_bin),
             '--eeprom', str(eeprom),
             '--emmc', str(emmc), '--emmc-size', '4GiB', '--ram', '2G',
             '--raw-gadget', str(raw_gadget),
             '--mass-storage-mode', 'raw-bot',
             '--raw-msd', str(
                 Path(self.qemu_bin).parent / 'qemu-rpi-cm4-msd'),
             '--rpiboot', str(rpiboot), '--boot-dir', str(boot_dir),
             '--imager', str(imager), '--image', str(release_image),
             '--manifest', str(manifest_path), '--workdir', str(workdir),
             '--handoff-only'],
            capture_output=True, text=True, timeout=360)
        self.assertEqual(
            run.returncode, 0,
            f'supervisor failed:\n{run.stdout}\n{run.stderr}')

        report = json.loads(
            (workdir / 'cm4-provision-report.json').read_text(
                encoding='utf-8'))
        self.assertEqual(report['status'], 'complete')
        self.assertEqual(report['mass_storage_mode'], 'raw-bot')
        self.assertEqual(report['events'], [
            'qemu-rpiboot-released',
            'rpiboot-complete',
            'image-written-verified-flushed',
            'post-flash-arm-handoff',
            'post-flash-qemu-stopped',
        ])
        self.assertEqual(report['artifacts']['image'],
                         self.RPIOS_LITE_XZ_SHA256)
        self.assertEqual(report['artifacts']['bootsys'],
                         self.PIEEPROM_BOOTSYS_SHA256)
        self.assertEqual(report['artifacts']['bootsys-dependencies'],
                         self.PIEEPROM_DEPENDENCIES_SHA256)
        self.assertEqual(
            report['artifacts']['bootsys-key-index'],
            str(self.PIEEPROM_BOOTSYS_KEY_INDEX))
        self.assertEqual(
            report['artifacts']['bootsys-dependency-count'],
            str(self.PIEEPROM_DEPENDENCY_COUNT))
        self.assertEqual(
            self._sha256_prefix(emmc, self.RPIOS_LITE_RAW_SIZE),
            self.RPIOS_LITE_RAW_SHA256)
        self.assertEqual(
            (workdir / 'cm4-provision.state').read_text(
                encoding='ascii').strip(),
            'qemu-stopped')


if __name__ == '__main__':
    LinuxKernelTest.main()
