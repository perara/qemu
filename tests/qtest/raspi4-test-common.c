/*
 * Raspberry Pi 4 qtest shared helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "raspi4-boot-test.h"

static const uint8_t test_dependency_hash[TEST_DEPENDENCY_HASH_SIZE] = {
    0xe4, 0xbd, 0xf4, 0xe3, 0xda, 0xd6, 0xcd, 0x1c,
    0x1d, 0xbd, 0x79, 0x50, 0xef, 0x75, 0x30, 0x14,
    0x4a, 0xbc, 0x94, 0x25, 0x2a, 0x66, 0xcc, 0xf3,
    0x9c, 0xe5, 0x6b, 0xeb, 0x2c, 0xde, 0xae, 0xc1,
};
static const uint8_t test_dependency_contents[] =
    "abcdefghabcdefghTAIL!";
static const uint8_t test_dependency_second_block[] = {
    0x04, 0x08, 0x00, 0x50, 'T', 'A', 'I', 'L', '!',
};

static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned int bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
        }
    }

    return ~crc;
}
char *qom_get_string(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': '/machine', 'property': %s } }", property);
    char *result = g_strdup(qdict_get_str(response, "return"));

    qobject_unref(response);
    return result;
}
uint32_t qom_get_uint32(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': '/machine', 'property': %s } }", property);
    uint32_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
uint32_t qom_path_get_uint32(QTestState *qts, const char *path,
                                    const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': %s, 'property': %s } }", path, property);
    uint32_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
uint64_t qom_get_uint64(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': '/machine', 'property': %s } }", property);
    uint64_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
int32_t qom_get_int32(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': '/machine', 'property': %s } }", property);
    int32_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
bool qom_get_bool(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': '/machine', 'property': %s } }", property);
    bool result = qdict_get_bool(response, "return");

    qobject_unref(response);
    return result;
}
uint64_t qom_path_get_uint64(QTestState *qts, const char *path,
                                    const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': "
             "{ 'path': %s, 'property': %s } }", path, property);
    uint64_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
void qom_path_set_uint64(QTestState *qts, const char *path,
                                const char *property, uint64_t value)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-set', 'arguments': "
             "{ 'path': %s, 'property': %s, 'value': %" PRIu64 " } }",
        path, property, value);

    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}
void qom_path_set_uint32(QTestState *qts, const char *path,
                                const char *property, uint32_t value)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-set', 'arguments': "
             "{ 'path': %s, 'property': %s, 'value': %" PRIu32 " } }",
        path, property, value);

    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}
size_t raspi4_dwc2_endpoint_transfer(QTestState *qts, uint8_t address,
                                            uint8_t endpoint, uint8_t type,
                                            uint16_t max_packet, bool in,
                                            uint32_t pid, void *buffer,
                                            size_t length)
{
    uint32_t hcchar = HCCHAR_CHENA |
                      ((uint32_t)address << HCCHAR_DEVADDR_SHIFT) |
                      ((uint32_t)endpoint << HCCHAR_EPNUM_SHIFT) |
                      ((uint32_t)type << HCCHAR_EPTYPE_SHIFT) |
                      (in ? HCCHAR_EPDIR : 0) | max_packet;
    uint32_t hctsiz = (pid << TSIZ_SC_MC_PID_SHIFT) |
                      (MAX(1, DIV_ROUND_UP(length, max_packet)) <<
                       TSIZ_PKTCNT_SHIFT) | length;
    uint32_t last_status = 0;

    g_assert_cmpuint(length, <=, 512);
    qtest_writel(qts, BCM2711_DWC2_BASE + GAHBCFG,
                 qtest_readl(qts, BCM2711_DWC2_BASE + GAHBCFG) |
                 GAHBCFG_DMA_EN);
    qtest_writel(qts, BCM2711_DWC2_BASE + HCINT(0), UINT32_MAX);
    if (!in && length) {
        qtest_memwrite(qts, DWC2_HOST_DMA, buffer, length);
    }
    qtest_writel(qts, BCM2711_DWC2_BASE + HCTSIZ(0), hctsiz);
    qtest_writel(qts, BCM2711_DWC2_BASE + HCDMA(0), DWC2_HOST_DMA);
    qtest_writel(qts, BCM2711_DWC2_BASE + HCCHAR(0), hcchar);

    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        uint32_t status = qtest_readl(
            qts, BCM2711_DWC2_BASE + HCINT(0));

        last_status = status;
        if (status & HCINTMSK_XFERCOMPL) {
            uint32_t remaining = qtest_readl(
                qts, BCM2711_DWC2_BASE + HCTSIZ(0)) &
                TSIZ_XFERSIZE_MASK;

            g_assert_cmpuint(remaining, <=, length);
            if (in && length != remaining) {
                qtest_memread(qts, DWC2_HOST_DMA, buffer,
                              length - remaining);
            }
            return length - remaining;
        }
        g_assert_cmphex(status &
                        (HCINTMSK_STALL | HCINTMSK_BBLERR |
                         HCINTMSK_XACTERR | HCINTMSK_AHBERR), ==, 0);
        qtest_clock_step(qts, 250000);
    }
    g_error("DWC2 host transfer did not complete: address=%u endpoint=%u "
            "type=%u in=%u pid=%u length=%zu hcint=0x%08x",
            address, endpoint, type, in, pid, length, last_status);
}
static size_t raspi4_dwc2_host_transfer(QTestState *qts, uint8_t address,
                                        bool in, uint32_t pid,
                                        void *buffer, size_t length)
{
    return raspi4_dwc2_endpoint_transfer(
        qts, address, 0, USB_ENDPOINT_XFER_CONTROL, 64,
        in, pid, buffer, length);
}
void raspi4_dwc2_reset_root_port(QTestState *qts)
{
    uint32_t hprt = qtest_readl(qts, BCM2711_DWC2_BASE + HPRT0);
    uint32_t writable = hprt &
        ~(HPRT0_CONNDET | HPRT0_ENACHG | HPRT0_OVRCURRCHG);

    g_assert_true(hprt & HPRT0_CONNSTS);
    qtest_writel(qts, BCM2711_DWC2_BASE + HPRT0,
                 writable | HPRT0_PWR | HPRT0_RST);
    qtest_writel(qts, BCM2711_DWC2_BASE + HPRT0,
                 writable | HPRT0_PWR);
    hprt = qtest_readl(qts, BCM2711_DWC2_BASE + HPRT0);
    g_assert_true(hprt & HPRT0_CONNSTS);
    g_assert_true(hprt & HPRT0_ENA);
}
void raspi4_dwc2_get_device_descriptor(QTestState *qts,
                                               uint8_t address,
                                               uint8_t descriptor[18])
{
    uint8_t setup[8] = {
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR,
        0, USB_DT_DEVICE,
        0, 0,
        18, 0,
    };
    uint8_t status = 0;

    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, address, false, TSIZ_SC_MC_PID_SETUP,
                         setup, sizeof(setup)), ==, sizeof(setup));
    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, address, true, TSIZ_SC_MC_PID_DATA1,
                         descriptor, 18), ==, 18);
    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, address, false, TSIZ_SC_MC_PID_DATA1,
                         &status, 0), ==, 0);
}
size_t raspi4_dwc2_control(QTestState *qts, uint8_t address,
                                  uint8_t request_type, uint8_t request,
                                  uint16_t value, uint16_t index,
                                  uint8_t *data, uint16_t length)
{
    uint8_t setup[8] = {
        request_type,
        request,
        value, value >> 8,
        index, index >> 8,
        length, length >> 8,
    };
    bool in = request_type & USB_DIR_IN;
    uint8_t status = 0;
    size_t actual = 0;

    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, address, false, TSIZ_SC_MC_PID_SETUP,
                         setup, sizeof(setup)), ==, sizeof(setup));
    if (length) {
        actual = raspi4_dwc2_host_transfer(
            qts, address, in, TSIZ_SC_MC_PID_DATA1, data, length);
    }
    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, address, !in, TSIZ_SC_MC_PID_DATA1,
                         &status, 0), ==, 0);
    return actual;
}
void raspi4_dwc2_set_address(QTestState *qts, uint8_t old_address,
                                    uint8_t new_address)
{
    uint8_t setup[8] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_ADDRESS,
        new_address, 0,
        0, 0,
        0, 0,
    };
    uint8_t status = 0;

    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, old_address, false, TSIZ_SC_MC_PID_SETUP,
                         setup, sizeof(setup)), ==, sizeof(setup));
    g_assert_cmpuint(raspi4_dwc2_host_transfer(
                         qts, old_address, true, TSIZ_SC_MC_PID_DATA1,
                         &status, 0), ==, 0);
}
void raspi4_dwc2_bot_command(QTestState *qts, uint8_t address,
                                    uint8_t lun, uint32_t tag,
                                    const uint8_t *cdb, uint8_t cdb_length,
                                    uint8_t *data, size_t data_length,
                                    bool require_success)
{
    uint8_t cbw[31] = { 0 };
    uint8_t csw[13] = { 0 };

    g_assert_cmpuint(cdb_length, >, 0);
    g_assert_cmpuint(cdb_length, <=, 16);
    g_assert_cmpuint(data_length, >, 0);
    g_assert_cmpuint(data_length, <=, 512);
    g_test_message("DWC2 BOT address=%u lun=%u opcode=0x%02x tag=0x%08x",
                   address, lun, cdb[0], tag);
    stl_le_p(cbw, 0x43425355);
    stl_le_p(cbw + 4, tag);
    stl_le_p(cbw + 8, data_length);
    cbw[12] = USB_DIR_IN;
    cbw[13] = lun;
    cbw[14] = cdb_length;
    memcpy(cbw + 15, cdb, cdb_length);

    g_assert_cmpuint(raspi4_dwc2_endpoint_transfer(
                         qts, address, 2, USB_ENDPOINT_XFER_BULK, 64,
                         false, TSIZ_SC_MC_PID_DATA0,
                         cbw, sizeof(cbw)), ==, sizeof(cbw));
    g_assert_cmpuint(raspi4_dwc2_endpoint_transfer(
                         qts, address, 1, USB_ENDPOINT_XFER_BULK, 64,
                         true, TSIZ_SC_MC_PID_DATA1,
                         data, data_length), ==, data_length);
    g_assert_cmpuint(raspi4_dwc2_endpoint_transfer(
                         qts, address, 1, USB_ENDPOINT_XFER_BULK, 64,
                         true, TSIZ_SC_MC_PID_DATA1,
                         csw, sizeof(csw)), ==, sizeof(csw));
    g_assert_cmphex(ldl_le_p(csw), ==, 0x53425355);
    g_assert_cmphex(ldl_le_p(csw + 4), ==, tag);
    if (require_success) {
        g_assert_cmpuint(ldl_le_p(csw + 8), ==, 0);
        g_assert_cmpuint(csw[12], ==, 0);
    }
}
static NvmeCqe raspi4_nvme_wait_cqe(QTestState *qts, uint64_t address,
                                    uint16_t cid)
{
    NvmeCqe cqe;

    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        qtest_memread(qts, address, &cqe, sizeof(cqe));
        if (le16_to_cpu(cqe.status) & 1) {
            g_assert_cmpuint(le16_to_cpu(cqe.cid), ==, cid);
            return cqe;
        }
        g_usleep(100);
    }
    g_error("NVMe completion did not arrive at 0x%" PRIx64, address);
}
void raspi4_assert_nvme_rw_statuses(QTestState *qts, uint8_t opcode,
                                           const uint8_t *dirty_data,
                                           const uint8_t *first_data,
                                           const uint8_t *second_data,
                                           uint16_t first_status,
                                           uint16_t second_status)
{
    NvmeCreateCq create_cq = {
        .opcode = NVME_ADM_CMD_CREATE_CQ,
        .cid = cpu_to_le16(1),
        .prp1 = cpu_to_le64(RASPI4_NVME_IO_CQ),
        .cqid = cpu_to_le16(1),
        .qsize = cpu_to_le16(3),
        .cq_flags = cpu_to_le16(NVME_CQ_PC),
    };
    NvmeCreateSq create_sq = {
        .opcode = NVME_ADM_CMD_CREATE_SQ,
        .cid = cpu_to_le16(2),
        .prp1 = cpu_to_le64(RASPI4_NVME_IO_SQ),
        .sqid = cpu_to_le16(1),
        .qsize = cpu_to_le16(3),
        .sq_flags = cpu_to_le16(NVME_SQ_PC),
        .cqid = cpu_to_le16(1),
    };
    NvmeRwCmd rw = {
        .opcode = opcode,
        .cid = cpu_to_le16(3),
        .nsid = cpu_to_le32(1),
        .dptr.prp1 = cpu_to_le64(RASPI4_NVME_DATA),
    };
    NvmeCqe cqe;
    uint8_t zero[4096] = { 0 };
    unsigned int io_index = 0;

    qtest_writel(qts, RASPI4_PCIE_ECAM_BASE + 0x18, 0x00010100);
    qtest_writel(qts, RASPI4_PCIE_ECAM_BASE + 0x20, 0xfff0c000);
    qtest_writew(qts, RASPI4_PCIE_ECAM_BASE + 4, 2);
    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_INDEX, 1 << 20);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_EXT_CFG_DATA), ==,
                    0x00101b36);
    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_DATA + 0x10, 0xc0000004);
    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_DATA + 0x14, 0);
    qtest_writew(qts, RASPI4_PCIE_EXT_CFG_DATA + 4, 6);

    qtest_memwrite(qts, RASPI4_NVME_ADMIN_SQ, zero, sizeof(zero));
    qtest_memwrite(qts, RASPI4_NVME_ADMIN_CQ, zero, sizeof(zero));
    qtest_memwrite(qts, RASPI4_NVME_IO_SQ, zero, sizeof(zero));
    qtest_memwrite(qts, RASPI4_NVME_IO_CQ, zero, sizeof(zero));
    qtest_memwrite(qts, RASPI4_NVME_DATA,
                   first_data ? first_data : zero, 512);
    qtest_writel(qts, RASPI4_NVME_BAR + NVME_REG_AQA, (3 << 16) | 3);
    qtest_writeq(qts, RASPI4_NVME_BAR + NVME_REG_ASQ,
                 RASPI4_NVME_ADMIN_SQ);
    qtest_writeq(qts, RASPI4_NVME_BAR + NVME_REG_ACQ,
                 RASPI4_NVME_ADMIN_CQ);
    qtest_writel(qts, RASPI4_NVME_BAR + NVME_REG_CC,
                 1 | (6 << 16) | (4 << 20));
    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        if (qtest_readl(qts, RASPI4_NVME_BAR + NVME_REG_CSTS) &
            NVME_CSTS_READY) {
            break;
        }
        g_assert_cmpuint(attempt, !=, 9999);
        g_usleep(100);
    }

    qtest_memwrite(qts, RASPI4_NVME_ADMIN_SQ, &create_cq,
                   sizeof(create_cq));
    qtest_writel(qts, RASPI4_NVME_BAR + 0x1000, 1);
    cqe = raspi4_nvme_wait_cqe(qts, RASPI4_NVME_ADMIN_CQ, 1);
    g_assert_cmpuint(le16_to_cpu(cqe.status) >> 1, ==, NVME_SUCCESS);
    qtest_writel(qts, RASPI4_NVME_BAR + 0x1004, 1);

    qtest_memwrite(qts, RASPI4_NVME_ADMIN_SQ + sizeof(NvmeCmd),
                   &create_sq, sizeof(create_sq));
    qtest_writel(qts, RASPI4_NVME_BAR + 0x1000, 2);
    cqe = raspi4_nvme_wait_cqe(
        qts, RASPI4_NVME_ADMIN_CQ + sizeof(NvmeCqe), 2);
    g_assert_cmpuint(le16_to_cpu(cqe.status) >> 1, ==, NVME_SUCCESS);
    qtest_writel(qts, RASPI4_NVME_BAR + 0x1004, 2);

    if (dirty_data) {
        NvmeRwCmd dirty = {
            .opcode = NVME_CMD_WRITE,
            .cid = cpu_to_le16(5),
            .nsid = cpu_to_le32(1),
            .dptr.prp1 = cpu_to_le64(RASPI4_NVME_DATA),
        };

        qtest_memwrite(qts, RASPI4_NVME_DATA, dirty_data, 512);
        qtest_memwrite(qts, RASPI4_NVME_IO_SQ, &dirty, sizeof(dirty));
        qtest_writel(qts, RASPI4_NVME_BAR + 0x1008, 1);
        cqe = raspi4_nvme_wait_cqe(qts, RASPI4_NVME_IO_CQ, 5);
        g_assert_cmpuint(le16_to_cpu(cqe.status) >> 1, ==, NVME_SUCCESS);
        qtest_writel(qts, RASPI4_NVME_BAR + 0x100c, 1);
        io_index = 1;
    }

    qtest_memwrite(qts, RASPI4_NVME_DATA,
                   first_data ? first_data : zero, 512);
    qtest_memwrite(qts, RASPI4_NVME_IO_SQ + io_index * sizeof(NvmeCmd),
                   &rw, sizeof(rw));
    qtest_writel(qts, RASPI4_NVME_BAR + 0x1008, io_index + 1);
    cqe = raspi4_nvme_wait_cqe(
        qts, RASPI4_NVME_IO_CQ + io_index * sizeof(NvmeCqe), 3);
    g_assert_cmpuint(le16_to_cpu(cqe.status) >> 1, ==, first_status);
    qtest_writel(qts, RASPI4_NVME_BAR + 0x100c, io_index + 1);
    io_index++;

    if (second_data) {
        qtest_memwrite(qts, RASPI4_NVME_DATA, second_data, 512);
    }
    rw.cid = cpu_to_le16(4);
    qtest_memwrite(qts, RASPI4_NVME_IO_SQ + io_index * sizeof(NvmeCmd),
                   &rw, sizeof(rw));
    qtest_writel(qts, RASPI4_NVME_BAR + 0x1008, io_index + 1);
    cqe = raspi4_nvme_wait_cqe(
        qts, RASPI4_NVME_IO_CQ + io_index * sizeof(NvmeCqe), 4);
    g_assert_cmpuint(le16_to_cpu(cqe.status) >> 1, ==, second_status);
    qtest_writel(qts, RASPI4_NVME_BAR + 0x100c, io_index + 1);
}
void wait_for_migration_complete(QTestState *qts)
{
    for (unsigned int attempt = 0; attempt < 30000; attempt++) {
        QDict *response = qtest_qmp(qts,
                                    "{ 'execute': 'query-migrate' }");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");
        bool completed = !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(response);
        if (completed) {
            return;
        }
        g_usleep(1000);
    }
    g_error("migration did not complete within 30 seconds");
}
QTestState *migrate_to_new_qtest_commands(
    QTestState *source, const char *destination_base,
    char **directory_out, char **socket_path_out)
{
    g_autofree char *uri = NULL;
    g_autofree char *destination_command = NULL;
    QTestState *destination;

    *directory_out = g_dir_make_tmp("raspi4-boot-migration-XXXXXX", NULL);
    g_assert_nonnull(*directory_out);
    *socket_path_out = g_build_filename(*directory_out, "stream.sock", NULL);
    uri = g_strdup_printf("unix:%s", *socket_path_out);
    destination_command = g_strdup_printf("%s -incoming %s",
                                          destination_base, uri);
    destination = qtest_init(destination_command);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);
    return destination;
}
QTestState *migrate_to_new_qtest(QTestState *source,
                                        const char *command,
                                        char **directory_out,
                                        char **socket_path_out)
{
    return migrate_to_new_qtest_commands(
        source, command, directory_out, socket_path_out);
}
void assert_bus_card_type(QTestState *qts, const char *bus_path,
                                 const char *card_type)
{
    g_autofree char *link_type = g_strdup_printf("link<%s>", card_type);
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-list', 'arguments': { 'path': %s } }",
        bus_path);
    QList *entries = qdict_get_qlist(response, "return");
    QListEntry *entry;
    bool found = false;

    QLIST_FOREACH_ENTRY(entries, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(property, "name");
        const char *type = qdict_get_str(property, "type");

        if (g_str_has_prefix(name, "child[") && !strcmp(type, link_type)) {
            found = true;
            break;
        }
    }
    g_assert_true(found);
    qobject_unref(response);
}
#ifndef _WIN32
int gpio_bridge_connect(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int elapsed_ms = 0;

    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    while (elapsed_ms < 2000) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);

        g_assert_cmpint(fd, >=, 0);
        if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) {
            return fd;
        }
        close(fd);
        g_usleep(10 * 1000);
        elapsed_ms += 10;
    }
    return -1;
}
#endif
#ifndef _WIN32
void gpio_bridge_write(int fd, const char *data)
{
    size_t offset = 0;
    size_t length = strlen(data);

    while (offset < length) {
        ssize_t written = RETRY_ON_EINTR(write(fd, data + offset,
                                               length - offset));

        g_assert_cmpint(written, >, 0);
        offset += written;
    }
}
#endif
#ifndef _WIN32
char *gpio_bridge_read(int fd)
{
    g_autoptr(GString) result = g_string_new(NULL);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int timeout_ms = 2000;

    while (poll(&pfd, 1, timeout_ms) == 1) {
        char buffer[4096];
        ssize_t length = RETRY_ON_EINTR(read(fd, buffer, sizeof(buffer)));

        g_assert_cmpint(length, >, 0);
        g_string_append_len(result, buffer, length);
        timeout_ms = 20;
    }
    return g_string_free(g_steal_pointer(&result), false);
}
#endif
#ifndef _WIN32
void dwc2_transport_write_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = RETRY_ON_EINTR(
            write(fd, bytes + offset, length - offset));

        g_assert_cmpint(written, >, 0);
        offset += written;
    }
}
#endif
#ifndef _WIN32
void dwc2_transport_read_all(int fd, void *data, size_t length)
{
    uint8_t *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        ssize_t received;

        g_assert_cmpint(RETRY_ON_EINTR(poll(&pfd, 1, 2000)), ==, 1);
        received = RETRY_ON_EINTR(read(fd, bytes + offset, length - offset));
        g_assert_cmpint(received, >, 0);
        offset += received;
    }
}
#endif
#ifndef _WIN32
int32_t dwc2_transport_exchange(int fd, uint8_t opcode, uint8_t ep,
                                       uint32_t value, const void *payload,
                                       uint32_t length, void *reply,
                                       uint32_t reply_capacity)
{
    uint8_t header[DWC2_DEVICE_TRANSPORT_HEADER_SIZE] = { 0 };
    uint8_t response[DWC2_DEVICE_TRANSPORT_HEADER_SIZE];
    uint32_t reply_length;

    stl_le_p(header, DWC2_DEVICE_TRANSPORT_MAGIC);
    header[4] = DWC2_DEVICE_TRANSPORT_VERSION;
    header[5] = opcode;
    header[6] = ep;
    stl_le_p(header + 8, length);
    stl_le_p(header + 12, value);

    /* Deliberately fragment the header to exercise stream reassembly. */
    dwc2_transport_write_all(fd, header, 5);
    dwc2_transport_write_all(fd, header + 5, sizeof(header) - 5);
    if (length) {
        dwc2_transport_write_all(fd, payload, length);
    }

    dwc2_transport_read_all(fd, response, sizeof(response));
    g_assert_cmphex(ldl_le_p(response), ==,
                    DWC2_DEVICE_TRANSPORT_MAGIC);
    g_assert_cmpuint(response[4], ==, DWC2_DEVICE_TRANSPORT_VERSION);
    g_assert_cmpuint(response[5], ==,
                     opcode | DWC2_DEVICE_TRANSPORT_RESPONSE);
    g_assert_cmpuint(response[6], ==, ep);
    reply_length = ldl_le_p(response + 8);
    g_assert_cmpuint(reply_length, <=, reply_capacity);
    if (reply_length) {
        g_assert_nonnull(reply);
        dwc2_transport_read_all(fd, reply, reply_length);
    }
    return (int32_t)ldl_le_p(response + 12);
}
#endif
#ifndef _WIN32
static size_t dwc2_transport_control_in(int fd, void *data, size_t length)
{
    uint8_t *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        int32_t received = dwc2_transport_exchange(
            fd, DWC2_DEVICE_TRANSPORT_IN, 0, 64, NULL, 0,
            bytes + offset, length - offset);

        g_assert_cmpint(received, >=, 0);
        offset += received;
        if (received < 64) {
            break;
        }
    }
    return offset;
}
#endif
#ifndef _WIN32
size_t dwc2_rpiboot_standard_control(int fd, const uint8_t setup[8],
                                            void *response,
                                            size_t response_capacity)
{
    uint16_t length = lduw_le_p(setup + 6);

    g_assert_cmpint(dwc2_transport_exchange(
                        fd, DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        setup, 8, NULL, 0), ==, 8);
    if (setup[0] & USB_DIR_IN) {
        size_t received = dwc2_transport_control_in(
            fd, response, MIN((size_t)length, response_capacity));

        g_assert_cmpint(dwc2_transport_exchange(
                            fd, DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                            NULL, 0, NULL, 0), ==, 0);
        return received;
    }

    g_assert_cmpuint(length, ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        fd, DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, response, response_capacity), ==, 0);
    return 0;
}
#endif
#ifndef _WIN32
void dwc2_rpiboot_configure(int fd, uint8_t address)
{
    uint8_t set_address[8] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_ADDRESS,
    };
    uint8_t set_configuration[8] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION, 1,
    };
    uint8_t ignored[1];

    set_address[2] = address;
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         fd, set_address, ignored, sizeof(ignored)), ==, 0);
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         fd, set_configuration,
                         ignored, sizeof(ignored)), ==, 0);
}
#endif
#ifndef _WIN32
static void dwc2_rpiboot_announce(int fd, uint32_t length)
{
    uint8_t setup[8] = { 0x40, 0 };
    uint8_t ignored[64];

    stw_le_p(setup + 2, length);
    stw_le_p(setup + 4, length >> 16);
    g_assert_cmpint(dwc2_transport_exchange(
                        fd, DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        setup, sizeof(setup), NULL, 0), ==, sizeof(setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        fd, DWC2_DEVICE_TRANSPORT_IN, 0, sizeof(ignored),
                        NULL, 0, ignored, sizeof(ignored)), ==, 0);
}
#endif
#ifndef _WIN32
static void dwc2_rpiboot_get_file_message(int fd, uint8_t message[260])
{
    uint8_t setup[8] = { USB_DIR_IN | USB_TYPE_VENDOR, 0 };

    stw_le_p(setup + 6, 260);
    g_assert_cmpint(dwc2_transport_exchange(
                        fd, DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        setup, sizeof(setup), NULL, 0), ==, sizeof(setup));
    g_assert_cmpuint(dwc2_transport_control_in(fd, message, 260), ==, 260);
    g_assert_cmpint(dwc2_transport_exchange(
                        fd, DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
}
#endif
void qom_set_uint64(QTestState *qts, const char *property,
                           uint64_t value)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-set', 'arguments': "
             "{ 'path': '/machine', 'property': %s, 'value': %" PRIu64
             " } }", property, value);

    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}
void qom_set_uint32(QTestState *qts, const char *property,
                           uint32_t value)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-set', 'arguments': "
             "{ 'path': '/machine', 'property': %s, 'value': %" PRIu32
             " } }", property, value);

    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}
void qom_set_int32(QTestState *qts, const char *property,
                          int32_t value)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-set', 'arguments': "
             "{ 'path': '/machine', 'property': %s, 'value': %" PRId32
             " } }", property, value);

    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}
void qom_set_bool(QTestState *qts, const char *property, bool value)
{
    qtest_qom_set_bool(qts, "/machine", property, value);
}
void qom_set_string(QTestState *qts, const char *property,
                           const char *value)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-set', 'arguments': "
             "{ 'path': '/machine', 'property': %s, 'value': %s } }",
        property, value);

    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}
void make_test_public_key(uint8_t public_key[264])
{
    for (size_t i = 0; i < 264; i++) {
        public_key[i] = i ^ 0xa5;
    }
}
uint8_t *make_eeprom_image(const char *config, bool corrupt)
{
    uint8_t *image = g_malloc(EEPROM_SIZE);

    memset(image, 0xff, EEPROM_SIZE);
    stl_be_p(image, corrupt ? 0x12345678 : FILE_MAGIC);
    stl_be_p(image + 4, 12 + 4 + strlen(config));
    memcpy(image + 8, "bootconf.txt", 12);
    memset(image + 20, 0, 4);
    memcpy(image + 24, config, strlen(config));
    return image;
}
static void set_eeprom_build_identity(uint8_t *image, uint32_t timestamp,
                                      const char *version)
{
    size_t offset = EEPROM_SIZE - 128;
    int length;

    memset(image + offset, 0, 128);
    length = snprintf((char *)image + offset, 128,
                      "BUILD_TIMESTAMP=%" PRIu32, timestamp);
    g_assert_cmpint(length, >, 0);
    offset += length + 1;
    length = snprintf((char *)image + offset, EEPROM_SIZE - offset,
                      "VERSION:%s", version);
    g_assert_cmpint(length, >, 0);
    g_assert_cmpuint(offset + length, <, EEPROM_SIZE);
}
static size_t eeprom_add_file(uint8_t *image, size_t offset,
                              const char name[12], const uint8_t *contents,
                              size_t contents_size)
{
    size_t length = 12 + 4 + contents_size;
    size_t next = QEMU_ALIGN_UP(offset + 8 + length, 8);

    g_assert_cmpuint(next, <, EEPROM_SIZE - 4096);
    stl_be_p(image + offset, FILE_MAGIC);
    stl_be_p(image + offset + 4, length);
    memcpy(image + offset + 8, name, 12);
    memset(image + offset + 20, 0, 4);
    memcpy(image + offset + 24, contents, contents_size);
    return next;
}
static uint8_t *make_eeprom_image_with_public_key(const char *config)
{
    g_autofree uint8_t *image = make_eeprom_image(config, false);
    uint8_t public_key[264];
    size_t config_length = 12 + 4 + strlen(config);
    size_t offset = QEMU_ALIGN_UP(8 + config_length, 8);

    make_test_public_key(public_key);
    eeprom_add_file(image, offset, "pubkey.bin\0\0",
                    public_key, sizeof(public_key));
    return g_steal_pointer(&image);
}
uint8_t *make_test_recovery_sized(size_t payload_size,
                                         size_t *recovery_size)
{
    size_t size = payload_size + sizeof(uint32_t) * 2 +
                  TEST_RECOVERY_RSA_SIZE + TEST_RECOVERY_HMAC_SIZE;
    uint8_t *recovery = g_malloc0(size);
    size_t rsa_offset = payload_size + sizeof(uint32_t) * 2;

    g_assert_cmpuint(payload_size, >=, TEST_RECOVERY_PAYLOAD_SIZE);
    memcpy(recovery, "RPI4", TEST_RECOVERY_PAYLOAD_SIZE);
    for (size_t i = TEST_RECOVERY_PAYLOAD_SIZE; i < payload_size; i++) {
        recovery[i] = i;
    }
    stl_le_p(recovery + payload_size, payload_size);
    stl_le_p(recovery + payload_size + sizeof(uint32_t), 1);
    for (size_t i = 0; i < TEST_RECOVERY_RSA_SIZE; i++) {
        recovery[rsa_offset + i] = i + 1;
    }
    memset(recovery + rsa_offset + TEST_RECOVERY_RSA_SIZE, 0xa5,
           TEST_RECOVERY_HMAC_SIZE);
    *recovery_size = size;
    return recovery;
}
uint8_t *make_test_recovery(void)
{
    size_t recovery_size;
    uint8_t *recovery = make_test_recovery_sized(
        TEST_RECOVERY_PAYLOAD_SIZE, &recovery_size);

    g_assert_cmpuint(recovery_size, ==, TEST_RECOVERY_SIZE);
    return recovery;
}
static size_t eeprom_add_bootsys(uint8_t *image)
{
    const size_t contents_size = TEST_BOOTSYS_PAYLOAD_SIZE +
        sizeof(uint32_t) * 2 + TEST_BOOTSYS_RSA_SIZE +
        TEST_BOOTSYS_HMAC_SIZE;
    const size_t next = QEMU_ALIGN_UP(8 + contents_size, 8);
    uint8_t *contents = image + 8;
    uint8_t *trailer = contents + TEST_BOOTSYS_PAYLOAD_SIZE;

    stl_be_p(image, 0x55aaf00f);
    stl_be_p(image + 4, contents_size);
    for (unsigned int i = 0; i < TEST_BOOTSYS_PAYLOAD_SIZE; i++) {
        contents[i] = 0xa0 + i;
    }
    memcpy(contents, test_dependency_hash, sizeof(test_dependency_hash));
    stl_le_p(trailer, TEST_BOOTSYS_PAYLOAD_SIZE);
    stl_le_p(trailer + sizeof(uint32_t), 1);
    for (unsigned int i = 0; i < TEST_BOOTSYS_RSA_SIZE; i++) {
        trailer[sizeof(uint32_t) * 2 + i] = i + 1;
    }
    for (unsigned int i = 0; i < TEST_BOOTSYS_HMAC_SIZE; i++) {
        trailer[sizeof(uint32_t) * 2 + TEST_BOOTSYS_RSA_SIZE + i] =
            0x80 + i;
    }
    return next;
}
static size_t eeprom_add_dependency(uint8_t *image, size_t offset,
                                    const char *name)
{
    const size_t data_size = sizeof(test_dependency_contents) - 1;
    const size_t length = TEST_DEPENDENCY_NAME_SIZE +
                          TEST_DEPENDENCY_FRAME_SIZE +
                          TEST_DEPENDENCY_HASH_SIZE;
    const size_t next = QEMU_ALIGN_UP(offset + 8 + length, 8);
    uint8_t *contents = image + offset + 8;
    uint8_t *frame = contents + TEST_DEPENDENCY_NAME_SIZE;
    size_t frame_offset = 0;

    g_assert_cmpuint(strlen(name), <, TEST_DEPENDENCY_NAME_SIZE);
    g_assert_cmpuint(next, <, EEPROM_SIZE - 4096);
    stl_be_p(image + offset, TEST_DEPENDENCY_MAGIC);
    stl_be_p(image + offset + 4, length);
    memset(contents, 0, TEST_DEPENDENCY_NAME_SIZE);
    memcpy(contents, name, strlen(name));
    stl_le_p(frame + frame_offset, 0x184d2204);
    frame_offset += sizeof(uint32_t);
    frame[frame_offset++] = 0x48;
    frame[frame_offset++] = 0x40;
    stq_le_p(frame + frame_offset, data_size);
    frame_offset += sizeof(uint64_t);
    frame[frame_offset++] = 0xc7;
    stl_le_p(frame + frame_offset, BIT(31) | 8);
    frame_offset += sizeof(uint32_t);
    memcpy(frame + frame_offset, test_dependency_contents, 8);
    frame_offset += 8;
    stl_le_p(frame + frame_offset, sizeof(test_dependency_second_block));
    frame_offset += sizeof(uint32_t);
    memcpy(frame + frame_offset, test_dependency_second_block,
           sizeof(test_dependency_second_block));
    frame_offset += sizeof(test_dependency_second_block);
    stl_le_p(frame + frame_offset, 0);
    frame_offset += sizeof(uint32_t);
    g_assert_cmpuint(frame_offset, ==, TEST_DEPENDENCY_FRAME_SIZE);
    memcpy(frame + frame_offset, test_dependency_hash,
           sizeof(test_dependency_hash));
    return next;
}
static size_t eeprom_add_secure_dependencies(uint8_t *image, size_t offset)
{
    offset = eeprom_add_dependency(image, offset, "bootmain");
    offset = eeprom_add_dependency(image, offset, "mcb.bin");
    return eeprom_add_dependency(image, offset, "memsys00.bin");
}
static uint8_t test_hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    return g_ascii_tolower(value) - 'a' + 10;
}
void test_decode_hex(const char *text, uint8_t *output,
                            size_t output_size)
{
    g_assert_cmpuint(strlen(text), ==, output_size * 2);
    for (size_t i = 0; i < output_size; i++) {
        g_assert_true(g_ascii_isxdigit(text[i * 2]));
        g_assert_true(g_ascii_isxdigit(text[i * 2 + 1]));
        output[i] = test_hex_nibble(text[i * 2]) << 4 |
                    test_hex_nibble(text[i * 2 + 1]);
    }
}
uint8_t *make_otp_image(uint32_t bootmode, uint32_t bootmode_copy,
                               uint32_t board_revision, bool customer_key)
{
    uint8_t *image = g_malloc0(OTP_SIZE);

    stl_le_p(image + (17 - 1) * 4, bootmode);
    stl_le_p(image + (18 - 1) * 4, bootmode_copy);
    stl_le_p(image + (30 - 1) * 4, board_revision);
    if (customer_key) {
        stl_le_p(image + (47 - 1) * 4, 0x01234567);
    }
    return image;
}
uint8_t *make_secure_otp_image(const uint8_t key_hash[32])
{
    uint8_t *image = make_otp_image(
        BIT(15), BIT(15), PI4_BOARD_REVISION, false);

    memcpy(image + (47 - 1) * 4, key_hash, 32);
    return image;
}
uint8_t *make_arm64_kernel(void)
{
    uint8_t *image = g_malloc0(TEST_KERNEL_SIZE);

    stl_le_p(image, 0x14000000); /* b . */
    stq_le_p(image + 8, 0);      /* firmware-selected load offset */
    stq_le_p(image + 16, TEST_KERNEL_SIZE);
    memcpy(image + 56, "ARM\x64", 4);
    return image;
}
uint8_t *make_arm32_kernel(void)
{
    uint8_t *image = g_malloc0(TEST_KERNEL_SIZE);

    stl_le_p(image, 0xeafffffe); /* b . */
    stl_le_p(image + 0x24, 0x016f2818);
    stl_le_p(image + 0x28, 0);
    stl_le_p(image + 0x2c, TEST_KERNEL_SIZE);
    return image;
}
static void fdt_set_override(void *fdt, int overrides, const char *name,
                             uint32_t phandle, const char *descriptor)
{
    uint8_t value[128];
    size_t length = sizeof(uint32_t) + strlen(descriptor) + 1;

    g_assert_cmpuint(length, <=, sizeof(value));
    stl_be_p(value, phandle);
    memcpy(value + sizeof(uint32_t), descriptor, strlen(descriptor) + 1);
    g_assert_cmpint(fdt_setprop(fdt, overrides, name, value, length), ==, 0);
}
static size_t fdt_set_override_cell(void *fdt, int overrides,
                                    const char *name, uint32_t phandle,
                                    const char *descriptor, uint32_t cell)
{
    uint8_t value[128] = { 0 };
    size_t cell_offset = sizeof(uint32_t) + strlen(descriptor) + 1;
    size_t length = cell_offset + sizeof(uint32_t);

    g_assert_cmpuint(length, <=, sizeof(value));
    stl_be_p(value, phandle);
    memcpy(value + sizeof(uint32_t), descriptor, strlen(descriptor) + 1);
    stl_be_p(value + cell_offset, cell);
    g_assert_cmpint(fdt_setprop(fdt, overrides, name, value, length), ==, 0);
    return cell_offset;
}
static FdtLookupCellOffsets fdt_set_override_lookup_cells(
    void *fdt, int overrides, const char *name, uint32_t phandle,
    const char *descriptor, uint32_t local_cell, uint32_t external_cell)
{
    uint8_t value[256] = { 0 };
    g_autofree char *local = g_strdup_printf("%s{local=", descriptor);
    static const char external[] = "external=";
    static const char closing[] = "}";
    FdtLookupCellOffsets offsets;
    size_t cursor = 0;

    stl_be_p(value + cursor, phandle);
    cursor += sizeof(uint32_t);
    memcpy(value + cursor, local, strlen(local) + 1);
    cursor += strlen(local) + 1;
    offsets.local = cursor;
    stl_be_p(value + cursor, local_cell);
    cursor += sizeof(uint32_t);
    memcpy(value + cursor, external, sizeof(external));
    cursor += sizeof(external);
    offsets.external = cursor;
    stl_be_p(value + cursor, external_cell);
    cursor += sizeof(uint32_t);
    memcpy(value + cursor, closing, sizeof(closing));
    cursor += sizeof(closing);

    g_assert_cmpuint(cursor, <=, sizeof(value));
    g_assert_cmpint(fdt_setprop(fdt, overrides, name, value, cursor), ==, 0);
    return offsets;
}
static void fdt_set_override_two_targets(
    void *fdt, int overrides, const char *name,
    uint32_t first_phandle, const char *first_descriptor,
    uint32_t second_phandle, const char *second_descriptor)
{
    uint8_t value[256];
    size_t cursor = 0;

    stl_be_p(value + cursor, first_phandle);
    cursor += sizeof(uint32_t);
    memcpy(value + cursor, first_descriptor, strlen(first_descriptor) + 1);
    cursor += strlen(first_descriptor) + 1;
    stl_be_p(value + cursor, second_phandle);
    cursor += sizeof(uint32_t);
    memcpy(value + cursor, second_descriptor, strlen(second_descriptor) + 1);
    cursor += strlen(second_descriptor) + 1;
    g_assert_cmpuint(cursor, <=, sizeof(value));
    g_assert_cmpint(fdt_setprop(fdt, overrides, name, value, cursor), ==, 0);
}
uint32_t fdt_get_u32(const void *fdt, int node, const char *property)
{
    int length;
    const uint8_t *value = fdt_getprop(fdt, node, property, &length);

    g_assert_nonnull(value);
    g_assert_cmpint(length, ==, sizeof(uint32_t));
    return ldl_be_p(value);
}
void assert_bootloader_reserved_blob(
    QTestState *qts, const char *compatible,
    const uint8_t *expected, size_t expected_size)
{
    uint64_t dt_address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t dt_size = qom_get_uint64(
        qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(dt_size);
    g_autofree uint8_t *actual = g_malloc(expected_size);
    const uint8_t *reg;
    uint64_t address;
    int length;
    int node;

    qtest_memread(qts, dt_address, fdt, dt_size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_node_offset_by_compatible(fdt, -1, compatible);
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr(fdt_getprop(fdt, node, "status", NULL), ==, "okay");
    reg = fdt_getprop(fdt, node, "reg", &length);
    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 3 * sizeof(uint32_t));
    address = ((uint64_t)ldl_be_p(reg) << 32) | ldl_be_p(reg + 4);
    g_assert_cmpuint(ldl_be_p(reg + 8), ==, expected_size);
    g_assert_cmpuint(address, <, 1 * GiB);
    g_assert_false(address & 63);
    qtest_memread(qts, address, actual, expected_size);
    g_assert_cmpmem(actual, expected_size, expected, expected_size);
}
void assert_bootloader_boot_mode(QTestState *qts, uint32_t boot_mode)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/chosen/bootloader");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-mode"), ==,
                     boot_mode);
    g_assert_cmpuint(fdt_get_u32(fdt, node, "boot-mode"), ==, boot_mode);
}
void assert_bootloader_device_tree(QTestState *qts,
                                          uint32_t boot_mode,
                                          uint32_t partition,
                                          uint32_t tryboot)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    assert_bootloader_boot_mode(qts, boot_mode);
    qtest_memread(qts, address, fdt, size);
    node = fdt_path_offset(fdt, "/chosen/bootloader");
    g_assert_cmpuint(fdt_get_u32(fdt, node, "pm_rsts"), ==,
                     qom_get_uint32(qts, "reset-status"));
    g_assert_null(fdt_getprop(fdt, node, "rsts", NULL));
    g_assert_cmpuint(fdt_get_u32(fdt, node, "partition"), ==, partition);
    g_assert_cmpuint(fdt_get_u32(fdt, node, "tryboot"), ==, tryboot);
}
uint32_t firmware_bootloader_u32(QTestState *qts,
                                       const char *property)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    qtest_memread(qts, address, fdt, size);
    node = fdt_path_offset(fdt, "/chosen/bootloader");
    g_assert_cmpint(node, >=, 0);
    return fdt_get_u32(fdt, node, property);
}
void assert_bootloader_build_identity(QTestState *qts, bool valid,
                                             uint32_t timestamp,
                                             const char *version)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    g_autofree char *qom_version =
        qom_get_string(qts, "boot-eeprom-version");
    bool capabilities_valid = valid &&
                              timestamp >= 1625568293;
    const uint8_t *property;
    int length;
    int node;

    g_assert_cmpint(qom_get_bool(
                        qts, "boot-eeprom-build-timestamp-valid"), ==,
                    valid);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "boot-eeprom-build-timestamp"), ==,
                     timestamp);
    g_assert_cmpstr(qom_version, ==, valid ? version : "none");
    g_assert_cmpint(qom_get_bool(
                        qts, "boot-eeprom-capabilities-valid"), ==,
                    capabilities_valid);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "boot-eeprom-capabilities"), ==,
                     capabilities_valid ? 0x7f : 0);
    qtest_memread(qts, address, fdt, size);
    node = fdt_path_offset(fdt, "/chosen/bootloader");
    g_assert_cmpint(node, >=, 0);
    property = fdt_getprop(fdt, node, "build_timestamp", &length);
    if (!valid) {
        g_assert_null(property);
        g_assert_cmpint(length, ==, -FDT_ERR_NOTFOUND);
        g_assert_null(fdt_getprop(fdt, node, "version", &length));
        g_assert_cmpint(length, ==, -FDT_ERR_NOTFOUND);
        g_assert_null(fdt_getprop(fdt, node, "capabilities", &length));
        g_assert_cmpint(length, ==, -FDT_ERR_NOTFOUND);
        return;
    }
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, sizeof(uint32_t));
    g_assert_cmpuint(ldl_be_p(property), ==, timestamp);
    property = fdt_getprop(fdt, node, "version", &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, strlen(version) + 1);
    g_assert_cmpstr((const char *)property, ==, version);
    property = fdt_getprop(fdt, node, "capabilities", &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, sizeof(uint32_t));
    g_assert_cmpuint(ldl_be_p(property), ==, 0x7f);
}
void assert_bootloader_update_timestamp(QTestState *qts, bool valid,
                                               uint32_t timestamp)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    const uint8_t *property;
    int length;
    int node;

    g_assert_cmpint(qom_get_bool(
                        qts, "boot-eeprom-update-timestamp-valid"), ==,
                    valid);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "boot-eeprom-update-timestamp"), ==,
                     timestamp);
    qtest_memread(qts, address, fdt, size);
    node = fdt_path_offset(fdt, "/chosen/bootloader");
    g_assert_cmpint(node, >=, 0);
    property = fdt_getprop(fdt, node, "update_timestamp", &length);
    if (!valid) {
        g_assert_null(property);
        g_assert_cmpint(length, ==, -FDT_ERR_NOTFOUND);
        return;
    }
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, sizeof(uint32_t));
    g_assert_cmpuint(ldl_be_p(property), ==, timestamp);
}
void assert_bootloader_usb_identity(QTestState *qts, bool valid,
                                           uint32_t version,
                                           uint32_t route_string,
                                           uint32_t root_hub_port,
                                           uint32_t lun)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    g_assert_cmpint(qom_get_bool(qts, "usb-boot-identity-valid"), ==,
                    valid);
    g_assert_cmpuint(qom_get_uint32(qts, "usb-boot-version"), ==,
                     valid ? version : 0);
    g_assert_cmpuint(qom_get_uint32(qts, "usb-boot-route-string"), ==,
                     valid ? route_string : 0);
    g_assert_cmpuint(qom_get_uint32(qts, "usb-boot-root-hub-port"), ==,
                     valid ? root_hub_port : 0);
    qtest_memread(qts, address, fdt, size);
    node = fdt_path_offset(fdt, "/chosen/bootloader/usb");
    if (!valid) {
        g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
        return;
    }
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpuint(fdt_get_u32(fdt, node, "usb-version"), ==, version);
    g_assert_cmpuint(fdt_get_u32(fdt, node, "route-string"), ==,
                     route_string);
    g_assert_cmpuint(fdt_get_u32(fdt, node, "root-hub-port-number"), ==,
                     root_hub_port);
    g_assert_cmpuint(fdt_get_u32(fdt, node, "lun"), ==, lun);
}
void assert_firmware_system_identity(QTestState *qts,
                                            uint32_t expected_revision,
                                            uint32_t expected_serial)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    const uint8_t *property;
    g_autofree char *expected_serial_number = g_strdup_printf(
        "%016" PRIx64, (uint64_t)expected_serial);
    int length;
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/system");
    g_assert_cmpint(node, >=, 0);
    property = fdt_getprop(fdt, node, "linux,revision", &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, sizeof(uint32_t));
    g_assert_cmphex(ldl_be_p(property), ==, expected_revision);
    property = fdt_getprop(fdt, node, "linux,serial", &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, sizeof(uint64_t));
    g_assert_cmphex(ldq_be_p(property), ==, expected_serial);
    property = fdt_getprop(fdt, 0, "serial-number", &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, strlen(expected_serial_number) + 1);
    g_assert_cmpstr((const char *)property, ==, expected_serial_number);
}
uint64_t firmware_kaslr_seed(QTestState *qts)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    const uint8_t *property;
    int length;
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/chosen");
    g_assert_cmpint(node, >=, 0);
    property = fdt_getprop(fdt, node, "kaslr-seed", &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, sizeof(uint64_t));
    return ldq_be_p(property);
}
uint32_t firmware_min_boot_version(QTestState *qts)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/chosen");
    g_assert_cmpint(node, >=, 0);
    return fdt_get_u32(fdt, node, "rpi-min-boot-ver");
}
uint32_t firmware_sdram_size_gbit(QTestState *qts)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/chosen");
    g_assert_cmpint(node, >=, 0);
    return fdt_get_u32(fdt, node, "rpi-sdram-size-gbit");
}
uint32_t firmware_board_revision_ext(QTestState *qts)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/chosen");
    g_assert_cmpint(node, >=, 0);
    return fdt_get_u32(fdt, node, "rpi-boardrev-ext");
}
void assert_firmware_chosen_string(QTestState *qts,
                                          const char *name,
                                          const char *expected)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    const char *property;
    int length;
    int node;

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_path_offset(fdt, "/chosen");
    g_assert_cmpint(node, >=, 0);
    property = fdt_getprop(fdt, node, name, &length);
    g_assert_nonnull(property);
    g_assert_cmpint(length, ==, strlen(expected) + 1);
    g_assert_cmpstr(property, ==, expected);
}
uint8_t *make_device_tree(size_t *size)
{
    size_t capacity = 4096;
    uint8_t *fdt = g_malloc0(capacity);
    int memory;
    int peripheral;
    int overrides;
    int aliases;
    int chosen;
    int symbols;
    int uart;
    int avs;
    int thermal;
    int genet;

    g_assert_cmpint(fdt_create_empty_tree(fdt, capacity), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, 0, "#address-cells", 2), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, 0, "#size-cells", 2), ==, 0);
    chosen = fdt_add_subnode(fdt, 0, "chosen");
    g_assert_cmpint(chosen, >=, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, chosen, "bootargs",
                        "coherent_pool=1M 8250.nr_uarts=1"), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, chosen, "phandle", 2), ==, 0);
    symbols = fdt_add_subnode(fdt, 0, "__symbols__");
    g_assert_cmpint(symbols, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, symbols, "chosen_label",
                                      "/chosen"), ==, 0);
    aliases = fdt_add_subnode(fdt, 0, "aliases");
    g_assert_cmpint(aliases, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, aliases, "serial0",
                                      "/soc/serial@7e215040"), ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, aliases, "serial1",
                                      "/soc/serial@7e201000"), ==, 0);
    g_assert_cmpint(fdt_add_subnode(fdt, 0, "soc"), >=, 0);
    uart = fdt_add_subnode(fdt, fdt_path_offset(fdt, "/soc"),
                           "serial@7e215040");
    g_assert_cmpint(uart, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, uart, "compatible",
                                      "brcm,bcm2835-aux-uart"), ==, 0);
    uart = fdt_add_subnode(fdt, fdt_path_offset(fdt, "/soc"),
                           "serial@7e201000");
    g_assert_cmpint(uart, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, uart, "compatible",
                                      "arm,pl011"), ==, 0);
    avs = fdt_add_subnode(fdt, fdt_path_offset(fdt, "/soc"),
                          "avs-monitor@7d5d2000");
    g_assert_cmpint(avs, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, avs, "compatible",
                                      "brcm,bcm2711-avs-monitor"), ==, 0);
    thermal = fdt_add_subnode(fdt, avs, "thermal");
    g_assert_cmpint(thermal, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, thermal, "compatible",
                                      "brcm,bcm2711-thermal"), ==, 0);
    genet = fdt_add_subnode(fdt, fdt_path_offset(fdt, "/soc"),
                            "ethernet@7d580000");
    g_assert_cmpint(genet, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, genet, "compatible",
                                      "brcm,bcm2711-genet-v5"), ==, 0);
    memory = fdt_add_subnode(fdt, 0, "memory@0");
    g_assert_cmpint(memory, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, memory, "device_type", "memory"),
                    ==, 0);
    peripheral = fdt_add_subnode(fdt, 0, "test-peripheral");
    g_assert_cmpint(peripheral, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, peripheral, "phandle", 1), ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, peripheral, "status", "disabled"),
                    ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, peripheral, "test-value", 7), ==, 0);
    overrides = fdt_add_subnode(fdt, 0, "__overrides__");
    g_assert_cmpint(overrides, >=, 0);
    fdt_set_override(fdt, overrides, "base_enable", 1, "status");
    fdt_set_override(fdt, overrides, "base_value", 1, "test-value:0");
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
static uint8_t *make_bootloader_nvram_device_tree(size_t *size)
{
    g_autofree uint8_t *base = make_device_tree(size);
    size_t capacity = *size + 2048;
    uint8_t *fdt = g_malloc0(capacity);
    int reserved;
    int nvram;
    int system;
    int bootloader;

    g_assert_cmpint(fdt_open_into(base, fdt, capacity), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, 0, "serial-number",
                        "ffffffffffffffff"), ==, 0);
    g_assert_cmpint(fdt_setprop_u64(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "kaslr-seed",
                        UINT64_C(0xfeedfacecafebeef)), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "rpi-min-boot-ver", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "rpi-sdram-size-gbit", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "rpi-boardrev-ext", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u64(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "linux,initrd-start",
                        UINT64_C(0xfeedface00000000)), ==, 0);
    g_assert_cmpint(fdt_setprop_u64(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "linux,initrd-end",
                        UINT64_C(0xfeedface00001000)), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "os_prefix", "stale/"), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, fdt_path_offset(fdt, "/chosen"),
                        "overlay_prefix", "stale-overlays/"), ==, 0);
    bootloader = fdt_add_subnode(
        fdt, fdt_path_offset(fdt, "/chosen"), "bootloader");
    g_assert_cmpint(bootloader, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "boot-mode", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "rsts", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "pm_rsts", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "build_timestamp",
                        0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "update_timestamp",
                        0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "capabilities",
                        0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, bootloader, "version", "stale-version"), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, bootloader, "signed", 0xdeadbeef), ==, 0);
    {
        int usb = fdt_add_subnode(fdt, bootloader, "usb");

        g_assert_cmpint(usb, >=, 0);
        g_assert_cmpint(fdt_setprop_u32(
                            fdt, usb, "usb-version", 0xdeadbeef), ==, 0);
        g_assert_cmpint(fdt_setprop_u32(
                            fdt, usb, "route-string", 0xdeadbeef), ==, 0);
        g_assert_cmpint(fdt_setprop_u32(
                            fdt, usb, "root-hub-port-number",
                            0xdeadbeef), ==, 0);
        g_assert_cmpint(fdt_setprop_u32(
                            fdt, usb, "lun", 0xdeadbeef), ==, 0);
    }
    system = fdt_add_subnode(fdt, 0, "system");
    g_assert_cmpint(system, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, system, "linux,revision", 0xdeadbeef), ==, 0);
    g_assert_cmpint(fdt_setprop_u64(
                        fdt, system, "linux,serial",
                        UINT64_C(0xfeedfacecafebeef)), ==, 0);
    reserved = fdt_add_subnode(fdt, 0, "reserved-memory");
    g_assert_cmpint(reserved, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, reserved, "#address-cells", 2), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(
                        fdt, reserved, "#size-cells", 1), ==, 0);
    g_assert_cmpint(fdt_setprop(fdt, reserved, "ranges", NULL, 0), ==, 0);
    nvram = fdt_add_subnode(fdt, reserved, "nvram@0");
    g_assert_cmpint(nvram, >=, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, nvram, "compatible",
                        "raspberrypi,bootloader-config"), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, nvram, "status", "disabled"), ==, 0);
    nvram = fdt_add_subnode(fdt, reserved, "nvram@1");
    g_assert_cmpint(nvram, >=, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, nvram, "compatible",
                        "raspberrypi,bootloader-public-key"), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, nvram, "status", "disabled"), ==, 0);
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
static uint8_t *make_wireless_device_tree(size_t *size)
{
    static const char compatible[] =
        "brcm,bcm2835-mmc\0brcm,bcm2835-sdhci";
    static const struct {
        const char *name;
        const char *compatible;
    } policy_nodes[] = {
        { "local-intc@7ef00100", "brcm,bcm2711-l2-intc" },
        { "dvp@7e806000", "brcm,brcm2711-dvp" },
        { "i2c@7ef04500", "brcm,bcm2711-hdmi-i2c" },
    };
    g_autofree uint8_t *base = make_device_tree(size);
    size_t capacity = *size + 2048;
    uint8_t *fdt = g_malloc0(capacity);
    int bluetooth;
    int uart;
    int wireless;

    g_assert_cmpint(fdt_open_into(base, fdt, capacity), ==, 0);
    uart = fdt_path_offset(fdt, "/soc/serial@7e201000");
    g_assert_cmpint(uart, >=, 0);
    bluetooth = fdt_add_subnode(fdt, uart, "bluetooth");
    g_assert_cmpint(bluetooth, >=, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, bluetooth, "compatible",
                        "brcm,bcm43438-bt"), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, bluetooth, "status", "okay"), ==, 0);
    wireless = fdt_add_subnode(fdt, fdt_path_offset(fdt, "/soc"),
                               "mmc@7e300000");
    g_assert_cmpint(wireless, >=, 0);
    g_assert_cmpint(fdt_setprop(
                        fdt, wireless, "compatible", compatible,
                        sizeof(compatible)), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, wireless, "status", "okay"), ==, 0);
    for (size_t index = 0; index < ARRAY_SIZE(policy_nodes); index++) {
        int node = fdt_add_subnode(
            fdt, fdt_path_offset(fdt, "/soc"), policy_nodes[index].name);

        g_assert_cmpint(node, >=, 0);
        g_assert_cmpint(fdt_setprop_string(
                            fdt, node, "compatible",
                            policy_nodes[index].compatible), ==, 0);
        /*
         * Start enabled to prove the final machine-policy pass wins over
         * firmware input and overlay state without deleting the real node.
         */
        g_assert_cmpint(fdt_setprop_string(
                            fdt, node, "status", "okay"), ==, 0);
    }
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
static uint8_t *make_device_tree_overlay(size_t *size)
{
    size_t capacity = 4096;
    uint8_t *fdt = g_malloc0(capacity);
    int fragment;
    int dormant_fragment;
    int bootargs_fragment;
    int direct_bootargs_fragment;
    int symbol_bootargs_fragment;
    int intra_parent_fragment;
    int intra_patch_fragment;
    int intra_deep_fragment;
    int overlay;
    int dormant;
    int device;
    int linked_device;
    int reg_device;
    int name_device;
    int overrides;
    int fixups;
    int symbols;
    int exports;
    int local_fixups;
    int local_fragment_fixups;
    int local_deep_fixups;
    int local_overrides;
    size_t linked_cell_offset;
    size_t external_cell_offset;
    FdtLookupCellOffsets lookup_local_offsets;
    FdtLookupCellOffsets lookup_external_offsets;
    fdt32_t linked_offsets[2];
    fdt32_t external_offsets[1];
    fdt32_t lookup_offsets[2];
    uint8_t chosen_fixups[320];
    size_t chosen_fixups_length;
    int written;
    const uint8_t initial_bytes[2] = { 0, 0 };
    const uint8_t initial_mac[6] = { 0, 0, 0, 0, 0, 0 };
    static const char chosen_target_fixup[] = "/fragment@4:target:0";

    g_assert_cmpint(fdt_create_empty_tree(fdt, capacity), ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, 0, "compatible",
                                      "brcm,bcm2835"), ==, 0);
    fragment = fdt_add_subnode(fdt, 0, "fragment@0");
    g_assert_cmpint(fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, fragment, "target-path", "/"),
                    ==, 0);
    overlay = fdt_add_subnode(fdt, fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    device = fdt_add_subnode(fdt, overlay, "overlay-device");
    g_assert_cmpint(device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "phandle", 1), ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, device, "status", "disabled"),
                    ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "test-value", 1), ==, 0);
    g_assert_cmpint(fdt_setprop(fdt, device, "test-bytes", initial_bytes,
                                sizeof(initial_bytes)), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "test-u16", 0), ==, 0);
    g_assert_cmpint(fdt_setprop_u64(fdt, device, "test-u64", 0), ==, 0);
    g_assert_cmpint(fdt_setprop(fdt, device, "test-mac", initial_mac,
                                sizeof(initial_mac)), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "test-link", 0), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "external-link", 0), ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, device, "lookup-name", ""),
                    ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, device, "lookup-default", ""),
                    ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, device, "lookup-pass", ""),
                    ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "lookup-local", 0), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "lookup-external", 0), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "multi-a", 0), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "multi-b", 0), ==, 0);
    linked_device = fdt_add_subnode(fdt, overlay, "linked-device");
    g_assert_cmpint(linked_device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, linked_device, "phandle", 5), ==, 0);
    reg_device = fdt_add_subnode(fdt, overlay, "reg-device@0");
    g_assert_cmpint(reg_device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, reg_device, "phandle", 6), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, reg_device, "reg", 0), ==, 0);
    name_device = fdt_add_subnode(fdt, overlay, "name-device");
    g_assert_cmpint(name_device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, name_device, "phandle", 7), ==, 0);
    dormant_fragment = fdt_add_subnode(fdt, 0, "fragment@1");
    g_assert_cmpint(dormant_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, dormant_fragment, "target-path",
                                      "/"), ==, 0);
    dormant = fdt_add_subnode(fdt, dormant_fragment, "__dormant__");
    g_assert_cmpint(dormant, >=, 0);
    g_assert_cmpint(fdt_add_subnode(fdt, dormant, "forced-fragment"), >=, 0);
    bootargs_fragment = fdt_add_subnode(fdt, 0, "fragment@2");
    g_assert_cmpint(bootargs_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, bootargs_fragment, "target-path",
                                      "/chosen"), ==, 0);
    overlay = fdt_add_subnode(fdt, bootargs_fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, overlay, "bootargs",
                                      "overlay-flag=1"), ==, 0);
    direct_bootargs_fragment = fdt_add_subnode(fdt, 0, "fragment@3");
    g_assert_cmpint(direct_bootargs_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, direct_bootargs_fragment, "target",
                                   2), ==, 0);
    overlay = fdt_add_subnode(fdt, direct_bootargs_fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, overlay, "bootargs",
                                      "direct-flag=1"), ==, 0);
    symbol_bootargs_fragment = fdt_add_subnode(fdt, 0, "fragment@4");
    g_assert_cmpint(symbol_bootargs_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, symbol_bootargs_fragment, "target",
                                   UINT32_MAX), ==, 0);
    overlay = fdt_add_subnode(fdt, symbol_bootargs_fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, overlay, "bootargs",
                                      "symbol-flag=1"), ==, 0);
    intra_parent_fragment = fdt_add_subnode(fdt, 0, "fragment@5");
    g_assert_cmpint(intra_parent_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, intra_parent_fragment, "target-path", "/"),
                    ==, 0);
    overlay = fdt_add_subnode(fdt, intra_parent_fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    device = fdt_add_subnode(fdt, overlay, "intra-parent");
    g_assert_cmpint(device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "phandle", 8), ==, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, device, "first-pass",
                                      "parent"), ==, 0);
    /*
     * fdt_add_subnode inserts first, so fragment@6 precedes fragment@5 in
     * wire order.  Generic libfdt fails that order; Pi firmware's first pass
     * resolves it.
     */
    intra_patch_fragment = fdt_add_subnode(fdt, 0, "fragment@6");
    g_assert_cmpint(intra_patch_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, intra_patch_fragment, "target", 8),
                    ==, 0);
    overlay = fdt_add_subnode(fdt, intra_patch_fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, overlay, "second-pass",
                                      "patched"), ==, 0);
    device = fdt_add_subnode(fdt, overlay, "intra-child");
    g_assert_cmpint(device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "phandle", 9), ==, 0);
    /*
     * fragment@7 precedes both dependencies in wire order and targets the
     * node created by fragment@6, which itself targets fragment@5.
     */
    intra_deep_fragment = fdt_add_subnode(fdt, 0, "fragment@7");
    g_assert_cmpint(intra_deep_fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, intra_deep_fragment, "target", 9),
                    ==, 0);
    overlay = fdt_add_subnode(fdt, intra_deep_fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, overlay, "third-pass",
                                      "deep-patched"), ==, 0);
    g_assert_cmpint(fdt_add_subnode(fdt, overlay, "deep-child"), >=, 0);

    symbols = fdt_add_subnode(fdt, 0, "__symbols__");
    g_assert_cmpint(symbols, >=, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, symbols, "private_label",
                        "/fragment@0/__overlay__/linked-device"), ==, 0);
    g_assert_cmpint(fdt_setprop_string(
                        fdt, symbols, "public_label",
                        "/fragment@5/__overlay__/intra-parent"), ==, 0);
    exports = fdt_add_subnode(fdt, 0, "__exports__");
    g_assert_cmpint(exports, >=, 0);
    g_assert_cmpint(fdt_setprop(fdt, exports, "public_label", NULL, 0),
                    ==, 0);
    fixups = fdt_add_subnode(fdt, 0, "__fixups__");
    g_assert_cmpint(fixups, >=, 0);
    overrides = fdt_add_subnode(fdt, 0, "__overrides__");
    g_assert_cmpint(overrides, >=, 0);
    fdt_set_override(fdt, overrides, "enable", 1, "status");
    fdt_set_override(fdt, overrides, "value", 1, "test-value:0");
    fdt_set_override(fdt, overrides, "byte_value", 1, "test-bytes.1");
    fdt_set_override(fdt, overrides, "word_value", 1, "test-u16;2");
    fdt_set_override(fdt, overrides, "wide_value", 1, "test-u64#0");
    fdt_set_override(fdt, overrides, "mac_value", 1, "test-mac[");
    fdt_set_override(fdt, overrides, "bool_value", 1, "test-bool?");
    fdt_set_override(fdt, overrides, "invert_value", 1, "test-inverted!");
    fdt_set_override(fdt, overrides, "force_fragment", 0, "+1");
    fdt_set_override(fdt, overrides, "malformed_cell", 1,
                     "test-u16:0=");
    fdt_set_override(fdt, overrides, "lookup_nomatch", 1,
                     "lookup-name{a=alpha}");
    fdt_set_override(fdt, overrides, "lookup_bad_tail", 1,
                     "lookup-name{a=alpha}tail");
    fdt_set_override(fdt, overrides, "lookup_truncated_cell", 1,
                     "lookup-local:0{local=");
    fdt_set_override(
        fdt, overrides, "lookup_name", 1,
        "lookup-name{a=alpha,b='bravo charlie',d,e,='tango uniform'}");
    fdt_set_override(fdt, overrides, "lookup_default", 1,
                     "lookup-default{a=alpha,=fallback}");
    fdt_set_override(fdt, overrides, "lookup_pass", 1,
                     "lookup-pass{a=alpha,,}");
    fdt_set_override_two_targets(fdt, overrides, "multi_value",
                                 1, "multi-a:0", 1, "multi-b:0");
    fdt_set_override_two_targets(fdt, overrides, "multi_bool",
                                 1, "multi-bool-a?", 1, "multi-bool-b?");
    g_assert_cmpint(fdt_setprop(fdt, overrides, "noop", NULL, 0), ==, 0);
    fdt_set_override(fdt, overrides, "reg_value", 6, "reg:0");
    fdt_set_override(fdt, overrides, "name_value", 7, "name");
    linked_cell_offset = fdt_set_override_cell(
        fdt, overrides, "linked", 1, "test-link:0=", 5);
    external_cell_offset = fdt_set_override_cell(
        fdt, overrides, "external_link", 1, "external-link:0=", UINT32_MAX);
    lookup_local_offsets = fdt_set_override_lookup_cells(
        fdt, overrides, "lookup_local", 1, "lookup-local:0", 5,
        UINT32_MAX);
    lookup_external_offsets = fdt_set_override_lookup_cells(
        fdt, overrides, "lookup_external", 1, "lookup-external:0", 5,
        UINT32_MAX);

    local_fixups = fdt_add_subnode(fdt, 0, "__local_fixups__");
    g_assert_cmpint(local_fixups, >=, 0);
    local_fragment_fixups = fdt_add_subnode(
        fdt, local_fixups, "fragment@6");
    g_assert_cmpint(local_fragment_fixups, >=, 0);
    external_offsets[0] = cpu_to_fdt32(0);
    g_assert_cmpint(fdt_setprop(
                        fdt, local_fragment_fixups, "target",
                        external_offsets, sizeof(external_offsets)), ==, 0);
    local_deep_fixups = fdt_add_subnode(fdt, local_fixups, "fragment@7");
    g_assert_cmpint(local_deep_fixups, >=, 0);
    g_assert_cmpint(fdt_setprop(fdt, local_deep_fixups, "target",
                                external_offsets,
                                sizeof(external_offsets)), ==, 0);
    local_overrides = fdt_add_subnode(fdt, local_fixups, "__overrides__");
    g_assert_cmpint(local_overrides, >=, 0);
    linked_offsets[0] = cpu_to_fdt32(0);
    linked_offsets[1] = cpu_to_fdt32(linked_cell_offset);
    g_assert_cmpint(fdt_setprop(fdt, local_overrides, "linked",
                                linked_offsets, sizeof(linked_offsets)), ==, 0);
    external_offsets[0] = cpu_to_fdt32(0);
    g_assert_cmpint(fdt_setprop(fdt, local_overrides, "external_link",
                                external_offsets,
                                sizeof(external_offsets)), ==, 0);
    lookup_offsets[0] = cpu_to_fdt32(0);
    lookup_offsets[1] = cpu_to_fdt32(lookup_local_offsets.local);
    g_assert_cmpint(fdt_setprop(fdt, local_overrides, "lookup_local",
                                lookup_offsets, sizeof(lookup_offsets)), ==, 0);
    lookup_offsets[1] = cpu_to_fdt32(lookup_external_offsets.local);
    g_assert_cmpint(fdt_setprop(fdt, local_overrides, "lookup_external",
                                lookup_offsets, sizeof(lookup_offsets)), ==, 0);
    g_assert_cmpint(fdt_setprop(fdt, local_overrides, "reg_value",
                                external_offsets,
                                sizeof(external_offsets)), ==, 0);
    g_assert_cmpint(fdt_setprop(fdt, local_overrides, "name_value",
                                external_offsets,
                                sizeof(external_offsets)), ==, 0);

    memcpy(chosen_fixups, chosen_target_fixup, sizeof(chosen_target_fixup));
    written = g_snprintf(
        (char *)chosen_fixups + sizeof(chosen_target_fixup),
        sizeof(chosen_fixups) - sizeof(chosen_target_fixup),
        "/__overrides__:external_link:%zu", external_cell_offset);
    g_assert_cmpint(written, >, 0);
    g_assert_cmpuint((size_t)written, <,
                     sizeof(chosen_fixups) - sizeof(chosen_target_fixup));
    chosen_fixups_length = sizeof(chosen_target_fixup) + written + 1;
    written = g_snprintf(
        (char *)chosen_fixups + chosen_fixups_length,
        sizeof(chosen_fixups) - chosen_fixups_length,
        "/__overrides__:lookup_local:%zu", lookup_local_offsets.external);
    g_assert_cmpint(written, >, 0);
    g_assert_cmpuint((size_t)written, <,
                     sizeof(chosen_fixups) - chosen_fixups_length);
    chosen_fixups_length += written + 1;
    written = g_snprintf(
        (char *)chosen_fixups + chosen_fixups_length,
        sizeof(chosen_fixups) - chosen_fixups_length,
        "/__overrides__:lookup_external:%zu",
        lookup_external_offsets.external);
    g_assert_cmpint(written, >, 0);
    g_assert_cmpuint((size_t)written, <,
                     sizeof(chosen_fixups) - chosen_fixups_length);
    chosen_fixups_length += written + 1;
    fixups = fdt_path_offset(fdt, "/__fixups__");
    g_assert_cmpint(fixups, >=, 0);
    g_assert_cmpint(fdt_setprop(fdt, fixups, "chosen_label", chosen_fixups,
                                chosen_fixups_length), ==, 0);
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
static uint8_t *make_export_consumer_overlay(size_t *size)
{
    static const char fixup[] = "/fragment@0:target:0";
    size_t capacity = 2048;
    uint8_t *fdt = g_malloc0(capacity);
    int fragment;
    int overlay;
    int fixups;

    g_assert_cmpint(fdt_create_empty_tree(fdt, capacity), ==, 0);
    fragment = fdt_add_subnode(fdt, 0, "fragment@0");
    g_assert_cmpint(fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, fragment, "target", UINT32_MAX),
                    ==, 0);
    overlay = fdt_add_subnode(fdt, fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, overlay, "export-consumer",
                                      "applied"), ==, 0);
    fixups = fdt_add_subnode(fdt, 0, "__fixups__");
    g_assert_cmpint(fixups, >=, 0);
    g_assert_cmpint(fdt_setprop(fdt, fixups, "public_label",
                                fixup, sizeof(fixup)), ==, 0);
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
uint16_t test_hat_crc16(const uint8_t *data, size_t size)
{
    uint16_t out = 0;

    for (size_t byte = 0; byte < size; byte++) {
        for (unsigned int bit = 0; bit < 8; bit++) {
            bool high = out & 0x8000;

            out = (out << 1) | ((data[byte] >> bit) & 1);
            if (high) {
                out ^= 0x8005;
            }
        }
    }
    for (unsigned int bit = 0; bit < 16; bit++) {
        bool high = out & 0x8000;

        out <<= 1;
        if (high) {
            out ^= 0x8005;
        }
    }
    out = (out & 0x5555) << 1 | (out >> 1 & 0x5555);
    out = (out & 0x3333) << 2 | (out >> 2 & 0x3333);
    out = (out & 0x0f0f) << 4 | (out >> 4 & 0x0f0f);
    return out << 8 | out >> 8;
}
static void test_hat_add_atom(GByteArray *image, uint16_t type,
                              uint16_t count, const void *payload,
                              size_t payload_size)
{
    uint8_t header[8];
    uint8_t crc_bytes[2];
    uint16_t crc;
    size_t start = image->len;

    stw_le_p(header, type);
    stw_le_p(header + 2, count);
    stl_le_p(header + 4, payload_size + 2);
    g_byte_array_append(image, header, sizeof(header));
    g_byte_array_append(image, payload, payload_size);
    crc = test_hat_crc16(image->data + start,
                         sizeof(header) + payload_size);
    stw_le_p(crc_bytes, crc);
    g_byte_array_append(image, crc_bytes, sizeof(crc_bytes));
}
uint8_t *make_hat_overlay(size_t *size)
{
    size_t capacity = 2048;
    uint8_t *fdt = g_malloc0(capacity);
    int fragment;
    int overlay;
    int device;
    int overrides;

    g_assert_cmpint(fdt_create_empty_tree(fdt, capacity), ==, 0);
    fragment = fdt_add_subnode(fdt, 0, "fragment@0");
    g_assert_cmpint(fragment, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, fragment, "target-path", "/"),
                    ==, 0);
    overlay = fdt_add_subnode(fdt, fragment, "__overlay__");
    g_assert_cmpint(overlay, >=, 0);
    device = fdt_add_subnode(fdt, overlay, "hat-device");
    g_assert_cmpint(device, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "phandle", 1), ==, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, device, "rate", 1), ==, 0);
    overrides = fdt_add_subnode(fdt, 0, "__overrides__");
    g_assert_cmpint(overrides, >=, 0);
    fdt_set_override(fdt, overrides, "rate", 1, "rate:0");
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
uint8_t *make_hat_eeprom(const uint8_t *dt, size_t dt_size,
                                size_t *size)
{
    static const char vendor[] = "QEMU Labs";
    static const char product[] = "Virtual HAT";
    static const uint8_t custom[] = { 0xde, 0xad, 0xbe, 0xef };
    uint8_t identity[22 + sizeof(vendor) - 1 + sizeof(product) - 1] = { 0 };
    uint8_t gpio[30] = { 0 };
    GByteArray *image = g_byte_array_sized_new(4096);
    uint8_t header[12] = { 'R', '-', 'P', 'i', 1, 0 };
    unsigned int atoms = 0;

    for (unsigned int i = 0; i < 16; i++) {
        identity[i] = i + 1;
    }
    stw_le_p(identity + 16, 0x1234);
    stw_le_p(identity + 18, 0x5678);
    identity[20] = sizeof(vendor) - 1;
    identity[21] = sizeof(product) - 1;
    memcpy(identity + 22, vendor, sizeof(vendor) - 1);
    memcpy(identity + 22 + sizeof(vendor) - 1,
           product, sizeof(product) - 1);
    gpio[0] = 4 | (1 << 4) | (2 << 6);
    gpio[1] = 2;
    gpio[2 + 4] = 0x80 | (1 << 5) | 4;
    gpio[2 + 17] = 0x80 | (2 << 5) | 1;
    gpio[2 + 27] = 0x80 | (3 << 5);
    g_byte_array_append(image, header, sizeof(header));
    test_hat_add_atom(image, 1, atoms++, identity, sizeof(identity));
    test_hat_add_atom(image, 2, atoms++, gpio, sizeof(gpio));
    if (dt) {
        test_hat_add_atom(image, 3, atoms++, dt, dt_size);
    }
    test_hat_add_atom(image, 4, atoms++, custom, sizeof(custom));
    stw_le_p(image->data + 6, atoms);
    stl_le_p(image->data + 8, image->len);
    *size = image->len;
    return g_byte_array_free(image, false);
}
static uint8_t *make_device_tree_overlay_export_variant(
    size_t *size, TestOverlayExportVariant variant)
{
    g_autofree uint8_t *packed = make_device_tree_overlay(size);
    size_t capacity = *size + 1024;
    uint8_t *fdt = g_malloc0(capacity);
    int exports;
    int symbols;

    g_assert_cmpint(fdt_open_into(packed, fdt, capacity), ==, 0);
    exports = fdt_path_offset(fdt, "/__exports__");
    symbols = fdt_path_offset(fdt, "/__symbols__");
    g_assert_cmpint(exports, >=, 0);
    g_assert_cmpint(symbols, >=, 0);
    switch (variant) {
    case TEST_EXPORT_VALID:
        break;
    case TEST_EXPORT_NONEMPTY:
        g_assert_cmpint(fdt_setprop_string(
                            fdt, exports, "public_label", "invalid"), ==, 0);
        break;
    case TEST_EXPORT_MISSING_SYMBOL:
        g_assert_cmpint(fdt_setprop(
                            fdt, exports, "missing_label", NULL, 0), ==, 0);
        break;
    case TEST_EXPORT_COLLISION:
        g_assert_cmpint(fdt_setprop_string(
                            fdt, symbols, "chosen_label",
                            "/fragment@0/__overlay__/overlay-device"), ==, 0);
        g_assert_cmpint(fdt_setprop(
                            fdt, exports, "chosen_label", NULL, 0), ==, 0);
        break;
    case TEST_EXPORT_INTRA_CYCLE:
    {
        fdt32_t target_offset = cpu_to_fdt32(0);
        int fragment = fdt_path_offset(fdt, "/fragment@5");
        int fixups = fdt_path_offset(fdt, "/__local_fixups__");
        int fragment_fixups;

        g_assert_cmpint(fragment, >=, 0);
        g_assert_cmpint(fdt_delprop(fdt, fragment, "target-path"), ==, 0);
        g_assert_cmpint(fdt_setprop_u32(fdt, fragment, "target", 9), ==, 0);
        g_assert_cmpint(fixups, >=, 0);
        fragment_fixups = fdt_add_subnode(fdt, fixups, "fragment@5");
        g_assert_cmpint(fragment_fixups, >=, 0);
        g_assert_cmpint(fdt_setprop(fdt, fragment_fixups, "target",
                                    &target_offset,
                                    sizeof(target_offset)), ==, 0);
        break;
    }
    }
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
static uint8_t *make_overlay_map(size_t *size)
{
    size_t capacity = 4096;
    uint8_t *fdt = g_malloc0(capacity);
    int node;

    g_assert_cmpint(fdt_create_empty_tree(fdt, capacity), ==, 0);
    node = fdt_add_subnode(fdt, 0, "vc4-kms-v3d");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpint(fdt_setprop_string(fdt, node, "bcm2711",
                                      "vc4-kms-v3d-pi4"), ==, 0);
    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    *size = fdt_totalsize(fdt);
    return g_realloc(fdt, *size);
}
void write_temp_image(const char *template, const uint8_t *image,
                             size_t size, char **path_out)
{
    int fd = g_file_open_tmp(template, path_out, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(qemu_write_full(fd, image, size), ==, size);
    close(fd);
}
void write_temp_text(const char *template, const char *text,
                            char **path_out)
{
    int fd = g_file_open_tmp(template, path_out, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(qemu_write_full(fd, text, strlen(text)), ==,
                    strlen(text));
    close(fd);
}
void finalize_edid_block(uint8_t edid[128])
{
    uint8_t checksum = 0;

    for (unsigned int index = 0; index < 127; index++) {
        checksum += edid[index];
    }
    edid[127] = -checksum;
}
void make_edid(uint8_t edid[128], const char manufacturer[3],
                      const char *product)
{
    static const uint8_t header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    };
    uint16_t vendor = 0;
    size_t product_length = strlen(product);

    g_assert_cmpuint(product_length, <=, 13);
    memset(edid, 0, 128);
    memcpy(edid, header, sizeof(header));
    for (unsigned int index = 0; index < 3; index++) {
        g_assert_true(manufacturer[index] >= 'A' &&
                      manufacturer[index] <= 'Z');
        vendor |= (manufacturer[index] - 'A' + 1) << (10 - index * 5);
    }
    stw_be_p(edid + 8, vendor);
    edid[18] = 1;
    edid[19] = 4;
    edid[54 + 3] = 0xfc;
    memset(edid + 54 + 5, ' ', 13);
    memcpy(edid + 54 + 5, product, product_length);
    finalize_edid_block(edid);
}
void make_timed_edid(uint8_t edid[128],
                            const char manufacturer[3],
                            uint16_t pixel_clock_10khz,
                            uint16_t hactive, uint16_t hblank,
                            uint16_t hfront, uint16_t hsync,
                            uint16_t vactive, uint16_t vblank,
                            uint16_t vfront, uint16_t vsync)
{
    uint8_t *desc = edid + 54;

    make_edid(edid, manufacturer, "TIMED");
    memcpy(edid + 72, desc, 18);
    memset(desc, 0, 18);
    stw_le_p(desc, pixel_clock_10khz);
    desc[2] = hactive;
    desc[3] = hblank;
    desc[4] = ((hactive >> 8) << 4) | (hblank >> 8);
    desc[5] = vactive;
    desc[6] = vblank;
    desc[7] = ((vactive >> 8) << 4) | (vblank >> 8);
    desc[8] = hfront;
    desc[9] = hsync;
    desc[10] = (vfront << 4) | vsync;
    desc[11] = ((hfront >> 8) << 6) | ((hsync >> 8) << 4) |
               ((vfront >> 4) << 2) | (vsync >> 4);
    desc[17] = 0x1e;
    finalize_edid_block(edid);
}
void overwrite_image(const char *path, const uint8_t *image,
                            size_t size)
{
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(qemu_write_full(fd, image, size), ==, size);
    g_assert_cmpint(fsync(fd), ==, 0);
    close(fd);
}
void fat16_init(Fat16Builder *builder)
{
    uint8_t *image = g_malloc0(SD_SIZE);
    uint8_t *partition = image + SD_PARTITION_LBA * 512;
    uint8_t *mbr_entry = image + 446;
    uint8_t *fat = partition + 512;

    image[510] = 0x55;
    image[511] = 0xaa;
    mbr_entry[4] = 0x0e;
    stl_le_p(mbr_entry + 8, SD_PARTITION_LBA);
    stl_le_p(mbr_entry + 12, SD_PARTITION_SECTORS);

    partition[0] = 0xeb;
    partition[1] = 0x3c;
    partition[2] = 0x90;
    memcpy(partition + 3, "QEMURPI ", 8);
    stw_le_p(partition + 11, 512);
    partition[13] = 1;
    stw_le_p(partition + 14, 1);
    partition[16] = 1;
    stw_le_p(partition + 17, FAT_ROOT_ENTRIES);
    stw_le_p(partition + 19, SD_PARTITION_SECTORS);
    partition[21] = 0xf8;
    stw_le_p(partition + 22, FAT_SECTORS);
    partition[510] = 0x55;
    partition[511] = 0xaa;
    stw_le_p(fat, 0xfff8);
    stw_le_p(fat + 2, 0xffff);

    builder->image = image;
    builder->next_cluster = 2;
    builder->directory_index = 0;
}
static void fat16_gpt_header(uint8_t *header, uint64_t current_lba,
                             uint64_t backup_lba, uint64_t entries_lba,
                             uint32_t entries_crc)
{
    memcpy(header, "EFI PART", 8);
    stl_le_p(header + 8, 0x00010000);
    stl_le_p(header + 12, 92);
    stq_le_p(header + 24, current_lba);
    stq_le_p(header + 32, backup_lba);
    stq_le_p(header + 40, 34);
    stq_le_p(header + 48, 16374);
    memcpy(header + 56, "QEMU-RPI-GPT-GID", 16);
    stq_le_p(header + 72, entries_lba);
    stl_le_p(header + 80, 32);
    stl_le_p(header + 84, 128);
    stl_le_p(header + 88, entries_crc);
    stl_le_p(header + 16, test_crc32(header, 92));
}
static void fat16_make_gpt(Fat16Builder *builder)
{
    uint8_t *image = builder->image;
    uint8_t *partition = image + SD_PARTITION_LBA * 512;
    uint8_t entries[32 * 128] = { 0 };
    uint8_t *mbr_entry = image + 446;
    uint32_t entries_crc;

    memset(image + 446, 0, 64);
    mbr_entry[4] = 0xee;
    stl_le_p(mbr_entry + 8, 1);
    stl_le_p(mbr_entry + 12, SD_SIZE / 512 - 1);
    stw_le_p(partition + 19, 16374 - SD_PARTITION_LBA + 1);

    memcpy(entries, (const uint8_t[]) {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
    }, 16);
    memcpy(entries + 16, "QEMU-RPI-BOOT-ID", 16);
    stq_le_p(entries + 32, SD_PARTITION_LBA);
    stq_le_p(entries + 40, 16374);
    entries_crc = test_crc32(entries, sizeof(entries));
    memcpy(image + 2 * 512, entries, sizeof(entries));
    memcpy(image + 16375 * 512, entries, sizeof(entries));
    fat16_gpt_header(image + 512, 1, 16383, 2, entries_crc);
    fat16_gpt_header(image + 16383 * 512, 16383, 1, 16375, entries_crc);
}
static uint16_t fat16_add_data(Fat16Builder *builder,
                               const uint8_t *contents, size_t size)
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *fat = partition + 512;
    uint8_t *data = partition + FAT_DATA_SECTOR * 512;
    unsigned clusters = MAX(DIV_ROUND_UP(size, 512), 1);
    uint16_t first_cluster = builder->next_cluster;

    for (unsigned i = 0; i < clusters; i++) {
        uint16_t cluster = builder->next_cluster++;
        uint16_t next = i + 1 == clusters ? 0xffff : cluster + 1;
        size_t offset = i * 512;
        size_t chunk = MIN((size_t)512, size - MIN(size, offset));

        stw_le_p(fat + cluster * 2, next);
        if (chunk) {
            memcpy(data + (cluster - 2) * 512, contents + offset, chunk);
        }
    }
    return first_cluster;
}
static void fat16_write_entry(uint8_t *entry, const char name[11],
                              uint8_t attributes, uint16_t first_cluster,
                              size_t size)
{
    memcpy(entry, name, 11);
    entry[11] = attributes;
    stw_le_p(entry + 26, first_cluster);
    stl_le_p(entry + 28, size);
}
void fat16_add_file(Fat16Builder *builder, const char name[11],
                           const uint8_t *contents, size_t size)
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *root = partition + (1 + FAT_SECTORS) * 512;
    uint8_t *entry = root + builder->directory_index++ * 32;
    uint16_t first_cluster = fat16_add_data(builder, contents, size);

    g_assert_cmpuint(builder->directory_index, <=, FAT_ROOT_ENTRIES);
    fat16_write_entry(entry, name, 0x20, first_cluster, size);
}
uint16_t fat16_add_fragmented_file(Fat16Builder *builder,
                                          const char name[11],
                                          const uint8_t *contents,
                                          size_t size)
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *fat = partition + 512;
    uint8_t *data = partition + FAT_DATA_SECTOR * 512;
    uint8_t *root = partition + (1 + FAT_SECTORS) * 512;
    uint8_t *entry = root + builder->directory_index++ * 32;
    unsigned clusters = MAX(DIV_ROUND_UP(size, 512), 1);
    uint16_t first_cluster = builder->next_cluster;

    g_assert_cmpuint(builder->directory_index, <=, FAT_ROOT_ENTRIES);
    for (unsigned int i = 0; i < clusters; i++) {
        uint16_t cluster = builder->next_cluster++;
        uint16_t next;
        size_t offset = i * 512;
        size_t chunk = MIN((size_t)512, size - MIN(size, offset));

        if (i + 1 == clusters) {
            next = 0xffff;
        } else {
            uint16_t gap = builder->next_cluster++;

            stw_le_p(fat + gap * 2, 0xffff);
            memset(data + (gap - 2) * 512, 0xa5, 512);
            next = builder->next_cluster;
        }
        stw_le_p(fat + cluster * 2, next);
        if (chunk) {
            memcpy(data + (cluster - 2) * 512, contents + offset, chunk);
        }
    }
    fat16_write_entry(entry, name, 0x20, first_cluster, size);
    return first_cluster;
}
void fat_variant_set_entry(FatVariantBuilder *builder,
                                  uint32_t cluster, uint32_t value)
{
    uint8_t *partition = builder->image + 512;
    uint8_t *fat = partition + builder->reserved_sectors * 512;

    if (builder->fat_type == 12) {
        size_t offset = cluster + cluster / 2;
        uint16_t entry = lduw_le_p(fat + offset);

        if (cluster & 1) {
            entry = (entry & 0x000f) | ((value & 0x0fff) << 4);
        } else {
            entry = (entry & 0xf000) | (value & 0x0fff);
        }
        stw_le_p(fat + offset, entry);
    } else {
        stl_le_p(fat + cluster * 4, value & 0x0fffffff);
    }
}
static uint8_t *fat_variant_cluster(FatVariantBuilder *builder,
                                    uint32_t cluster)
{
    uint8_t *partition = builder->image + 512;
    uint64_t sector = builder->data_start_sector +
                      (uint64_t)(cluster - 2) *
                      builder->sectors_per_cluster;

    return partition + sector * 512;
}
void fat_variant_init(FatVariantBuilder *builder,
                             unsigned int fat_type)
{
    uint8_t *partition;
    uint8_t *mbr_entry;

    memset(builder, 0, sizeof(*builder));
    builder->fat_type = fat_type;
    if (fat_type == 12) {
        builder->image_size = 8 * MiB;
        builder->reserved_sectors = 1;
        builder->fat_sectors = 6;
        builder->root_entries = 64;
        builder->root_sectors = 4;
        builder->sectors_per_cluster = 8;
        builder->root_cluster = 0;
        builder->next_cluster = 2;
    } else {
        g_assert_cmpuint(fat_type, ==, 32);
        builder->image_size = 64 * MiB;
        builder->reserved_sectors = 32;
        builder->fat_sectors = 1024;
        builder->root_entries = 0;
        builder->root_sectors = 0;
        builder->sectors_per_cluster = 1;
        builder->root_cluster = 2;
        builder->next_cluster = 3;
    }
    builder->partition_sectors = builder->image_size / 512 - 1;
    builder->data_start_sector = builder->reserved_sectors +
                                 builder->fat_sectors +
                                 builder->root_sectors;
    builder->image = g_malloc0(builder->image_size);
    partition = builder->image + 512;
    mbr_entry = builder->image + 446;

    builder->image[510] = 0x55;
    builder->image[511] = 0xaa;
    mbr_entry[4] = fat_type == 12 ? 0x01 : 0x0c;
    stl_le_p(mbr_entry + 8, 1);
    stl_le_p(mbr_entry + 12, builder->partition_sectors);

    partition[0] = 0xeb;
    partition[1] = 0x3c;
    partition[2] = 0x90;
    memcpy(partition + 3, "QEMURPI ", 8);
    stw_le_p(partition + 11, 512);
    partition[13] = builder->sectors_per_cluster;
    stw_le_p(partition + 14, builder->reserved_sectors);
    partition[16] = 1;
    stw_le_p(partition + 17, builder->root_entries);
    if (fat_type == 12) {
        stw_le_p(partition + 19, builder->partition_sectors);
        partition[21] = 0xf8;
        stw_le_p(partition + 22, builder->fat_sectors);
    } else {
        stl_le_p(partition + 32, builder->partition_sectors);
        stl_le_p(partition + 36, builder->fat_sectors);
        stl_le_p(partition + 44, builder->root_cluster);
    }
    partition[510] = 0x55;
    partition[511] = 0xaa;

    fat_variant_set_entry(builder, 0,
                          fat_type == 12 ? 0x0ff8 : 0x0ffffff8);
    fat_variant_set_entry(builder, 1,
                          fat_type == 12 ? 0x0fff : 0x0fffffff);
    if (fat_type == 32) {
        fat_variant_set_entry(builder, builder->root_cluster, 0x0fffffff);
    }
}
uint32_t fat_variant_add_file(FatVariantBuilder *builder,
                                     const char name[11],
                                     const uint8_t *contents, size_t size,
                                     bool fragmented)
{
    size_t cluster_bytes = builder->sectors_per_cluster * 512;
    unsigned int clusters = MAX(DIV_ROUND_UP(size, cluster_bytes), 1);
    uint32_t first_cluster = builder->next_cluster;
    uint8_t *root;
    uint8_t *entry;

    if (builder->fat_type == 12) {
        uint8_t *partition = builder->image + 512;

        root = partition +
               (builder->reserved_sectors + builder->fat_sectors) * 512;
    } else {
        root = fat_variant_cluster(builder, builder->root_cluster);
    }
    entry = root + builder->directory_index++ * 32;
    g_assert_cmpuint(builder->directory_index,
                     <=, cluster_bytes / 32);

    for (unsigned int i = 0; i < clusters; i++) {
        uint32_t cluster = builder->next_cluster++;
        uint32_t next;
        size_t offset = (size_t)i * cluster_bytes;
        size_t chunk = MIN(cluster_bytes, size - MIN(size, offset));

        if (i + 1 == clusters) {
            next = builder->fat_type == 12 ? 0x0fff : 0x0fffffff;
        } else if (fragmented) {
            uint32_t gap = builder->next_cluster++;

            fat_variant_set_entry(
                builder, gap,
                builder->fat_type == 12 ? 0x0fff : 0x0fffffff);
            memset(fat_variant_cluster(builder, gap), 0xa5, cluster_bytes);
            next = builder->next_cluster;
        } else {
            next = builder->next_cluster;
        }
        fat_variant_set_entry(builder, cluster, next);
        if (chunk) {
            memcpy(fat_variant_cluster(builder, cluster),
                   contents + offset, chunk);
        }
    }

    fat16_write_entry(entry, name, 0x20, first_cluster, size);
    if (builder->fat_type == 32) {
        stw_le_p(entry + 20, first_cluster >> 16);
    }
    return first_cluster;
}
static uint8_t fat16_short_checksum(const char name[11])
{
    uint8_t checksum = 0;

    for (unsigned int i = 0; i < 11; i++) {
        checksum = ((checksum & 1) << 7) + (checksum >> 1) + name[i];
    }
    return checksum;
}
static void fat16_add_lfn_entries(uint8_t *directory, unsigned int *index,
                                  const char *long_name,
                                  const char short_name[11])
{
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
    };
    size_t length = strlen(long_name);
    unsigned int entries = DIV_ROUND_UP(length + 1, 13);
    uint8_t checksum = fat16_short_checksum(short_name);

    g_assert_cmpuint(entries, <=, 20);
    for (unsigned int sequence = entries; sequence > 0; sequence--) {
        uint8_t *entry = directory + (*index)++ * 32;

        memset(entry, 0xff, 32);
        entry[0] = sequence | (sequence == entries ? 0x40 : 0);
        entry[11] = 0x0f;
        entry[12] = 0;
        entry[13] = checksum;
        stw_le_p(entry + 26, 0);
        for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++) {
            size_t character = (sequence - 1) * 13 + i;
            uint16_t value = character < length ? long_name[character] :
                             character == length ? 0 : 0xffff;

            stw_le_p(entry + offsets[i], value);
        }
    }
}
void fat16_add_long_file(Fat16Builder *builder, const char *long_name,
                                const char short_name[11],
                                const uint8_t *contents, size_t size)
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *root = partition + (1 + FAT_SECTORS) * 512;

    fat16_add_lfn_entries(root, &builder->directory_index, long_name,
                          short_name);
    fat16_add_file(builder, short_name, contents, size);
}
static uint16_t fat16_add_directory(Fat16Builder *builder,
                                    const char name[11])
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *root = partition + (1 + FAT_SECTORS) * 512;
    uint8_t *entry = root + builder->directory_index++ * 32;
    uint8_t empty[512] = { 0 };
    uint16_t cluster = fat16_add_data(builder, empty, sizeof(empty));

    g_assert_cmpuint(builder->directory_index, <=, FAT_ROOT_ENTRIES);
    fat16_write_entry(entry, name, 0x10, cluster, 0);
    return cluster;
}
static void fat16_add_file_to_directory(Fat16Builder *builder,
                                        uint16_t directory_cluster,
                                        unsigned int *directory_index,
                                        const char name[11],
                                        const uint8_t *contents, size_t size)
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *data = partition + FAT_DATA_SECTOR * 512;
    uint8_t *directory = data + (directory_cluster - 2) * 512;
    uint8_t *entry = directory + (*directory_index)++ * 32;
    uint16_t first_cluster = fat16_add_data(builder, contents, size);

    g_assert_cmpuint(*directory_index, <=, 16);
    fat16_write_entry(entry, name, 0x20, first_cluster, size);
}
static void fat16_add_long_file_to_directory(
    Fat16Builder *builder, uint16_t directory_cluster,
    unsigned int *directory_index, const char *long_name,
    const char short_name[11], const uint8_t *contents, size_t size)
{
    uint8_t *partition = builder->image + SD_PARTITION_LBA * 512;
    uint8_t *data = partition + FAT_DATA_SECTOR * 512;
    uint8_t *directory = data + (directory_cluster - 2) * 512;

    fat16_add_lfn_entries(directory, directory_index, long_name, short_name);
    fat16_add_file_to_directory(builder, directory_cluster, directory_index,
                                short_name, contents, size);
}
uint8_t *make_secure_inner_boot_image(size_t *image_size)
{
    static const uint8_t start[] = "QEMU signed start4 fixture";
    static const uint8_t fixup[] = "QEMU signed fixup4 fixture";
    static const uint8_t config[] = "arm_64bit=1\n";
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    uint8_t *image;
    size_t device_tree_size;
    Fat16Builder builder;

    device_tree = make_device_tree(&device_tree_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF", start, sizeof(start));
    fat16_add_file(&builder, "FIXUP4  DAT", fixup, sizeof(fixup));
    fat16_add_file(&builder, "KERNEL8 IMG", kernel, TEST_KERNEL_SIZE);
    fat16_add_long_file(&builder, "bcm2711-rpi-4-b.dtb", "BCM271~1DTB",
                        device_tree, device_tree_size);
    fat16_add_file(&builder, "CONFIG  TXT", config, sizeof(config));

    *image_size = 4 * 1024 * 1024;
    image = g_memdup2(builder.image + SD_PARTITION_LBA * 512, *image_size);
    stw_le_p(image + 19, *image_size / 512);
    g_free(builder.image);
    return image;
}
char *make_secure_boot_signature(void)
{
    static const char signature_hex[] =
        "2e8c02e9ef9bc6a91de3a8e500bc1fde63374f3d0301e9ad10243f66e036e36"
        "a9cba0a57425630bcd45236b92f9d78e958398c045d02a7fb3ac69b49b4a373d"
        "8731b0bdf8bd09f2b134184e184dc9662b4460944e824af027b8d7f1cb7a7be5"
        "7c6a00b7c4485a8f4d84e80647e19433c69d46055f9d6b5ee2f83ad58faa90e8"
        "cddb135e968ae375bab7b5f0832f986e44212224f2c2f7d600fdbbf3973602d6b"
        "31be6a8a6505de1079f88ec8f0b65f61de3a075a9d3a6e34143b8ba35105ac35"
        "0a2020b10ae922779865b28259856783034e0cb1bc444377e6760f6ac809b70a74"
        "726c66aa923042471e9e454e67e8a193f7dac61644b39ed4e1c5cffd8e6341";

    return g_strdup_printf(
        "c9b2b839d858258c94c37a3039c809ef6be59207a1bbb1afb977d7d65373e788\n"
        "ts: 1\n"
        "rsa2048: %s\n", signature_hex);
}
char *make_secure_nvme_boot_signature(void)
{
    static const char signature_hex[] =
        "3b3cdcddefabfbc37e7b1ba7e57afdd5ec49fb59ab69caae0d49c4b583cfd32e"
        "1ceea3acd958295478eeb406a8632f71bb01e0cd7f75c6c7ef27f549375cfa9c"
        "8052dccfd0bd6af36b454e3f2794a385985bf4738e18847f45aa8747686c8c8f"
        "5c72e8995b77234a0cd05ecf130d31858e7bd0647f7d1e4c907cb60ed1c62f69"
        "1716a1c059697ab303de1450080583f3b365ccd6626a8ac45876080776ba29635"
        "ecfa1d6cd84b099b58401e4745cd639ed5d05f1228fd39695fc6d6ab112b0eca"
        "6ecc4754c034e9b9c68c85f33abb6cc233f108c637a7b97d08def877aa9e63c"
        "6bc4d85ae2a1645813f2410e6dd60f1c4e0166885458411a286b1de394864966";

    return g_strdup_printf(
        "c9b2b839d858258c94c37a3039c809ef6be59207a1bbb1afb977d7d65373e788\n"
        "ts: 1\n"
        "rsa2048: %s\n", signature_hex);
}
char *make_dns_secure_boot_signature(void)
{
    static const char signature_hex[] =
        "8efeb25261f303b4df8034c1921b92fb4f5a56f94b88005d09856e508d8e2086"
        "ed1d6b382113c19af1bd4075cac0d1d7a1051c9cf50ab7d9f6d38fe7006b447c"
        "fb46d0932e71108f38d895e3be4f79edb739079698ac68975be1f961aa6a7eab"
        "5af4b316f4cf5d1d218a325adc2468d0c147852a0387178e8a51a815d6942ca5"
        "d3ebf3e2c70cca9024b88c0b75fc19012da64554789fc8f3ed43f2271c7178ae"
        "9fa26611a8282e2fef1349b481e33eb2cfa5a76eb967ae936055927a9f0e9967"
        "6110e37aa926e9e59e9bba4e7f1aa27b7cc4d6fafd7db1d4f7cf6fbae1e05fd9"
        "7fdc8ccb15551668eba563bb11b05f90395289a46e11b1cda922ffc0afb23c02";

    return g_strdup_printf(
        "c9b2b839d858258c94c37a3039c809ef6be59207a1bbb1afb977d7d65373e788\n"
        "ts: 1\n"
        "rsa2048: %s\n", signature_hex);
}
QTestState *start_with_eeprom(const char *config, bool corrupt,
                                     char **path_out)
{
    g_autofree uint8_t *image = make_eeprom_image(config, corrupt);
    g_autofree char *command = NULL;

    write_temp_image("raspi4-eeprom-XXXXXX", image, EEPROM_SIZE, path_out);

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s", *path_out);
    return qtest_init(command);
}
static const char *firmware_fixup_short_name(
    const char firmware_name[11])
{
    static const struct {
        char firmware[11];
        char fixup[11];
    } pairs[] = {
        { "START   ELF", "FIXUP   DAT" },
        { "START4X ELF", "FIXUP4X DAT" },
        { "START_X ELF", "FIXUP_X DAT" },
        { "START_DBELF", "FIXUP_DBDAT" },
        { "START4CDELF", "FIXUP4CDDAT" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(pairs); i++) {
        if (!memcmp(firmware_name, pairs[i].firmware, 11)) {
            return pairs[i].fixup;
        }
    }
    return "FIXUP4  DAT";
}
QTestState *start_with_eeprom_and_sd_ram(const char *config,
                                                const char firmware_name[11],
                                                const uint8_t *firmware,
                                                size_t firmware_size,
                                                const char *sd_config,
                                                bool gpt,
                                                unsigned int ram_gib,
                                                uint32_t size_cells,
                                                char **eeprom_path_out,
                                                char **sd_path_out)
{
    static const uint8_t fixup[] = "QEMU fixup fixture";
    static const uint8_t cmdline[] = "console=ttyAMA0 root=/dev/mmcblk0p2\n";
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree char *command = NULL;
    size_t device_tree_size;
    Fat16Builder builder;

    device_tree = make_device_tree(&device_tree_size);
    g_assert_cmpint(fdt_setprop_u32(device_tree, 0, "#size-cells",
                                    size_cells), ==, 0);
    fat16_init(&builder);
    if (firmware) {
        fat16_add_file(&builder, firmware_name, firmware, firmware_size);
        fat16_add_file(&builder, firmware_fixup_short_name(firmware_name),
                       fixup, sizeof(fixup));
        fat16_add_file(&builder, "KERNEL8 IMG", kernel, TEST_KERNEL_SIZE);
        fat16_add_long_file(&builder, "bcm2711-rpi-4-b.dtb", "BCM271~1DTB",
                            device_tree, device_tree_size);
        fat16_add_file(&builder, "CMDLINE TXT", cmdline, sizeof(cmdline));
    }
    if (sd_config) {
        fat16_add_file(&builder, "CONFIG  TXT",
                       (const uint8_t *)sd_config, strlen(sd_config));
    }
    if (gpt) {
        fat16_make_gpt(&builder);
    }
    write_temp_image("raspi4-sd-XXXXXX", builder.image, SD_SIZE,
                     sd_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-m %uG "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=sd,format=raw,file=%s "
        "-nic none",
        ram_gib, *eeprom_path_out, *sd_path_out);
    return qtest_init(command);
}
QTestState *start_with_eeprom_and_sd(const char *config,
                                            const char firmware_name[11],
                                            const uint8_t *firmware,
                                            size_t firmware_size,
                                            const char *sd_config,
                                            char **eeprom_path_out,
                                            char **sd_path_out)
{
    return start_with_eeprom_and_sd_ram(
        config, firmware_name, firmware, firmware_size, sd_config, false, 2,
        2,
        eeprom_path_out, sd_path_out);
}
char *make_custom_kernel_boot_command(
    const char *machine, const uint8_t *kernel, size_t kernel_size,
    const char *media_config,
    const uint8_t *cmdline, size_t cmdline_size,
    const uint8_t *initramfs, size_t initramfs_size,
    char **eeprom_path_out, char **media_path_out)
{
    static const uint8_t start[] = "QEMU start4 fixture";
    static const uint8_t fixup[] = "QEMU fixup4 fixture";
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0xe1\n", false);
    g_autofree uint8_t *device_tree = NULL;
    bool cm4 = !strcmp(machine, "raspi-cm4");
    size_t device_tree_size;
    Fat16Builder builder;

    device_tree = make_device_tree(&device_tree_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF", start, sizeof(start));
    fat16_add_file(&builder, "FIXUP4  DAT", fixup, sizeof(fixup));
    fat16_add_file(&builder, "KERNEL8 IMG", kernel, kernel_size);
    if (initramfs) {
        fat16_add_file(&builder, "INITRD  IMG",
                       initramfs, initramfs_size);
    }
    if (media_config) {
        fat16_add_file(&builder, "CONFIG  TXT",
                       (const uint8_t *)media_config, strlen(media_config));
    }
    if (cmdline) {
        fat16_add_file(&builder, "CMDLINE TXT", cmdline, cmdline_size);
    }
    fat16_add_long_file(
        &builder, cm4 ? "bcm2711-rpi-cm4.dtb" : "bcm2711-rpi-4-b.dtb",
        cm4 ? "BCM271~2DTB" : "BCM271~1DTB",
        device_tree, device_tree_size);
    write_temp_image("raspi4-kernel-media-XXXXXX", builder.image, SD_SIZE,
                     media_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-kernel-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);

    if (cm4) {
        return g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "emmc-drive=emmc "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off "
            "-nic none",
            *eeprom_path_out, *media_path_out);
    }
    return g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        *eeprom_path_out, *media_path_out);
}

uint8_t *make_usb_boot_image_with_update(
    const char firmware_name[11], const uint8_t *firmware,
    size_t firmware_size, const char *media_config,
    const uint8_t *eeprom_update, const char *eeprom_signature)
{
    static const uint8_t fixup[] = "QEMU USB fixup fixture";
    static const uint8_t cmdline[] =
        "console=ttyAMA0 root=/dev/sda2 rootwait\n";
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree uint8_t *overlay = NULL;
    size_t device_tree_size;
    size_t overlay_size;
    Fat16Builder builder;

    device_tree = make_device_tree(&device_tree_size);
    fat16_init(&builder);
    if (firmware) {
        fat16_add_file(&builder, firmware_name, firmware, firmware_size);
        fat16_add_file(&builder, firmware_fixup_short_name(firmware_name),
                       fixup, sizeof(fixup));
        fat16_add_file(&builder, "KERNEL8 IMG", kernel, TEST_KERNEL_SIZE);
        fat16_add_long_file(&builder, "bcm2711-rpi-4-b.dtb", "BCM271~1DTB",
                            device_tree, device_tree_size);
        fat16_add_long_file(&builder, "bcm2711-rpi-cm4.dtb", "BCM271~2DTB",
                            device_tree, device_tree_size);
        fat16_add_file(&builder, "CMDLINE TXT", cmdline, sizeof(cmdline));
    }
    if (media_config) {
        fat16_add_file(&builder, "CONFIG  TXT",
                       (const uint8_t *)media_config,
                       strlen(media_config));
        if (strstr(media_config, "include extra.txt")) {
            unsigned int directory_index = 0;
            uint16_t directory = fat16_add_directory(
                &builder, "OVERLAYS   ");

            fat16_add_file(&builder, "EXTRA   TXT",
                           (const uint8_t *)TEST_NETWORK_INCLUDE,
                           strlen(TEST_NETWORK_INCLUDE));
            fat16_add_long_file(
                &builder, "initramfs-a", "INITRA~1   ",
                (const uint8_t *)TEST_NETWORK_INITRAMFS_A,
                strlen(TEST_NETWORK_INITRAMFS_A) + 1);
            fat16_add_long_file(
                &builder, "initramfs-b", "INITRA~2   ",
                (const uint8_t *)TEST_NETWORK_INITRAMFS_B,
                strlen(TEST_NETWORK_INITRAMFS_B) + 1);
            overlay = make_device_tree_overlay(&overlay_size);
            fat16_add_long_file_to_directory(
                &builder, directory, &directory_index, "test.dtbo",
                "TEST    DTB", overlay, overlay_size);
        }
    }
    if (eeprom_update) {
        fat16_add_file(&builder, "PIEEPROMUPD",
                       eeprom_update, EEPROM_SIZE);
        fat16_add_file(&builder, "PIEEPROMSIG",
                       (const uint8_t *)eeprom_signature,
                       strlen(eeprom_signature));
    }
    return builder.image;
}
uint8_t *make_usb_boot_image(const char firmware_name[11],
                                    const uint8_t *firmware,
                                    size_t firmware_size,
                                    const char *media_config)
{
    return make_usb_boot_image_with_update(
        firmware_name, firmware, firmware_size, media_config, NULL, NULL);
}
QTestState *start_with_eeprom_and_media_serial(
    bool network, const char *media_config, const char *serial_path,
    char **eeprom_path_out, char **media_path_out)
{
    static const uint8_t firmware[] = "QEMU uart_2ndstage fixture";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        network ? "BOOT_ORDER=0x2\n" : "BOOT_ORDER=0x1\n", false);
    g_autofree uint8_t *media = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), media_config);
    g_autofree char *command = NULL;

    write_temp_image(network ? "raspi4-network-XXXXXX" :
                               "raspi4-sd-XXXXXX",
                     media, SD_SIZE, media_path_out);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);
    if (network) {
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "network-boot-drive=netboot,network-boot-wire=off "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=netboot,format=raw,file=%s "
            "-serial file:%s",
            *eeprom_path_out, *media_path_out, serial_path);
    } else {
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=sd,format=raw,file=%s "
            "-serial file:%s -nic none",
            *eeprom_path_out, *media_path_out, serial_path);
    }
    return qtest_init(command);
}
uint8_t *make_tryboot_boot_image(const uint8_t *normal_start,
                                        size_t normal_start_size,
                                        const uint8_t *try_start,
                                        size_t try_start_size,
                                        const char *config_override)
{
    static const uint8_t normal_fixup[] = "QEMU normal fixup fixture";
    static const uint8_t try_fixup[] = "QEMU tryboot fixup fixture";
    static const uint8_t tryboot_config[] =
        "[tryboot]\n"
        "start_file=try-start.elf\n"
        "fixup_file=try-fixup.dat\n";
    static const uint8_t normal_config[] =
        "[bootvar0&0x0f=0x05]\n"
        "start_file=try-start.elf\n"
        "fixup_file=try-fixup.dat\n";
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    size_t device_tree_size;
    Fat16Builder builder;

    device_tree = make_device_tree(&device_tree_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF",
                   normal_start, normal_start_size);
    fat16_add_file(&builder, "FIXUP4  DAT",
                   normal_fixup, sizeof(normal_fixup));
    fat16_add_long_file(&builder, "try-start.elf", "TRYSTA~1ELF",
                        try_start, try_start_size);
    fat16_add_long_file(&builder, "try-fixup.dat", "TRYFIX~1DAT",
                        try_fixup, sizeof(try_fixup));
    fat16_add_file(&builder, "KERNEL8 IMG", kernel, TEST_KERNEL_SIZE);
    fat16_add_long_file(&builder, "bcm2711-rpi-4-b.dtb", "BCM271~1DTB",
                        device_tree, device_tree_size);
    fat16_add_file(&builder, "CONFIG  TXT",
                   config_override ?
                       (const uint8_t *)config_override : normal_config,
                   config_override ?
                       strlen(config_override) : sizeof(normal_config) - 1);
    fat16_add_file(&builder, "TRYBOOT TXT",
                   tryboot_config, sizeof(tryboot_config) - 1);
    return builder.image;
}
uint8_t *make_ab_boot_image(const uint8_t *partition2,
                                   const uint8_t *partition3)
{
    static const uint32_t lbas[] = { 2048, 18432, 34816 };
    static const uint8_t autoboot[] =
        "[all]\n"
        "tryboot_a_b=1\n"
        "boot_partition=2\n"
        "[tryboot]\n"
        "boot_partition=3\n";
    uint8_t *image = g_malloc0(AB_SD_SIZE);
    Fat16Builder partition1;

    fat16_init(&partition1);
    fat16_add_file(&partition1, "AUTOBOOTTXT",
                   autoboot, sizeof(autoboot) - 1);
    image[510] = 0x55;
    image[511] = 0xaa;
    for (unsigned int i = 0; i < ARRAY_SIZE(lbas); i++) {
        uint8_t *entry = image + 446 + i * 16;
        const uint8_t *source = i == 0 ? partition1.image :
                                i == 1 ? partition2 : partition3;

        entry[4] = 0x0e;
        stl_le_p(entry + 8, lbas[i]);
        stl_le_p(entry + 12, SD_PARTITION_SECTORS);
        memcpy(image + (uint64_t)lbas[i] * 512,
               source + SD_PARTITION_LBA * 512,
               (size_t)SD_PARTITION_SECTORS * 512);
    }
    g_free(partition1.image);
    return image;
}
uint8_t *make_logical_boot_image(const uint8_t *partition5,
                                        const uint8_t *partition6)
{
    const uint32_t media_sectors = AB_SD_SIZE / 512;
    const uint32_t extended_sectors = media_sectors - SD_PARTITION_LBA;
    uint8_t *image = g_malloc0(AB_SD_SIZE);
    uint8_t *mbr = image + 446;
    uint8_t *ebr5 = image + (uint64_t)SD_PARTITION_LBA * 512;
    uint8_t *ebr6 =
        image + (uint64_t)(SD_PARTITION_LBA + LOGICAL_EBR_GAP) * 512;

    image[510] = 0x55;
    image[511] = 0xaa;
    mbr[4] = 0x0f;
    stl_le_p(mbr + 8, SD_PARTITION_LBA);
    stl_le_p(mbr + 12, extended_sectors);

    ebr5[510] = 0x55;
    ebr5[511] = 0xaa;
    ebr5[446 + 4] = 0x0e;
    stl_le_p(ebr5 + 446 + 8, 1);
    stl_le_p(ebr5 + 446 + 12, SD_PARTITION_SECTORS);
    ebr5[446 + 16 + 4] = 0x0f;
    stl_le_p(ebr5 + 446 + 16 + 8, LOGICAL_EBR_GAP);
    stl_le_p(ebr5 + 446 + 16 + 12,
             extended_sectors - LOGICAL_EBR_GAP);

    ebr6[510] = 0x55;
    ebr6[511] = 0xaa;
    ebr6[446 + 4] = 0x0e;
    stl_le_p(ebr6 + 446 + 8, 1);
    stl_le_p(ebr6 + 446 + 12, SD_PARTITION_SECTORS);

    memcpy(ebr5 + 512, partition5 + (uint64_t)SD_PARTITION_LBA * 512,
           (size_t)SD_PARTITION_SECTORS * 512);
    memcpy(ebr6 + 512, partition6 + (uint64_t)SD_PARTITION_LBA * 512,
           (size_t)SD_PARTITION_SECTORS * 512);
    return image;
}
QTestState *start_with_eeprom_and_usb_controller(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    const char *controller, char **eeprom_path_out, char **usb_path_out)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree uint8_t *usb = make_usb_boot_image(
        firmware_name, firmware, firmware_size, NULL);
    g_autofree char *command = NULL;

    write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, usb_path_out);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drive=usbboot,usb-boot-controller=%s "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=usbboot,format=raw,file=%s",
        controller, *eeprom_path_out, *usb_path_out);
    return qtest_init(command);
}
QTestState *start_with_eeprom_and_usb(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    char **eeprom_path_out, char **usb_path_out)
{
    const char *controller =
        strstr(config, "BOOT_ORDER=0x5") ? "dwc2" : "auto";

    return start_with_eeprom_and_usb_controller(
        config, firmware_name, firmware, firmware_size, controller,
        eeprom_path_out, usb_path_out);
}
QTestState *start_with_eeprom_and_network_config(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    const char *media_config,
    char **eeprom_path_out, char **network_path_out)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree uint8_t *network = make_usb_boot_image(
        firmware_name, firmware, firmware_size, media_config);
    g_autofree char *command = NULL;

    write_temp_image("raspi4-network-XXXXXX", network, SD_SIZE,
                     network_path_out);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-drive=netboot,network-boot-wire=off "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=netboot,format=raw,file=%s",
        *eeprom_path_out, *network_path_out);
    return qtest_init(command);
}
QTestState *start_with_eeprom_and_network(
    const char *config, const char firmware_name[11],
    const uint8_t *firmware, size_t firmware_size,
    char **eeprom_path_out, char **network_path_out)
{
    return start_with_eeprom_and_network_config(
        config, firmware_name, firmware, firmware_size,
        TEST_NETWORK_CONFIG, eeprom_path_out, network_path_out);
}
QTestState *start_cm4_with_eeprom_and_emmc(
    const char *config, char **eeprom_path_out, char **emmc_path_out)
{
    static const uint8_t start[] = "QEMU CM4 start4 fixture";
    static const uint8_t fixup[] = "QEMU CM4 fixup4 fixture";
    static const uint8_t recovery[] = "must not execute from CM4 eMMC";
    static const uint8_t cmdline[] =
        "console=ttyAMA0 root=/dev/mmcblk0p2\n";
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree char *command = NULL;
    size_t device_tree_size;
    Fat16Builder builder;

    device_tree = make_device_tree(&device_tree_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF", start, sizeof(start));
    fat16_add_file(&builder, "FIXUP4  DAT", fixup, sizeof(fixup));
    fat16_add_file(&builder, "KERNEL8 IMG", kernel, TEST_KERNEL_SIZE);
    fat16_add_long_file(&builder, "bcm2711-rpi-cm4.dtb", "BCM271~1DTB",
                        device_tree, device_tree_size);
    fat16_add_file(&builder, "CMDLINE TXT", cmdline, sizeof(cmdline));
    fat16_add_file(&builder, "RECOVERYBIN", recovery, sizeof(recovery));
    fat16_add_file(&builder, "CONFIG  TXT", (const uint8_t *)config,
                   strlen(config));
    write_temp_image("raspi4-emmc-XXXXXX", builder.image, SD_SIZE,
                     emmc_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);

    command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "emmc-drive=emmc "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=emmc,format=raw,file=%s",
        *eeprom_path_out, *emmc_path_out);
    return qtest_init(command);
}
QTestState *start_arm32_memory_model_with_otp(
    const char *machine, unsigned int ram_gib, const char *bootconf,
    const char *extra_config,
    const uint8_t *initramfs, size_t initramfs_size,
    const char *otp_path, uint32_t min_boot_version,
    const char *extra_options,
    char **eeprom_path_out, char **media_path_out)
{
    static const uint8_t start[] = "QEMU ARM32 start4 fixture";
    static const uint8_t fixup[] = "QEMU ARM32 fixup4 fixture";
    static const uint8_t cmdline[] = "console=ttyAMA1\n";
    g_autofree char *config = g_strdup_printf(
        "arm_64bit=0\n%s", extra_config ? extra_config : "");
    g_autofree uint8_t *eeprom = make_eeprom_image_with_public_key(
        bootconf ? bootconf : "BOOT_ORDER=0xf1\n");
    g_autofree uint8_t *kernel = make_arm32_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree char *command = NULL;
    g_autofree char *otp_drive = otp_path ? g_strdup_printf(
        " -drive if=none,id=piotp,format=raw,file=%s,file.locking=off",
        otp_path) : g_strdup("");
    const char *otp_machine = otp_path ? ",otp-drive=piotp" : "";
    const bool cm4 = !strcmp(machine, "raspi-cm4");
    size_t device_tree_size;
    Fat16Builder builder;

    set_eeprom_build_identity(eeprom, 1779045198, "224877da");
    device_tree = make_bootloader_nvram_device_tree(&device_tree_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF", start, sizeof(start));
    fat16_add_file(&builder, "FIXUP4  DAT", fixup, sizeof(fixup));
    fat16_add_file(&builder, "KERNEL7LIMG", kernel, TEST_KERNEL_SIZE);
    if (initramfs) {
        fat16_add_file(&builder, "INITRD  IMG",
                       initramfs, initramfs_size);
    }
    fat16_add_long_file(
        &builder,
        cm4 ? "bcm2711-rpi-cm4.dtb" : "bcm2711-rpi-4-b.dtb",
        "BCM271~1DTB", device_tree, device_tree_size);
    fat16_add_file(&builder, "CMDLINE TXT", cmdline, sizeof(cmdline));
    fat16_add_file(&builder, "CONFIG  TXT",
                   (const uint8_t *)config, strlen(config));
    write_temp_image("raspi4-arm32-memory-XXXXXX", builder.image, SD_SIZE,
                     media_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-arm32-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);

    if (cm4) {
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "emmc-drive=emmc%s,min-boot-version=%u -m %uG "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=emmc,format=raw,file=%s "
            "-nic none%s %s",
            otp_machine, min_boot_version, ram_gib,
            *eeprom_path_out, *media_path_out,
            otp_drive, extra_options ? extra_options : "");
    } else {
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom%s,"
            "min-boot-version=%u "
            "-m %uG "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=sd,format=raw,file=%s "
            "-nic none%s %s",
            otp_machine, min_boot_version, ram_gib,
            *eeprom_path_out, *media_path_out,
            otp_drive, extra_options ? extra_options : "");
    }
    return qtest_init(command);
}
QTestState *start_arm32_memory_model(
    const char *machine, unsigned int ram_gib, const char *extra_config,
    const uint8_t *initramfs, size_t initramfs_size,
    char **eeprom_path_out, char **media_path_out)
{
    return start_arm32_memory_model_with_otp(
        machine, ram_gib, NULL, extra_config, initramfs, initramfs_size,
        NULL, 0, NULL, eeprom_path_out, media_path_out);
}
QTestState *start_with_complex_firmware_config_variant(
    const char *extra_config, TestOverlayExportVariant export_variant,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    bool wireless_model,
    char **eeprom_path_out, char **sd_path_out, char **hat_path_out)
{
    static const uint8_t start[] = "QEMU start4 fixture";
    static const uint8_t fixup[] = "QEMU fixup4 fixture";
    static const uint8_t cmdline[] = "console=serial0 root=PARTUUID=test\n";
    static const uint8_t initramfs_first[] = "QEMU initramfs first";
    static const uint8_t initramfs_second[] = "QEMU initramfs second";
    static const char config[] =
        "[pi5]\n"
        "kernel=ignored.img\n"
        "[all]\n"
        "os_prefix=alt/\n"
        "overlay_prefix=\n"
        "initramfs initr-a,initr-b followkernel\n"
        "include extra.txt\n"
        "caller_probe=1\n";
    static const char default_extra[] =
        "[pi4]\n"
        "dtparam=base_enable=on,base_value=99\n"
        "dtoverlay=vc4-kms-v3d,enable=on,value=42,byte_value=171,"
        "word_value=4660,linked=on,external_link=on\n"
        "dtparam=wide_value=0x1122334455667788,"
        "mac_value=b8:27:eb:01:23:45\n"
        "dtparam=bool_value=on,invert_value=off,"
        "force_fragment=ignored\n"
        "dtparam=lookup_name=b,lookup_default=z\n"
        "dtparam=lookup_pass=verbatim,lookup_local=local\n"
        "dtparam=lookup_external=external,multi_value=123,"
        "multi_bool=on,noop=ignored\n"
        "dtparam=reg_value=42,name_value=renamed-device\n"
        "dtoverlay=consumer\n"
        "start_file=ignored.elf\n"
        "fixup_file=ignored.dat\n";
    const char *extra = extra_config ? extra_config : default_extra;
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xf1\nMAC_ADDRESS=dc:a6:32:01:36:c2\n", false);
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree uint8_t *overlay = NULL;
    g_autofree uint8_t *consumer_overlay = NULL;
    g_autofree uint8_t *overlay_map = NULL;
    g_autofree char *command = NULL;
    size_t device_tree_size;
    size_t overlay_size;
    size_t consumer_overlay_size;
    size_t overlay_map_size;
    Fat16Builder builder;
    uint16_t directory_cluster;
    unsigned int directory_index = 0;

    device_tree = make_wireless_device_tree(&device_tree_size);
    overlay = make_device_tree_overlay_export_variant(
        &overlay_size, export_variant);
    consumer_overlay = make_export_consumer_overlay(&consumer_overlay_size);
    overlay_map = make_overlay_map(&overlay_map_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF", start, sizeof(start));
    fat16_add_file(&builder, "FIXUP4  DAT", fixup, sizeof(fixup));
    fat16_add_file(&builder, "CONFIG  TXT", (const uint8_t *)config,
                   strlen(config));
    fat16_add_file(&builder, "EXTRA   TXT", (const uint8_t *)extra,
                   strlen(extra));
    directory_cluster = fat16_add_directory(&builder, "ALT        ");
    fat16_add_file_to_directory(&builder, directory_cluster,
                                &directory_index, "KERNEL8 IMG",
                                kernel, TEST_KERNEL_SIZE);
    fat16_add_long_file_to_directory(
        &builder, directory_cluster, &directory_index,
        "bcm2711-rpi-4-b.dtb", "BCM271~1DTB", device_tree,
        device_tree_size);
    fat16_add_file_to_directory(&builder, directory_cluster,
                                &directory_index, "CMDLINE TXT",
                                cmdline, sizeof(cmdline));
    fat16_add_file_to_directory(
        &builder, directory_cluster, &directory_index, "INITR-A    ",
        initramfs_first, sizeof(initramfs_first));
    fat16_add_file_to_directory(
        &builder, directory_cluster, &directory_index, "INITR-B    ",
        initramfs_second, sizeof(initramfs_second));
    fat16_add_long_file_to_directory(
        &builder, directory_cluster, &directory_index,
        "overlay_map.dtb", "OVERLA~1DTB", overlay_map, overlay_map_size);
    fat16_add_long_file_to_directory(
        &builder, directory_cluster, &directory_index,
        "vc4-kms-v3d-pi4.dtbo", "VC4KM~1 DTB", overlay, overlay_size);
    fat16_add_long_file_to_directory(
        &builder, directory_cluster, &directory_index,
        "consumer.dtbo", "CONSUM~1DTB", consumer_overlay,
        consumer_overlay_size);

    write_temp_image("raspi4-sd-XXXXXX", builder.image, SD_SIZE,
                     sd_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     eeprom_path_out);
    if (hat_eeprom) {
        write_temp_image("raspi4-hat-eeprom-XXXXXX", hat_eeprom,
                         hat_eeprom_size, hat_path_out);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom%s,"
            "hat-eeprom-drive=hat "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=hat,format=raw,readonly=on,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off",
            wireless_model ? ",wireless-model=on" : "",
            *eeprom_path_out, *hat_path_out, *sd_path_out);
    } else {
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off",
            wireless_model ? ",wireless-model=on" : "",
            *eeprom_path_out, *sd_path_out);
    }
    return qtest_init(command);
}
QTestState *start_with_complex_firmware_config(
    const char *extra_config, char **eeprom_path_out, char **sd_path_out)
{
    return start_with_complex_firmware_config_variant(
        extra_config, TEST_EXPORT_VALID, NULL, 0,
        false,
        eeprom_path_out, sd_path_out, NULL);
}
QTestState *start_recovery_stage_extra(
    bool bad_signature, bool write_protect, uint64_t fail_after,
    const char *fail_stage, bool permanent, bool fragmented,
    const char *eeprom_debug_path, const char *machine_extra,
    const char *recovery_config, const char *extra_drive,
    char **eeprom_path_out, char **sd_path_out, uint8_t **expected_out)
{
    g_autofree uint8_t *recovery = make_test_recovery();
    g_autofree uint8_t *initial =
        make_eeprom_image("BOOT_ORDER=0xf41\n", false);
    g_autofree char *digest = NULL;
    g_autofree char *recovery_digest = NULL;
    g_autofree char *signature = NULL;
    g_autofree char *command = NULL;
    g_autofree char *eeprom_drive = NULL;
    Fat16Builder builder;

    *expected_out = make_eeprom_image("BOOT_ORDER=0xf14\n", false);
    digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, *expected_out,
                                         EEPROM_SIZE);
    signature = g_strdup_printf("%s\nts: 1\n", digest);
    recovery_digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, recovery, TEST_RECOVERY_SIZE);
    if (bad_signature) {
        signature[0] = signature[0] == '0' ? '1' : '0';
    }

    fat16_init(&builder);
    fat16_add_file(&builder, "RECOVERYBIN", recovery, TEST_RECOVERY_SIZE);
    if (fragmented) {
        fat16_add_fragmented_file(
            &builder, permanent ? "PIEEPROMBIN" : "PIEEPROMUPD",
            *expected_out, EEPROM_SIZE);
    } else {
        fat16_add_file(&builder,
                       permanent ? "PIEEPROMBIN" : "PIEEPROMUPD",
                       *expected_out, EEPROM_SIZE);
    }
    fat16_add_file(&builder, "PIEEPROMSIG", (uint8_t *)signature,
                   strlen(signature));
    if (recovery_config) {
        fat16_add_file(&builder, "CONFIG  TXT",
                       (const uint8_t *)recovery_config,
                       strlen(recovery_config));
    }
    write_temp_image("raspi4-sd-XXXXXX", builder.image, SD_SIZE,
                     sd_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-eeprom-XXXXXX", initial, EEPROM_SIZE,
                     eeprom_path_out);

    eeprom_drive = eeprom_debug_path ?
        g_strdup_printf(
            "-drive if=none,id=pieeprom,format=raw,"
            "file=blkdebug:%s:%s ",
            eeprom_debug_path, *eeprom_path_out) :
        g_strdup_printf(
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off ",
            *eeprom_path_out);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "recovery-trusted-sha256=%s,"
        "eeprom-write-protect=%s,eeprom-fail-after=%" PRIu64 ","
        "eeprom-fail-stage=%s%s "
        "%s"
        "%s"
        "-drive if=sd,format=raw,file=%s,file.locking=off",
        recovery_digest, write_protect ? "on" : "off",
        fail_after, fail_stage, machine_extra ? machine_extra : "",
        eeprom_drive, extra_drive ? extra_drive : "", *sd_path_out);
    return qtest_init(command);
}
QTestState *start_recovery_stage(bool bad_signature,
                                        bool write_protect,
                                        uint64_t fail_after,
                                        const char *fail_stage,
                                        bool permanent,
                                        bool fragmented,
                                        const char *eeprom_debug_path,
                                        char **eeprom_path_out,
                                        char **sd_path_out,
                                        uint8_t **expected_out)
{
    return start_recovery_stage_extra(
        bad_signature, write_protect, fail_after, fail_stage, permanent,
        fragmented, eeprom_debug_path, NULL, NULL, NULL, eeprom_path_out,
        sd_path_out, expected_out);
}
QTestState *start_recovery(bool bad_signature, bool write_protect,
                                  uint64_t fail_after, bool permanent,
                                  char **eeprom_path_out, char **sd_path_out,
                                  uint8_t **expected_out)
{
    return start_recovery_stage(bad_signature, write_protect, fail_after,
                                "program", permanent, false, NULL,
                                eeprom_path_out, sd_path_out, expected_out);
}
QTestState *start_with_otp(uint32_t bootmode,
                                  uint32_t bootmode_copy,
                                  uint32_t board_revision,
                                  bool customer_key,
                                  unsigned int rpiboot_gpio,
                                  bool nrpiboot,
                                  char **path_out)
{
    g_autofree uint8_t *image = make_otp_image(
        bootmode, bootmode_copy, board_revision, customer_key);
    g_autofree char *command = NULL;

    write_temp_image("raspi4-otp-XXXXXX", image, OTP_SIZE, path_out);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,otp-drive=piotp,"
        "otp-rpiboot-gpio=%u,nrpiboot=%s "
        "-drive if=none,id=piotp,format=raw,file=%s",
        rpiboot_gpio, nrpiboot ? "on" : "off", *path_out);
    return qtest_init(command);
}
void assert_file_equals(const char *path, const uint8_t *expected,
                               size_t expected_size)
{
    g_autofree char *contents = NULL;
    size_t size;

    g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
    g_assert_cmpuint(size, ==, expected_size);
    g_assert_cmpmem(contents, size, expected, expected_size);
}
void assert_logical_mbr_rejected(const uint8_t *sd,
                                        const char *eeprom_config)
{
    g_autofree uint8_t *eeprom =
        make_eeprom_image(eeprom_config, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-logical-invalid-XXXXXX", sd, AB_SD_SIZE,
                     &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
uint8_t *test_read_handoff_dtb(QTestState *qts)
{
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    uint8_t *dtb = g_malloc(size);

    g_assert_cmpuint(size, >, 0);
    qtest_memread(qts, address, dtb, size);
    g_assert_cmpint(fdt_check_header(dtb), ==, 0);
    return dtb;
}
void raspi4_sdhci_command(QTestState *qts, uint64_t base,
                                 uint16_t block_size, uint16_t block_count,
                                 uint32_t argument, uint16_t transfer_mode,
                                 uint16_t command)
{
    qtest_writew(qts, base + SDHC_BLKSIZE, block_size);
    qtest_writew(qts, base + SDHC_BLKCNT, block_count);
    qtest_writel(qts, base + SDHC_ARGUMENT, argument);
    qtest_writew(qts, base + SDHC_TRNMOD, transfer_mode);
    qtest_writew(qts, base + SDHC_CMDREG, command);
}
uint32_t cyw_sdio_cmd52_argument(bool write, bool raw,
                                        uint8_t function, uint32_t address,
                                        uint8_t value)
{
    return (write ? BIT(31) : 0) | ((uint32_t)function << 28) |
           (raw ? BIT(27) : 0) | (address << 9) | value;
}
uint32_t cyw_sdio_cmd53_argument(bool write, uint8_t function,
                                        bool increment, uint32_t address,
                                        uint16_t count)
{
    return (write ? BIT(31) : 0) | ((uint32_t)function << 28) |
           (increment ? BIT(26) : 0) | (address << 9) | (count & 0x1ff);
}
void cyw_sdio_set_backplane_window(QTestState *qts, uint32_t address)
{
    const uint32_t base = address & ~0x7fff;

    for (unsigned int i = 0; i < 3; i++) {
        raspi4_sdhci_command(
            qts, BCM2711_EMMC_BASE, 0, 0,
            cyw_sdio_cmd52_argument(true, false, 1, 0x1000a + i,
                                    extract32(base, 8 + i * 8, 8)),
            0, (52 << 8) | SDHC_CMD_RESPONSE);
    }
}
uint32_t cyw_sdio_backplane_readl(QTestState *qts, uint32_t address)
{
    cyw_sdio_set_backplane_window(qts, address);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(false, 1, true,
                                0x8000 | (address & 0x7fff), 4),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    return qtest_readl(qts, BCM2711_EMMC_BASE + SDHC_BDATA);
}
void cyw_sdio_backplane_writel(QTestState *qts, uint32_t address,
                                      uint32_t value)
{
    cyw_sdio_set_backplane_window(qts, address);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(true, 1, true,
                                0x8000 | (address & 0x7fff), 4),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    qtest_writel(qts, BCM2711_EMMC_BASE + SDHC_BDATA, value);
}
void cyw_sdio_init_card(QTestState *qts)
{
    uint32_t response;

    qtest_writeb(qts, BCM2711_EMMC_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, BCM2711_EMMC_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         0x00ff8000, 0,
                         (5 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0, 0, 0,
                         (3 << 8) | SDHC_CMD_RESPONSE);
    response = qtest_readl(qts, BCM2711_EMMC_BASE + SDHC_RSPREG0);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         response & 0xffff0000, 0,
                         (7 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x02, BIT(1)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
}
void cyw_sdio_start_firmware(QTestState *qts)
{
    cyw_sdio_init_card(qts);
    cyw_sdio_backplane_writel(qts, 0x00198000, 0xe1a00000);
    cyw_sdio_backplane_writel(qts, 0, 0xe1a00000);
    cyw_sdio_backplane_writel(qts, 0x0025fff4, 0x313d6161);
    cyw_sdio_backplane_writel(qts, 0x0025fff8, 0);
    cyw_sdio_backplane_writel(qts, 0x0025fffc, 0xfffd0002);
    cyw_sdio_backplane_writel(qts, 0x18103800, 1);
    cyw_sdio_backplane_writel(qts, 0x18103800, 0);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x02, BIT(1) | BIT(2)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
}
void cyw_sdio_send_guest_packet(QTestState *qts,
                                       const uint8_t *packet, size_t size,
                                       uint8_t sequence)
{
    g_autofree uint8_t *frame = g_malloc0(16 + size);

    g_assert_cmpuint(16 + size, <=, UINT16_MAX);
    stw_le_p(frame, 16 + size);
    stw_le_p(frame + 2, (uint16_t)~(16 + size));
    stl_le_p(frame + 4, sequence | (2U << 8) | (12U << 24));
    frame[12] = 2 << 4;
    memcpy(frame + 16, packet, size);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 16 + size, 1,
        cyw_sdio_cmd53_argument(true, 2, false, 0, 16 + size),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 16 + size; i += sizeof(uint32_t)) {
        uint32_t word = 0;

        memcpy(&word, frame + i, MIN(sizeof(word), 16 + size - i));
        qtest_writel(qts, BCM2711_EMMC_BASE + SDHC_BDATA, le32_to_cpu(word));
    }
}
void cyw_sdio_read_guest_packet(QTestState *qts, uint8_t *packet,
                                       size_t size)
{
    g_autofree uint8_t *frame = g_malloc0(16 + size);

    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 16 + size, 1,
        cyw_sdio_cmd53_argument(false, 2, false, 0, 16 + size),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 16 + size; i += sizeof(uint32_t)) {
        uint32_t word = cpu_to_le32(
            qtest_readl(qts, BCM2711_EMMC_BASE + SDHC_BDATA));

        memcpy(frame + i, &word, MIN(sizeof(word), 16 + size - i));
    }
    g_assert_cmphex(lduw_le_p(frame), ==, 16 + size);
    g_assert_cmphex(extract32(ldl_le_p(frame + 4), 8, 4), ==, 2);
    g_assert_cmphex(frame[12], ==, 2 << 4);
    memcpy(packet, frame + 16, size);
}
void raspi4_hci_uart_write(QTestState *qts, const uint8_t *data,
                                  size_t size)
{
    for (size_t i = 0; i < size; i++) {
        qtest_writel(qts, BCM2711_UART0_BASE + PL011_DR, data[i]);
    }
}
void raspi4_hci_uart_read(QTestState *qts, uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        for (unsigned int attempt = 0; attempt < 100; attempt++) {
            if (!(qtest_readl(qts, BCM2711_UART0_BASE + PL011_FR) &
                  PL011_FR_RXFE)) {
                break;
            }
            g_usleep(1000);
        }
        g_assert_false(qtest_readl(qts, BCM2711_UART0_BASE + PL011_FR) &
                       PL011_FR_RXFE);
        data[i] = qtest_readl(qts, BCM2711_UART0_BASE + PL011_DR);
    }
}
void raspi4_hci_uart_configure(QTestState *qts)
{
    qtest_writel(qts, BCM2711_UART0_BASE + PL011_LCRH, BIT(4));
    qtest_writel(qts, BCM2711_UART0_BASE + PL011_CR,
                 BIT(9) | BIT(8) | BIT(0));
}
void raspi4_sdhci_initialize_card(QTestState *qts)
{
    uint32_t rca;

    qtest_writeb(qts, BCM2711_EMMC2_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, BCM2711_EMMC2_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE,
                         0, 0, 0, 0, 55 << 8);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE,
                         0, 0, 0x41200000, 0, 41 << 8);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE,
                         0, 0, 0, 0, 2 << 8);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0, 0, 0,
                         (3 << 8) | SDHC_CMD_RESPONSE);
    rca = qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_RSPREG0) >> 16;
    g_assert_cmpuint(rca, !=, 0);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE,
                         0, 0, rca << 16, 0, 7 << 8);
}
void raspi4_sdhci_initialize_emmc(QTestState *qts)
{
    const uint32_t rca = 1;

    qtest_writeb(qts, BCM2711_EMMC2_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, BCM2711_EMMC2_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0, 0, 0, 0);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0,
                         0x40ff8000, 0, (1 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE,
                         0, 0, 0, 0, 2 << 8);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0,
                         rca << 16, 0, (3 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0,
                         rca << 16, 0, 7 << 8);
}
void raspi4_emmc_switch(QTestState *qts, uint8_t index, uint8_t value)
{
    uint32_t argument = (3u << 24) | ((uint32_t)index << 16) |
                        ((uint32_t)value << 8);

    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0, argument, 0,
                         (6 << 8) | SDHC_CMD_RESPONSE);
}
void raspi4_emmc_write_sector(QTestState *qts, uint32_t address,
                                     uint32_t value)
{
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 1, address,
                         SDHC_TRNS_BLK_CNT_EN,
                         (24 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 128; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, value);
    }
}
uint32_t raspi4_emmc_read_word(QTestState *qts, uint32_t address)
{
    uint32_t first;

    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 1, address,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
                         (17 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    first = qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_BDATA);
    for (unsigned int i = 1; i < 128; i++) {
        qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_BDATA);
    }
    return first;
}
void raspi4_emmc_read_ext_csd(QTestState *qts, uint8_t ext_csd[512])
{
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 1, 0,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
                         (8 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 512 / sizeof(uint32_t); i++) {
        stl_le_p(ext_csd + i * sizeof(uint32_t),
                 qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_BDATA));
    }
}
uint32_t raspi4_emmc_read_cache_size_kib(QTestState *qts)
{
    uint8_t ext_csd[512];

    raspi4_emmc_read_ext_csd(qts, ext_csd);
    return ldl_le_p(ext_csd + 249);
}
uint32_t raspi4_emmc_cache_dirty_sectors(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
             "'path': '/machine/soc/peripherals/emmc2/sd-bus/child[0]',"
             "'property': 'cache-dirty-sectors' } }");
    uint32_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
bool raspi4_emmc_cache_flush_active(QTestState *qts)
{
    return qtest_qom_get_bool(
        qts, "/machine/soc/peripherals/emmc2/sd-bus/child[0]",
        "cache-flush-active");
}
uint64_t raspi4_emmc_cache_flush_completed(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
             "'path': '/machine/soc/peripherals/emmc2/sd-bus/child[0]',"
             "'property': 'cache-flush-completed-sectors' } }");
    uint64_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
uint32_t raspi4_emmc_program_pending(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
             "'path': '/machine/soc/peripherals/emmc2/sd-bus/child[0]',"
             "'property': 'program-pending-sectors' } }");
    uint32_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
bool raspi4_emmc_program_active(QTestState *qts)
{
    return qtest_qom_get_bool(
        qts, "/machine/soc/peripherals/emmc2/sd-bus/child[0]",
        "program-active");
}
uint64_t raspi4_emmc_program_completed(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
             "'path': '/machine/soc/peripherals/emmc2/sd-bus/child[0]',"
             "'property': 'program-completed-sectors' } }");
    uint64_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
bool raspi4_emmc_erase_active(QTestState *qts)
{
    return qtest_qom_get_bool(
        qts, "/machine/soc/peripherals/emmc2/sd-bus/child[0]",
        "erase-active");
}
uint64_t raspi4_emmc_erase_pending(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
             "'path': '/machine/soc/peripherals/emmc2/sd-bus/child[0]',"
             "'property': 'erase-pending-groups' } }");
    uint64_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
uint64_t raspi4_emmc_erase_completed(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
             "'path': '/machine/soc/peripherals/emmc2/sd-bus/child[0]',"
             "'property': 'erase-completed-groups' } }");
    uint64_t result = qdict_get_int(response, "return");

    qobject_unref(response);
    return result;
}
void raspi4_emmc_erase(QTestState *qts, uint32_t start_address,
                              uint32_t end_address)
{
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0,
                         start_address, 0,
                         (35 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0,
                         end_address, 0,
                         (36 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0, 0, 0,
                         (38 << 8) | SDHC_CMD_RESPONSE);
}
void raspi4_emmc_write_multiple(QTestState *qts, uint32_t address,
                                       const uint32_t *values,
                                       unsigned int count, bool reliable)
{
    uint32_t auto_cmd23 = count;

    g_assert_cmpuint(count, >, 0);
    g_assert_cmpuint(count, <=, UINT16_MAX);
    if (reliable) {
        auto_cmd23 |= BIT(31);
    }
    qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_SYSAD, auto_cmd23);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, count, address,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_ACMD23 |
                         SDHC_TRNS_MULTI,
                         (25 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int sector = 0; sector < count; sector++) {
        for (unsigned int word = 0; word < 128; word++) {
            qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA,
                         values[sector]);
        }
    }
}
uint32_t raspi4_emmc_status(QTestState *qts)
{
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 0, 0, 1 << 16, 0,
                         (13 << 8) | SDHC_CMD_RESPONSE);
    return qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_RSPREG0);
}
uint8_t *make_signed_eeprom(bool omit_signature, bool tamper_config,
                                   bool tamper_signature,
                                   unsigned int network_mode,
                                   TestDuplicateEepromFile duplicate_file)
{
    static const char block_config[] =
        "BOOT_ORDER=0xf1\n"
        "SIGNED_BOOT=1\n";
    static const char network_config[] =
        "BOOT_ORDER=0xf2\n"
        "SIGNED_BOOT=1\n"
        "TFTP_IP=10.0.2.99\n"
        "CLIENT_IP=10.0.2.15\n"
        "SUBNET=255.255.255.0\n";
    static const char http_config[] =
        "BOOT_ORDER=0xf7\n"
        "SIGNED_BOOT=1\n"
        "HTTP_HOST=10.0.2.99\n"
        "HTTP_PATH=net_install\n"
        "HTTP_PORT=80\n";
    static const char public_key_hex[] =
        "29c3e02ebad00899d584d6b0c104969f4c31c97ea22e75ecab4beb120472220b"
        "13d28f1da7c9ad3b359c2ec8dacee01fda62c21e6ec5164d94d643de393622b4"
        "ae7add1dc916011a8c286f4a2f1faf4684fc6a3d29900c7a57686f873de46f169"
        "59a6ddfadc833364b2bd1b5239fe1bdc29172e98acac500df86644fef4102073e"
        "d180a6589c8299640753e7ff60b268c320a60dedafab6b6b7521e24c3c7454f61"
        "9e16ae57a0d2f04b9ab364e821557b1460d9e8001bd0794b8a3fd7eb3417e786c"
        "2a496f28a40ebea90a44d95fa54d9f1362f62e7aee6633291c7f90e01acd4ec9"
        "e22a3a9de1af156035720bee0e6b0c792f32c90c9cb1cb6c66b7029cbb92010001"
        "0000000000";
    static const char signature_hex[] =
        "36f4a581dccf36dce89fdc52d708fe803f178a691e5b1c6a96ba5c2448ac1f1d"
        "5072730d9757d839115ca7201c3455c3b7cbb9f820d05482e93cf8f822363edc"
        "4adfacbdd0490517f9cff921666a45ab030844cd51172478d6ea61f52759ac064"
        "987c87dcbbcb0724729945dee1c8d4f00bb3b911bfe1a823471b7ea5298163445"
        "33b16edad808b7b815bad23582cd71a8e70ec2fb8103b7dba7891da85a2ef7823"
        "5bf80e58528901f09740402ac8a96d1b4665396ab632c1bfb14a09398954ae7bd"
        "e16cc5cb2a6ed204e7ed5fb9ed1fe7dc8791c3b4fc418ff9f9891d7cd0091e0c"
        "5f6a5e240bfd34b3f91b5765cf7f841008b568f77fc908f8fca350132f8a";
    static const char network_signature_hex[] =
        "8b40057e6802cd8c933f330596401c17bb9e55caeb7cc0e5b288015fdf4f587"
        "83273f55b2f574d48559493ed0ae0d90f64d34e4f85d699b5d847da17273d65"
        "d77c87a548b1c9f988a0ad7ce8e05cc7fe5b587b01a4dc4fd2b48d5013b202e"
        "6e4ede960ce08f72689203dc4e7fcb179c333dd41a1f09034ba87dcd932f2288"
        "58a48fc43c4e8da0d71f0e315d65cb0b8bdeaa6c517b03b65d2a645398a9740"
        "9ecb339e459abff66374e12e0602452ab48acc985a00afb2ba7c45b585fae5f1"
        "38e0a65d12fc22c293f9c33e4723e04200f24cfa6bb7b25e835d793a7d7a20a"
        "c1dbbc8e1655c1f539ede17726c47ed719e1329c2251bb38e2a32baef3574d67"
        "9ebec";
    static const char http_signature_hex[] =
        "65ab0e76cade3ba222bcb8751efa8213cf3e5f1fb1c57d6672f3149d5b74ccfd"
        "fdc29a99bd2293ad11f62210118849c48b5b1713ed9f025d6b3981d356f58795"
        "0ca4e38640dedce95e6981fab14deeb42de47810ba0adf8e044a35da63231ed93"
        "f8ad23594966317e98d68623e4fb27402fc698659d8a7e21cb87ae3d327303045"
        "3208c08d612fe5eb394717d02c67a6855e2a4e52efbc091ffa92dc22ccba15d3"
        "a454108cf4f217b452b03bb96d21c994f0fd165978b2e6398eb1df27faf8d160"
        "99d4241acf00643a7608f80a104e94102114d150760234e674d17e435614b44fb"
        "8c3cdbe13a723ef057523aceda9b9178dcc4009a1a409c9c662dee16e982b";
    const char *config = network_mode == 2 ? http_config :
                         network_mode == 1 ? network_config : block_config;
    const char *digest = network_mode == 2 ?
        "cb2bdce1edd96225088f6403b0ede7d45e76232eea5c9942b142294dff09f3e6" :
        network_mode == 1 ?
        "b5fa8308241788f407e163622cf60b34bd9d74ed3db47c2860f6abb371374df3" :
        "79205af6f5a1e92c4026e22a98353179d29672a410cc803f5f0ba799986af298";
    const char *rsa = network_mode == 2 ? http_signature_hex :
                      network_mode == 1 ? network_signature_hex :
                                          signature_hex;
    uint8_t public_key[264];
    uint8_t *image = g_malloc(EEPROM_SIZE);
    g_autofree char *signature = g_strdup_printf(
        "%s\n"
        "ts: 1\n"
        "rsa2048: %s\n", digest, rsa);
    size_t config_offset;
    size_t offset;

    test_decode_hex(public_key_hex, public_key, sizeof(public_key));
    if (tamper_signature) {
        signature[strlen(signature) - 2] =
            signature[strlen(signature) - 2] == '0' ? '1' : '0';
    }
    memset(image, 0xff, EEPROM_SIZE);
    offset = eeprom_add_bootsys(image);
    offset = eeprom_add_secure_dependencies(image, offset);
    config_offset = offset;
    offset = eeprom_add_file(
        image, offset, "bootconf.txt", (const uint8_t *)config,
        strlen(config));
    if (tamper_config) {
        image[config_offset + 24] ^= 1;
    }
    if (!omit_signature) {
        offset = eeprom_add_file(
            image, offset, "bootconf.sig", (const uint8_t *)signature,
            strlen(signature));
    }
    offset = eeprom_add_file(image, offset, "pubkey.bin\0\0", public_key,
                             sizeof(public_key));
    switch (duplicate_file) {
    case TEST_EEPROM_DUPLICATE_NONE:
        break;
    case TEST_EEPROM_DUPLICATE_BOOTCONF:
        eeprom_add_file(image, offset, "bootconf.txt",
                        (const uint8_t *)config, strlen(config));
        break;
    case TEST_EEPROM_DUPLICATE_SIGNATURE:
        eeprom_add_file(image, offset, "bootconf.sig",
                        (const uint8_t *)signature, strlen(signature));
        break;
    case TEST_EEPROM_DUPLICATE_PUBLIC_KEY:
        eeprom_add_file(image, offset, "pubkey.bin\0\0", public_key,
                        sizeof(public_key));
        break;
    default:
        g_assert_not_reached();
    }
    return image;
}
QTestState *start_secure_provision_recovery(
    const char *provision_config, const uint8_t *initial_otp,
    bool attach_otp, bool trust_bootsys, uint8_t otp_fail_after,
    char **eeprom_path_out, char **otp_path_out,
    char **sd_path_out, uint8_t **signed_eeprom_out)
{
    g_autofree uint8_t *recovery = make_test_recovery();
    g_autofree uint8_t *initial_eeprom =
        make_eeprom_image("BOOT_ORDER=0xf41\n", false);
    g_autofree char *digest = NULL;
    g_autofree char *recovery_digest = NULL;
    g_autofree char *signature = NULL;
    g_autofree char *command = NULL;
    g_autofree char *otp_argument = NULL;
    Fat16Builder builder;

    *signed_eeprom_out = make_signed_eeprom(
        false, false, false, 0, TEST_EEPROM_DUPLICATE_NONE);
    digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, *signed_eeprom_out, EEPROM_SIZE);
    signature = g_strdup_printf("%s\nts: 1\n", digest);
    recovery_digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, recovery, TEST_RECOVERY_SIZE);

    fat16_init(&builder);
    fat16_add_file(&builder, "RECOVERYBIN", recovery, TEST_RECOVERY_SIZE);
    fat16_add_file(&builder, "PIEEPROMUPD",
                   *signed_eeprom_out, EEPROM_SIZE);
    fat16_add_file(&builder, "PIEEPROMSIG",
                   (const uint8_t *)signature, strlen(signature));
    fat16_add_file(&builder, "CONFIG  TXT",
                   (const uint8_t *)provision_config,
                   strlen(provision_config));
    write_temp_image("raspi4-secure-provision-sd-XXXXXX",
                     builder.image, SD_SIZE, sd_path_out);
    g_free(builder.image);
    write_temp_image("raspi4-secure-provision-eeprom-XXXXXX",
                     initial_eeprom, EEPROM_SIZE, eeprom_path_out);
    if (attach_otp) {
        write_temp_image("raspi4-secure-provision-otp-XXXXXX",
                         initial_otp, OTP_SIZE, otp_path_out);
        otp_argument = g_strdup_printf(
            ",otp-drive=piotp "
            "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off ",
            *otp_path_out);
    } else {
        *otp_path_out = NULL;
        otp_argument = g_strdup(" ");
    }
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "recovery-trusted-sha256=%s,"
        "otp-provision-fail-after=%u%s%s"
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        recovery_digest, otp_fail_after,
        trust_bootsys ? TEST_BOOTSYS_MACHINE_OPTION : "",
        otp_argument, *eeprom_path_out, *sd_path_out);
    return qtest_init(command);
}
uint8_t *make_signed_nvme_eeprom(void)
{
    static const char config[] =
        "BOOT_ORDER=0xf6\n"
        "SIGNED_BOOT=1\n";
    static const char public_key_hex[] =
        "d7c5c33fd29459de661b8e68362dc2c8bb7d9d9d4f6c92a33a111c28aeb819b6"
        "973871a8800322d28524da161ac39b05faa99db4ea79d7dd9bdde0a0ba53c278f"
        "7a80f5109cdbab74ae3ec8df181d0300c01e9d33cbd637e6e5b915edc38d1dd5"
        "354cb2ecd7a227095a8c002dd35d6954536deb3c4139f471730b224853e1cf6c4"
        "6215e7f7b94589fe0fa9db17c770518ac72d3488f71927f9441997e5b62b7817"
        "5139aba1ac4d8a00630c1c832dbd278fd0be667b0bc96463f4e5132f688103261"
        "22166138fa9c0f68b38ed9f57869ded637e5537ab44c6d520353bb9b81fc089ac"
        "9d2d103ad5bd64ba6a9f6328d0694eb1e1cda8d59e6cd6d32eefc45e93a8010"
        "0010000000000";
    static const char signature_hex[] =
        "39c29d093cc11ae151e6444dbd800f535ced75bd511f53a94c5b413629b5029b"
        "d57d8227eaab67be5aff266ab2d6e364ad04767185061a8eadd4ac2f249fbdc9a"
        "0a99541092d429852f745a4b13f334fc9801bc76b1a4e8e067c853566b040aa78"
        "fe22983ba9bc02c7b83e7e1985d3c26cbf5634e3972db8ed17d883dbcf4bb788"
        "94f654cbf109ff7cd0c05b822f7537535048f6bad0a4d5ed6d393b41d06e9b5e"
        "e5506fe44bf96cc37639e66e7f16f753f28b8d76bec51f4319551530a8f4220a"
        "076ae109dc795a7ac4510a6dfe193928d1d70b2a4cf3e6a52e332ba8174dc428d"
        "483d855ded001c2a16e04bd390d5475bbad0efca18f5719949feafcbe22ea";
    static const char digest[] =
        "850ab68d6fe4746e249284245971c99514dce864467b8c1e7e94949e63189a36";
    uint8_t public_key[264];
    uint8_t *image = g_malloc(EEPROM_SIZE);
    g_autofree char *signature = g_strdup_printf(
        "%s\n"
        "ts: 1\n"
        "rsa2048: %s\n", digest, signature_hex);
    size_t offset;

    test_decode_hex(public_key_hex, public_key, sizeof(public_key));
    memset(image, 0xff, EEPROM_SIZE);
    offset = eeprom_add_bootsys(image);
    offset = eeprom_add_secure_dependencies(image, offset);
    offset = eeprom_add_file(
        image, offset, "bootconf.txt", (const uint8_t *)config,
        strlen(config));
    offset = eeprom_add_file(
        image, offset, "bootconf.sig", (const uint8_t *)signature,
        strlen(signature));
    eeprom_add_file(image, offset, "pubkey.bin\0\0", public_key,
                    sizeof(public_key));
    return image;
}
uint8_t *make_dns_signed_http_eeprom(void)
{
    static const char config[] =
        "BOOT_ORDER=0xf7\n"
        "SIGNED_BOOT=1\n"
        "HTTP_HOST=boot.test\n"
        "HTTP_PATH=net_install\n"
        "HTTP_PORT=80\n";
    static const char public_key_hex[] =
        "B5C0E664C3D160EC3AECA2B90DBB59BBF788F525C5F987C88A208AA60C6DB405"
        "EAB5B4A6D0AB650A8458ADE07D3DA9B72383B47EC86E39DE9F6694A134689757"
        "6EBF53E98E2729906862DA6B29CC7DBDF18C85ADD5BA7D5FB3097EF5730BDC4E"
        "A484F8386574AFDD248AD1DCE94EEFBFE67C0B102C9FAB878FE8F30828E1112F"
        "681FB170FAA50CDFB724FE63C3461BCDC684634628A3F9B73916BAE9566C5A57"
        "B334EAF968C17DA43B1845813FEE909CC201824BD5248D780AC4509BFDB75802"
        "43D8D5D0C74A5C3FB92C4A0AF3903DC853A864C370153D78AECFE6AC8B4FD10D"
        "B56DE0D7E0AB7DD26D1CC3E02C8B82A77423E162BF782B8C5E7480E08AB29EB4"
        "0100010000000000";
    static const char signature_hex[] =
        "347f68ecb306aaf5727a7277cb46b38fd4dc396331e9228ff31c24af1e01b516"
        "9937f749faff33c47782b025425002d37833c3182aa00d860ef2eff3280774a5"
        "808914e5c6fcba5797b460c25ff52e210b73fdeabe014b1964b99158e23e4237"
        "324adf3fa3b3e6f5054ebdc7c82ce95950bf09e06d7b0ba241f30726dfff0a47"
        "2424d85707aa927dfb9286e18de5f1a62ac6bb28a796348cb651601c97c65a9e"
        "dfda7c689690e4dc50e6e900d04bbddfe0e5a8b23e2809b783683dbd436ad824"
        "0c14734afdea55ecb3ee1cba3ba2aea9e0ada4018f28a7484b8c81d9deddf76d"
        "f54d73579b6d1874df751f8d75dc5ec005d10e30047254abf0b23b44aeb121a4";
    uint8_t public_key[264];
    uint8_t *image = g_malloc(EEPROM_SIZE);
    g_autofree char *signature = g_strdup_printf(
        "835349b63048075b3d4b91993bfc46b345564131cc6ee37344646eb55b14dfdd\n"
        "ts: 1\n"
        "rsa2048: %s\n", signature_hex);
    size_t offset;

    test_decode_hex(public_key_hex, public_key, sizeof(public_key));
    memset(image, 0xff, EEPROM_SIZE);
    offset = eeprom_add_bootsys(image);
    offset = eeprom_add_secure_dependencies(image, offset);
    offset = eeprom_add_file(image, offset, "bootconf.txt",
                             (const uint8_t *)config, strlen(config));
    offset = eeprom_add_file(image, offset, "bootconf.sig",
                             (const uint8_t *)signature,
                             strlen(signature));
    eeprom_add_file(image, offset, "pubkey.bin\0\0", public_key,
                    sizeof(public_key));
    return image;
}
void raspi4_trigger_guest_halt(QTestState *qts)
{
    const uint64_t pm_base = 0xfe100000;

    qtest_writel(qts, pm_base + 0x20, 0x5a000555);
    qtest_writel(qts, pm_base + 0x24, 0x5a00000a);
    qtest_writel(qts, pm_base + 0x1c, 0x5a000020);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
}
uint32_t rng200_xorshift32(uint32_t value)
{
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}
uint32_t bcm2711_genet_mdio_read(QTestState *qts, unsigned int phy,
                                        unsigned int reg)
{
    uint32_t command = GENET_MDIO_RD | (phy << GENET_MDIO_PHY_SHIFT) |
                       (reg << GENET_MDIO_REG_SHIFT) |
                       GENET_MDIO_START_BUSY;

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDIO_CMD, command);
    return qtest_readl(qts, BCM2711_GENET_BASE + GENET_UMAC_MDIO_CMD);
}
#ifndef _WIN32
void genet_socket_read_all(int fd, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;

    while (length) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        ssize_t received;

        g_assert_cmpint(poll(&pfd, 1, 5000), ==, 1);
        received = recv(fd, cursor, length, 0);
        g_assert_cmpint(received, >, 0);
        cursor += received;
        length -= received;
    }
}
#endif
#ifndef _WIN32
void genet_socket_send_packet(int fd, const uint8_t *packet,
                                     size_t length)
{
    uint32_t framed_length = htonl(length);

    g_assert_cmpint(send(fd, &framed_length, sizeof(framed_length), 0), ==,
                    sizeof(framed_length));
    g_assert_cmpint(send(fd, packet, length, 0), ==, length);
}
#endif
#ifndef _WIN32
size_t genet_socket_read_packet(int fd, uint8_t *packet,
                                       size_t capacity)
{
    uint32_t framed_length;
    size_t length;

    genet_socket_read_all(fd, &framed_length, sizeof(framed_length));
    length = ntohl(framed_length);
    g_assert_cmpuint(length, <=, capacity);
    genet_socket_read_all(fd, packet, length);
    return length;
}
#endif
#ifndef _WIN32
uint16_t test_ip_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;

    while (length >= 2) {
        sum += lduw_be_p(data);
        data += 2;
        length -= 2;
    }
    if (length) {
        sum += *data << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}
#endif
#ifndef _WIN32
static uint16_t test_tcp_checksum(const uint8_t *ip, const uint8_t *tcp,
                                  size_t tcp_length)
{
    g_autofree uint8_t *pseudo = g_malloc0(12 + tcp_length);

    memcpy(pseudo, ip + 12, 8);
    pseudo[9] = 6;
    stw_be_p(pseudo + 10, tcp_length);
    memcpy(pseudo + 12, tcp, tcp_length);
    return test_ip_checksum(pseudo, 12 + tcp_length);
}
#endif
#ifndef _WIN32
size_t test_make_tcp_reply(uint8_t *reply, size_t capacity,
                                  const uint8_t *request, uint32_t sequence,
                                  uint32_t acknowledgement, uint8_t flags,
                                  const uint8_t *payload,
                                  size_t payload_size)
{
    static const uint8_t server_mac[6] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x02,
    };
    const uint8_t *request_ip = request + 14;
    const uint8_t *request_tcp = request_ip + (request_ip[0] & 0xf) * 4;
    uint8_t *ip = reply + 14;
    uint8_t *tcp = ip + 20;
    size_t tcp_length = 20 + payload_size;
    size_t ip_length = 20 + tcp_length;
    size_t packet_size = 14 + ip_length;

    g_assert_cmpuint(packet_size, <=, capacity);
    memset(reply, 0, packet_size);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, server_mac, sizeof(server_mac));
    stw_be_p(reply + 12, 0x0800);
    stw_be_p(tcp, lduw_be_p(request_tcp + 2));
    stw_be_p(tcp + 2, lduw_be_p(request_tcp));
    stl_be_p(tcp + 4, sequence);
    stl_be_p(tcp + 8, acknowledgement);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    stw_be_p(tcp + 14, 64240);
    /*
     * A bare ACK, SYN-ACK or FIN carries no payload, and passing a null
     * source to memcpy is undefined even when the length is zero.
     */
    if (payload_size) {
        memcpy(tcp + 20, payload, payload_size);
    }
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    ip[8] = 64;
    ip[9] = 6;
    memcpy(ip + 12, request_ip + 16, 4);
    memcpy(ip + 16, request_ip + 12, 4);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    stw_be_p(tcp + 16, test_tcp_checksum(ip, tcp, tcp_length));
    return packet_size;
}
#endif
#ifndef _WIN32
const uint8_t *test_dhcp_option(const uint8_t *packet, size_t size,
                                       uint8_t wanted, uint8_t *value_length)
{
    const uint8_t *ip = packet + 14;
    const uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    const uint8_t *option = udp + 8 + 240;
    const uint8_t *end = packet + size;

    while (option < end && *option != 255) {
        uint8_t code = *option++;
        uint8_t length;

        if (!code) {
            continue;
        }
        g_assert_true(option < end);
        length = *option++;
        g_assert_cmpuint(length, <=, end - option);
        if (code == wanted) {
            *value_length = length;
            return option;
        }
        option += length;
    }
    *value_length = 0;
    return NULL;
}
#endif
#ifndef _WIN32
uint8_t test_dhcp_message_type(const uint8_t *packet, size_t size)
{
    uint8_t length;
    const uint8_t *value = test_dhcp_option(packet, size, 53, &length);

    return value && length == 1 ? value[0] : 0;
}
#endif
#ifndef _WIN32
size_t test_make_dhcp_reply(uint8_t *reply, size_t capacity,
                                   const uint8_t *request,
                                   uint8_t message_type)
{
    static const uint8_t server_mac[6] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x02,
    };
    const uint8_t *request_ip = request + 14;
    const uint8_t *request_udp = request_ip + (request_ip[0] & 0xf) * 4;
    const uint8_t *request_bootp = request_udp + 8;
    uint8_t *ip = reply + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    uint8_t *option = bootp + 240;
    uint8_t server_ip[4] = { 10, 0, 2, 2 };
    uint8_t subnet[4] = { 255, 255, 255, 0 };
    uint8_t gateway[4] = { 10, 0, 2, 1 };
    uint16_t udp_length;
    uint16_t ip_length;
    size_t size;

    g_assert_cmpuint(capacity, >=, 300);
    memset(reply, 0, capacity);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, server_mac, sizeof(server_mac));
    stw_be_p(reply + 12, 0x0800);
    bootp[0] = 2;
    bootp[1] = 1;
    bootp[2] = 6;
    memcpy(bootp + 4, request_bootp + 4, 4);
    stl_be_p(bootp + 16, 0x0a00020f);
    stl_be_p(bootp + 20, 0x0a000202);
    memcpy(bootp + 28, request_bootp + 28, 16);
    stl_be_p(bootp + 236, 0x63825363);
    *option++ = 53;
    *option++ = 1;
    *option++ = message_type;
    *option++ = 54;
    *option++ = sizeof(server_ip);
    memcpy(option, server_ip, sizeof(server_ip));
    option += sizeof(server_ip);
    *option++ = 1;
    *option++ = sizeof(subnet);
    memcpy(option, subnet, sizeof(subnet));
    option += sizeof(subnet);
    *option++ = 3;
    *option++ = sizeof(gateway);
    memcpy(option, gateway, sizeof(gateway));
    option += sizeof(gateway);
    *option++ = 67;
    *option++ = 7;
    memcpy(option, "unused", 6);
    option += 6;
    *option++ = 0;
    *option++ = 255;

    udp_length = option - udp;
    stw_be_p(udp, 67);
    stw_be_p(udp + 2, 68);
    stw_be_p(udp + 4, udp_length);
    ip_length = 20 + udp_length;
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, server_ip, sizeof(server_ip));
    memset(ip + 16, 0xff, 4);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    size = 14 + ip_length;
    return size;
}
#endif
#ifndef _WIN32
void test_set_dhcp_server_addresses(uint8_t *packet, size_t size,
                                           uint32_t dhcp_server,
                                           uint32_t tftp_server)
{
    uint8_t option_length;
    uint8_t *server_identifier = (uint8_t *)test_dhcp_option(
        packet, size, 54, &option_length);
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    uint8_t *bootp = udp + 8;

    g_assert_nonnull(server_identifier);
    g_assert_cmpuint(option_length, ==, 4);
    stl_be_p(server_identifier, dhcp_server);
    stl_be_p(bootp + 20, tftp_server);
}
#endif
#ifndef _WIN32
size_t test_add_overloaded_string_option(uint8_t *packet,
                                                size_t capacity,
                                                uint8_t code,
                                                const char *value)
{
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    uint8_t *bootp = udp + 8;
    uint8_t *option = udp + 8 + 240;
    uint8_t *end = packet + lduw_be_p(ip + 2) + 14;
    uint8_t *file_option = bootp + 108;
    size_t value_length = strlen(value);
    uint16_t udp_length;
    uint16_t ip_length;

    while (option < end && *option != 255) {
        if (!*option++) {
            continue;
        }
        g_assert_true(option < end);
        option += 1 + *option;
    }
    g_assert_true(option < end);
    g_assert_cmpuint(value_length, <=, UINT8_MAX);
    g_assert_cmpuint(option + 4 - packet, <=, capacity);
    g_assert_cmpuint(value_length + 3, <=, 128);
    *option++ = 52;
    *option++ = 1;
    *option++ = 1;
    *option++ = 255;
    *file_option++ = code;
    *file_option++ = value_length;
    memcpy(file_option, value, value_length);
    file_option += value_length;
    *file_option = 255;
    udp_length = option - udp;
    ip_length = 20 + udp_length;
    stw_be_p(udp + 4, udp_length);
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 10, 0);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return 14 + ip_length;
}
#endif
#ifndef _WIN32
size_t test_add_overloaded_tftp_server_name(uint8_t *packet,
                                                    size_t capacity,
                                                    const char *name)
{
    return test_add_overloaded_string_option(
        packet, capacity, 66, name);
}
#endif
#ifndef _WIN32
void test_set_dhcp_bootfile(uint8_t *packet, size_t size,
                                   const char value[7])
{
    uint8_t length;
    uint8_t *bootfile = (uint8_t *)test_dhcp_option(
        packet, size, 67, &length);

    g_assert_nonnull(bootfile);
    g_assert_cmpuint(length, ==, 7);
    memcpy(bootfile, value, 7);
}
#endif
#ifndef _WIN32
size_t test_add_dhcp_ipv4_option(uint8_t *packet, size_t capacity,
                                        uint8_t code, uint32_t address)
{
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    uint8_t *option = udp + 8 + 240;
    uint8_t *end = packet + lduw_be_p(ip + 2) + 14;
    uint16_t udp_length;
    uint16_t ip_length;

    while (option < end && *option != 255) {
        if (!*option++) {
            continue;
        }
        g_assert_true(option < end);
        option += 1 + *option;
    }
    g_assert_true(option < end);
    g_assert_cmpuint(option + 7 - packet, <=, capacity);
    *option++ = code;
    *option++ = 4;
    stl_be_p(option, address);
    option += 4;
    *option++ = 255;
    udp_length = option - udp;
    ip_length = 20 + udp_length;
    stw_be_p(udp + 4, udp_length);
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 10, 0);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return 14 + ip_length;
}
#endif
#ifndef _WIN32
size_t test_add_dhcp_bytes_option(uint8_t *packet, size_t capacity,
                                         uint8_t code, const uint8_t *value,
                                         uint8_t value_length)
{
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    uint8_t *option = udp + 8 + 240;
    uint8_t *end = packet + lduw_be_p(ip + 2) + 14;
    uint16_t udp_length;
    uint16_t ip_length;

    while (option < end && *option != 255) {
        if (!*option++) {
            continue;
        }
        g_assert_true(option < end);
        option += 1 + *option;
    }
    g_assert_true(option < end);
    g_assert_cmpuint(option + value_length + 3 - packet, <=, capacity);
    *option++ = code;
    *option++ = value_length;
    memcpy(option, value, value_length);
    option += value_length;
    *option++ = 255;
    udp_length = option - udp;
    ip_length = 20 + udp_length;
    stw_be_p(udp + 4, udp_length);
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 10, 0);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return 14 + ip_length;
}
#endif
#ifndef _WIN32
size_t test_add_conflicting_overloaded_server(uint8_t *packet,
                                                      size_t capacity,
                                                      uint32_t server)
{
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    uint8_t *bootp = udp + 8;
    uint8_t *option = bootp + 240;
    uint8_t *end = packet + lduw_be_p(ip + 2) + 14;
    uint8_t *file_option = bootp + 108;
    uint16_t udp_length;
    uint16_t ip_length;

    while (option < end && *option != 255) {
        if (!*option++) {
            continue;
        }
        g_assert_true(option < end);
        option += 1 + *option;
    }
    g_assert_true(option < end);
    g_assert_cmpuint(option + 4 - packet, <=, capacity);
    *option++ = 52;
    *option++ = 1;
    *option++ = 1;
    *option++ = 255;
    *file_option++ = 54;
    *file_option++ = 4;
    stl_be_p(file_option, server);
    file_option += 4;
    *file_option = 255;
    udp_length = option - udp;
    ip_length = 20 + udp_length;
    stw_be_p(udp + 4, udp_length);
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 10, 0);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return 14 + ip_length;
}
#endif
#ifndef _WIN32
size_t test_make_arp_reply(uint8_t reply[60],
                                  const uint8_t request[60])
{
    static const uint8_t server_mac[6] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x02,
    };
    const uint8_t *request_arp = request + 14;
    uint8_t *arp = reply + 14;

    memset(reply, 0, 60);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, server_mac, sizeof(server_mac));
    stw_be_p(reply + 12, 0x0806);
    stw_be_p(arp, 1);
    stw_be_p(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    stw_be_p(arp + 6, 2);
    memcpy(arp + 8, server_mac, sizeof(server_mac));
    memcpy(arp + 14, request_arp + 24, 4);
    memcpy(arp + 18, request_arp + 8, 6);
    memcpy(arp + 24, request_arp + 14, 4);
    return 60;
}
#endif
#ifndef _WIN32
size_t test_make_dns_reply(uint8_t *reply, size_t capacity,
                                  const uint8_t *request, size_t request_size,
                                  uint32_t address)
{
    const uint8_t *request_ip = request + 14;
    const uint8_t *request_udp = request_ip + (request_ip[0] & 0xf) * 4;
    const uint8_t *request_dns = request_udp + 8;
    size_t question_size = lduw_be_p(request_udp + 4) - 8 - 12;
    uint8_t *ip = reply + 14;
    uint8_t *udp = ip + 20;
    uint8_t *dns = udp + 8;
    uint8_t *answer = dns + 12 + question_size;
    uint16_t udp_length = 8 + 12 + question_size + 16;
    uint16_t ip_length = 20 + udp_length;
    size_t size = 14 + ip_length;

    g_assert_cmpuint(request_size, >=, 14 + 20 + 8 + 12 + question_size);
    g_assert_cmpuint(capacity, >=, size);
    memset(reply, 0, size);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, request, 6);
    stw_be_p(reply + 12, 0x0800);
    stw_be_p(udp, 53);
    stw_be_p(udp + 2, lduw_be_p(request_udp));
    stw_be_p(udp + 4, udp_length);
    memcpy(dns, request_dns, 12 + question_size);
    stw_be_p(dns + 2, 0x8180);
    stw_be_p(dns + 6, 1);
    stw_be_p(answer, 0xc00c);
    stw_be_p(answer + 2, 1);
    stw_be_p(answer + 4, 1);
    stl_be_p(answer + 6, 60);
    stw_be_p(answer + 10, 4);
    stl_be_p(answer + 12, address);
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, request_ip + 16, 4);
    memcpy(ip + 16, request_ip + 12, 4);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return size;
}
#endif
#ifndef _WIN32
size_t test_make_tftp_data(uint8_t *reply, size_t capacity,
                                  const uint8_t *request, uint16_t block,
                                  const uint8_t *data, size_t data_size)
{
    static const uint8_t server_mac[6] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x02,
    };
    const uint8_t *request_ip = request + 14;
    const uint8_t *request_udp = request_ip + (request_ip[0] & 0xf) * 4;
    uint8_t *ip = reply + 14;
    uint8_t *udp = ip + 20;
    uint8_t *tftp = udp + 8;
    uint16_t udp_length = 8 + 4 + data_size;
    uint16_t ip_length = 20 + udp_length;
    size_t size = 14 + ip_length;

    g_assert_cmpuint(size, <=, capacity);
    memset(reply, 0, capacity);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, server_mac, sizeof(server_mac));
    stw_be_p(reply + 12, 0x0800);
    stw_be_p(udp, 12345);
    stw_be_p(udp + 2, lduw_be_p(request_udp));
    stw_be_p(udp + 4, udp_length);
    stw_be_p(tftp, 3);
    stw_be_p(tftp + 2, block);
    memcpy(tftp + 4, data, data_size);
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, request_ip + 16, 4);
    memcpy(ip + 16, request_ip + 12, 4);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return size;
}
#endif
#ifndef _WIN32
void test_assert_tftp_rrq(const uint8_t *packet,
                                  const char *filename,
                                  bool options)
{
    const uint8_t *ip = packet + 14;
    const uint8_t *udp = ip + (ip[0] & 0xf) * 4;
    const uint8_t *tftp = udp + 8;
    const char *cursor = (const char *)tftp + 2;

    g_assert_cmphex(lduw_be_p(tftp), ==, 1);
    g_assert_cmpstr(cursor, ==, filename);
    cursor += strlen(cursor) + 1;
    g_assert_cmpstr(cursor, ==, "octet");
    cursor += strlen(cursor) + 1;
    if (options) {
        g_assert_cmpstr(cursor, ==, "tsize");
        cursor += strlen(cursor) + 1;
        g_assert_cmpstr(cursor, ==, "0");
        cursor += strlen(cursor) + 1;
        g_assert_cmpstr(cursor, ==, "blksize");
        cursor += strlen(cursor) + 1;
        g_assert_cmpstr(cursor, ==, "1024");
        cursor += strlen(cursor) + 1;
    }
    g_assert_cmpuint(cursor - (const char *)udp, ==,
                     lduw_be_p(udp + 4));
}
#endif
#ifndef _WIN32
static size_t test_make_tftp_oack(uint8_t *reply, size_t capacity,
                                  const uint8_t *request,
                                  uint16_t block_size,
                                  size_t transfer_size)
{
    static const uint8_t server_mac[6] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x02,
    };
    const uint8_t *request_ip = request + 14;
    const uint8_t *request_udp = request_ip + (request_ip[0] & 0xf) * 4;
    uint8_t *ip = reply + 14;
    uint8_t *udp = ip + 20;
    uint8_t *tftp = udp + 8;
    char block_value[6];
    char size_value[16];
    uint8_t *option = tftp + 2;
    uint16_t udp_length;
    uint16_t ip_length;

    memset(reply, 0, capacity);
    g_snprintf(block_value, sizeof(block_value), "%u", block_size);
    g_snprintf(size_value, sizeof(size_value), "%zu", transfer_size);
    stw_be_p(tftp, 6);
    memcpy(option, "tsize", sizeof("tsize"));
    option += sizeof("tsize");
    memcpy(option, size_value, strlen(size_value) + 1);
    option += strlen(size_value) + 1;
    memcpy(option, "blksize", sizeof("blksize"));
    option += sizeof("blksize");
    memcpy(option, block_value, strlen(block_value) + 1);
    option += strlen(block_value) + 1;
    udp_length = option - udp;
    ip_length = 20 + udp_length;
    g_assert_cmpuint(14 + ip_length, <=, capacity);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, server_mac, sizeof(server_mac));
    stw_be_p(reply + 12, 0x0800);
    stw_be_p(udp, 12345);
    stw_be_p(udp + 2, lduw_be_p(request_udp));
    stw_be_p(udp + 4, udp_length);
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, request_ip + 16, 4);
    memcpy(ip + 16, request_ip + 12, 4);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return 14 + ip_length;
}
#endif
#ifndef _WIN32
size_t test_make_tftp_error(uint8_t *reply, size_t capacity,
                                   const uint8_t *request, uint16_t code,
                                   const char *message)
{
    static const uint8_t server_mac[6] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x02,
    };
    const uint8_t *request_ip = request + 14;
    const uint8_t *request_udp = request_ip + (request_ip[0] & 0xf) * 4;
    uint8_t *ip = reply + 14;
    uint8_t *udp = ip + 20;
    uint8_t *tftp = udp + 8;
    size_t message_size = strlen(message) + 1;
    uint16_t udp_length = 8 + 4 + message_size;
    uint16_t ip_length = 20 + udp_length;
    size_t size = 14 + ip_length;

    g_assert_cmpuint(size, <=, capacity);
    memset(reply, 0, capacity);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, server_mac, sizeof(server_mac));
    stw_be_p(reply + 12, 0x0800);
    stw_be_p(udp, 12345);
    stw_be_p(udp + 2, lduw_be_p(request_udp));
    stw_be_p(udp + 4, udp_length);
    stw_be_p(tftp, 5);
    stw_be_p(tftp + 2, code);
    memcpy(tftp + 4, message, message_size);
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, request_ip + 16, 4);
    memcpy(ip + 16, request_ip + 12, 4);
    stw_be_p(ip + 10, test_ip_checksum(ip, 20));
    return size;
}
#endif
#ifndef _WIN32
size_t test_make_tftp_not_found(uint8_t *reply, size_t capacity,
                                       const uint8_t *request)
{
    return test_make_tftp_error(
        reply, capacity, request, 1, "File not found");
}
#endif
#ifndef _WIN32
void test_reject_optional_self_update(int socket_fd, uint8_t *client,
                                             size_t capacity)
{
    uint8_t server[576];
    size_t server_size;

    genet_socket_read_packet(socket_fd, client, capacity);
    test_assert_tftp_rrq(client, "pieeprom.upd", true);
    server_size = test_make_tftp_not_found(
        server, sizeof(server), client);
    genet_socket_send_packet(socket_fd, server, server_size);
}
#endif
#ifndef _WIN32
unsigned int test_transfer_tftp_files(
    int socket_fd, uint8_t *client, size_t capacity,
    const char * const *filenames, const uint8_t * const *files,
    const size_t *sizes, size_t file_count)
{
    uint8_t server[576];
    size_t server_size;
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *tftp;
    uint8_t request[576];
    unsigned int blocks = 0;

    for (size_t file = 0; file < file_count; file++) {
        size_t offset = 0;
        uint16_t block = 1;
        size_t chunk;

        genet_socket_read_packet(socket_fd, client, capacity);
        ip = client + 14;
        udp = ip + (ip[0] & 0xf) * 4;
        tftp = udp + 8;
        g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
        g_assert_cmphex(lduw_be_p(udp + 2), ==, 69);
        test_assert_tftp_rrq(client, filenames[file], true);
        g_assert_cmpuint(sizeof(request), >=, capacity);
        memcpy(request, client, capacity);
        server_size = test_make_tftp_oack(
            server, sizeof(server), request, 512, sizes[file]);
        genet_socket_send_packet(socket_fd, server, server_size);
        genet_socket_read_packet(socket_fd, client, capacity);
        ip = client + 14;
        udp = ip + (ip[0] & 0xf) * 4;
        tftp = udp + 8;
        g_assert_cmphex(lduw_be_p(tftp), ==, 4);
        g_assert_cmphex(lduw_be_p(tftp + 2), ==, 0);
        blocks++;
        if (file == 0) {
            genet_socket_send_packet(socket_fd, server, server_size);
            genet_socket_read_packet(socket_fd, client, capacity);
            ip = client + 14;
            udp = ip + (ip[0] & 0xf) * 4;
            tftp = udp + 8;
            g_assert_cmphex(lduw_be_p(tftp), ==, 4);
            g_assert_cmphex(lduw_be_p(tftp + 2), ==, 0);
            blocks++;
        }
        do {
            chunk = MIN(sizes[file] - offset, (size_t)512);
            server_size = test_make_tftp_data(
                server, sizeof(server), request, block,
                files[file] + offset, chunk);
            genet_socket_send_packet(socket_fd, server, server_size);

            genet_socket_read_packet(socket_fd, client, capacity);
            ip = client + 14;
            udp = ip + (ip[0] & 0xf) * 4;
            tftp = udp + 8;
            g_assert_cmphex(lduw_be_p(udp + 2), ==, 12345);
            g_assert_cmphex(lduw_be_p(tftp), ==, 4);
            g_assert_cmphex(lduw_be_p(tftp + 2), ==, block);
            offset += chunk;
            block++;
            blocks++;
        } while (chunk == 512);
    }
    return blocks;
}
#endif
#ifndef _WIN32
static size_t test_http_read_tcp_packet(int socket_fd, uint8_t *packet,
                                        size_t capacity)
{
    uint8_t arp_reply[60];

    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        size_t size = genet_socket_read_packet(
            socket_fd, packet, capacity);
        uint16_t ether_type;

        g_assert_cmpuint(size, >=, 14);
        ether_type = lduw_be_p(packet + 12);
        /*
         * QEMU may announce the migrated NIC with one RARP frame before
         * resuming the retained TCP flight.  It is link-state traffic, not
         * part of the HTTP exchange.
         */
        if (ether_type == 0x8035) {
            continue;
        }
        if (ether_type == 0x0806) {
            g_assert_cmpuint(size, >=, 42);
            g_assert_cmphex(lduw_be_p(packet + 20), ==, 1);
            genet_socket_send_packet(
                socket_fd, arp_reply,
                test_make_arp_reply(arp_reply, packet));
            continue;
        }
        g_assert_cmphex(ether_type, ==, 0x0800);
        g_assert_cmpuint(size, >=, 14 + 20);
        g_assert_cmphex(packet[14 + 9], ==, 6);
        return size;
    }

    g_error("HTTP peer did not emit TCP after bounded RARP/ARP refresh");
}
#endif
#ifndef _WIN32
void test_transfer_http_file(QTestState *qts, int socket_fd,
                                    uint8_t *client,
                                    size_t client_capacity,
                                    const char *filename,
                                    const char *host,
                                    const uint8_t *contents, size_t size,
                                    uint32_t server_sequence,
                                    bool drop_syn, bool drop_get,
                                    bool reorder_response,
                                    bool fin_response)
{
    uint8_t server[1600];
    const uint8_t *ip;
    const uint8_t *tcp;
    const uint8_t *request_payload;
    size_t client_size;
    size_t server_size;
    size_t request_size;
    uint32_t client_sequence;
    uint32_t client_ack;
    g_autofree char *header = NULL;
    g_autofree uint8_t *response = NULL;
    size_t response_size;
    size_t offset = 0;

    client_size = test_http_read_tcp_packet(
        socket_fd, client, client_capacity);
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(ip[9], ==, 6);
    g_assert_true(tcp[13] & 0x02);
    client_sequence = ldl_be_p(tcp + 4);
    if (drop_syn) {
        qtest_clock_step(qts, INT64_C(500) * 1000 * 1000);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_true(tcp[13] & 0x02);
        g_assert_cmphex(ldl_be_p(tcp + 4), ==, client_sequence);
    }
    server_size = test_make_tcp_reply(
        server, sizeof(server), client, server_sequence,
        client_sequence + 1, 0x12, NULL, 0);
    genet_socket_send_packet(socket_fd, server, server_size);

    client_size = test_http_read_tcp_packet(
        socket_fd, client, client_capacity);
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    request_payload = tcp + (tcp[12] >> 4) * 4;
    request_size = client + client_size - request_payload;
    g_assert_true(tcp[13] & 0x08);
    g_assert_nonnull(g_strstr_len(
        (const char *)request_payload, request_size, filename));
    {
        g_autofree char *host_header = g_strdup_printf("Host: %s\r\n", host);

        g_assert_nonnull(g_strstr_len(
            (const char *)request_payload, request_size, host_header));
    }
    if (drop_get) {
        g_autofree uint8_t *first_request = g_memdup2(
            request_payload, request_size);
        uint32_t first_sequence = ldl_be_p(tcp + 4);

        qtest_clock_step(qts, INT64_C(500) * 1000 * 1000);
        client_size = test_http_read_tcp_packet(
            socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        request_payload = tcp + (tcp[12] >> 4) * 4;
        size_t retransmit_request_size =
            (client + client_size) - request_payload;

        g_assert_cmphex(ldl_be_p(tcp + 4), ==, first_sequence);
        g_assert_cmpmem(request_payload, retransmit_request_size,
                        first_request, request_size);
    }
    client_sequence = ldl_be_p(tcp + 4) + request_size;
    client_ack = server_sequence + 1;
    header = g_strdup_printf(
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
        "Content-Type: application/octet-stream\r\nConnection: close\r\n"
        "\r\n", size);
    response_size = strlen(header) + size;
    response = g_malloc(response_size);
    memcpy(response, header, strlen(header));
    memcpy(response + strlen(header), contents, size);

    if (reorder_response) {
        enum { segment_size = 700 };

        g_assert_cmpuint(response_size, >, 4 * segment_size);
        server_size = test_make_tcp_reply(
            server, sizeof(server), client, client_ack + 3 * segment_size,
            client_sequence, 0x18, response + 3 * segment_size,
            segment_size);
        genet_socket_send_packet(socket_fd, server, server_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(ldl_be_p(tcp + 8), ==, client_ack);

        server_size = test_make_tcp_reply(
            server, sizeof(server), client, client_ack + segment_size,
            client_sequence, 0x18, response + segment_size, segment_size);
        genet_socket_send_packet(socket_fd, server, server_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(ldl_be_p(tcp + 8), ==, client_ack);

        server_size = test_make_tcp_reply(
            server, sizeof(server), client, client_ack, client_sequence,
            0x18, response, segment_size);
        genet_socket_send_packet(socket_fd, server, server_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                        client_ack + segment_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                        client_ack + 2 * segment_size);

        server_size = test_make_tcp_reply(
            server, sizeof(server), client,
            client_ack + 2 * segment_size, client_sequence, 0x18,
            response + 2 * segment_size, segment_size);
        genet_socket_send_packet(socket_fd, server, server_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                        client_ack + 3 * segment_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                        client_ack + 4 * segment_size);
        client_ack += 4 * segment_size;
        offset = 4 * segment_size;
    }

    while (offset < response_size) {
        uint8_t payload[1400];
        size_t payload_size = MIN(sizeof(payload), response_size - offset);
        bool final = offset + payload_size == response_size;

        memcpy(payload, response + offset, payload_size);
        server_size = test_make_tcp_reply(
            server, sizeof(server), client, client_ack, client_sequence,
            final && fin_response ? 0x19 : 0x18, payload, payload_size);
        genet_socket_send_packet(socket_fd, server, server_size);
        test_http_read_tcp_packet(socket_fd, client, client_capacity);
        ip = client + 14;
        tcp = ip + (ip[0] & 0xf) * 4;
        g_assert_cmphex(tcp[13] & 0x10, ==, 0x10);
        g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                        client_ack + payload_size);
        if (final && fin_response) {
            test_http_read_tcp_packet(socket_fd, client, client_capacity);
            ip = client + 14;
            tcp = ip + (ip[0] & 0xf) * 4;
            g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                            client_ack + payload_size + 1);
        }
        client_ack += payload_size;
        if (final && fin_response) {
            client_ack++;
        }
        offset += payload_size;
    }
    test_http_read_tcp_packet(socket_fd, client, client_capacity);
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(tcp[13], ==, 0x11);
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, client_ack);
}
#endif
#ifndef _WIN32
#ifdef CONFIG_TASN1
ssize_t test_tls_pull(gnutls_transport_ptr_t opaque,
                             void *buf, size_t size)
{
    TestTlsTransport *transport = opaque;
    size_t available = transport->rx->len - transport->rx_offset;
    size_t copied;

    if (!available) {
        errno = EAGAIN;
        return -1;
    }
    copied = MIN(available, size);
    memcpy(buf, transport->rx->data + transport->rx_offset, copied);
    transport->rx_offset += copied;
    if (transport->rx_offset == transport->rx->len) {
        g_byte_array_set_size(transport->rx, 0);
        transport->rx_offset = 0;
    }
    return copied;
}
#endif
#endif
#ifndef _WIN32
#ifdef CONFIG_TASN1
ssize_t test_tls_push(gnutls_transport_ptr_t opaque,
                             const void *buf, size_t size)
{
    TestTlsTransport *transport = opaque;

    g_byte_array_append(transport->tx, buf, size);
    return size;
}
#endif
#endif
#ifndef _WIN32
#ifdef CONFIG_TASN1
void test_tls_feed_client_packet(
    int socket_fd, TestTlsTransport *transport, uint8_t *client,
    size_t client_size, uint8_t *server, size_t server_capacity,
    uint32_t *server_sequence, uint32_t *client_sequence)
{
    const uint8_t *ip = client + 14;
    const uint8_t *tcp = ip + (ip[0] & 0xf) * 4;
    const uint8_t *payload = tcp + (tcp[12] >> 4) * 4;
    size_t payload_size = client + client_size - payload;
    bool fin = tcp[13] & 0x01;
    size_t server_size;

    if (tcp[13] & 0x02) {
        transport->saw_syn = true;
        return;
    }
    if (!payload_size && !fin) {
        return;
    }
    g_assert_cmphex(ldl_be_p(tcp + 4), ==, *client_sequence);
    if (payload_size) {
        g_byte_array_append(transport->rx, payload, payload_size);
    }
    *client_sequence += payload_size;
    if (fin) {
        transport->saw_fin = true;
        (*client_sequence)++;
    }
    server_size = test_make_tcp_reply(
        server, server_capacity, client, *server_sequence,
        *client_sequence, 0x10, NULL, 0);
    genet_socket_send_packet(socket_fd, server, server_size);
}
#endif
#endif
#ifndef _WIN32
#ifdef CONFIG_TASN1
void test_tls_send_server_flight(
    int socket_fd, TestTlsTransport *transport, uint8_t *client,
    size_t client_capacity, uint8_t *server, size_t server_capacity,
    uint32_t *server_sequence, uint32_t *client_sequence)
{
    size_t offset = 0;

    while (offset < transport->tx->len) {
        size_t payload_size = MIN(
            transport->tx->len - offset, (size_t)1400);
        size_t server_size = test_make_tcp_reply(
            server, server_capacity, client, *server_sequence,
            *client_sequence, 0x18, transport->tx->data + offset,
            payload_size);
        struct pollfd pollfd = {
            .fd = socket_fd,
            .events = POLLIN,
        };
        const uint8_t *ip;
        const uint8_t *tcp;
        uint32_t sent_sequence = *server_sequence;
        uint32_t target_sequence = sent_sequence + payload_size;

        genet_socket_send_packet(socket_fd, server, server_size);
        *server_sequence = target_sequence;
        for (;;) {
            size_t client_size = genet_socket_read_packet(
                socket_fd, client, client_capacity);
            size_t client_payload_size;

            ip = client + 14;
            tcp = ip + (ip[0] & 0xf) * 4;
            client_payload_size =
                client + client_size -
                (tcp + (tcp[12] >> 4) * 4);
            g_assert_cmphex(tcp[13] & 0x10, ==, 0x10);
            if (client_payload_size) {
                test_tls_feed_client_packet(
                    socket_fd, transport, client, client_size,
                    server, server_capacity, server_sequence,
                    client_sequence);
            }
            if ((int32_t)(ldl_be_p(tcp + 8) -
                          target_sequence) >= 0) {
                break;
            }
        }
        while (poll(&pollfd, 1, 0) > 0) {
            size_t client_size = genet_socket_read_packet(
                socket_fd, client, client_capacity);

            test_tls_feed_client_packet(
                socket_fd, transport, client, client_size,
                server, server_capacity, server_sequence, client_sequence);
            pollfd.revents = 0;
        }
        offset += payload_size;
    }
    g_byte_array_set_size(transport->tx, 0);
}
#endif
#endif
#ifndef _WIN32
#ifdef CONFIG_TASN1
bool test_tls_drain_client(
    int socket_fd, TestTlsTransport *transport, uint8_t *client,
    size_t client_capacity, uint8_t *server, size_t server_capacity,
    uint32_t *server_sequence, uint32_t *client_sequence)
{
    struct pollfd pollfd = {
        .fd = socket_fd,
        .events = POLLIN,
    };
    bool received = false;

    while (poll(&pollfd, 1, received ? 0 : 100) > 0) {
        size_t client_size = genet_socket_read_packet(
            socket_fd, client, client_capacity);

        test_tls_feed_client_packet(
            socket_fd, transport, client, client_size,
            server, server_capacity, server_sequence, client_sequence);
        received = true;
        pollfd.revents = 0;
    }
    return received;
}
#endif
#endif
#ifndef _WIN32
void test_secure_http_error_case(const uint8_t *response,
                                        size_t response_size,
                                        uint8_t response_flags,
                                        const char *expected_status)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    uint8_t key_hash[32];
    g_autofree uint8_t *eeprom = make_signed_eeprom(
        false, false, false, 2, TEST_EEPROM_DUPLICATE_NONE);
    g_autofree uint8_t *otp = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *firmware_status = NULL;
    uint8_t client[1600];
    uint8_t server[1600];
    const uint8_t *ip;
    const uint8_t *tcp;
    const uint8_t *request_payload;
    size_t client_size;
    size_t request_size;
    size_t server_size;
    uint32_t client_sequence;
    int sockets[2];
    QTestState *qts;

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    otp = make_secure_otp_image(key_hash);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
        ",network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=piotp,format=raw,file=%s "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, otp_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    client_sequence = ldl_be_p(tcp + 4);
    server_size = test_make_tcp_reply(
        server, sizeof(server), client, 0x10203040,
        client_sequence + 1, 0x12, NULL, 0);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    request_payload = tcp + (tcp[12] >> 4) * 4;
    request_size = (client + client_size) - request_payload;
    g_assert_nonnull(g_strstr_len(
        (const char *)request_payload, request_size,
        "net_install/boot.sig"));
    client_sequence = ldl_be_p(tcp + 4) + request_size;
    server_size = test_make_tcp_reply(
        server, sizeof(server), client, 0x10203041,
        client_sequence, response_flags, response, response_size);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));

    state = qom_get_string(qts, "boot-state");
    firmware_status = qom_get_string(qts, "firmware-status");
    g_assert_cmpstr(state, ==, "restart-loop");
    g_assert_cmpstr(firmware_status, ==, expected_status);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(otp_path);
}
#endif
#ifndef _WIN32
static unsigned int test_transfer_tftp_file_with_dropped_ack(
    QTestState *qts, int socket_fd, uint8_t *client, size_t capacity,
    const char *filename, const uint8_t *file, size_t size)
{
    uint8_t request[576];
    uint8_t server[576];
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *tftp;
    size_t offset = 0;
    uint16_t block = 1;
    size_t chunk;
    size_t server_size;
    unsigned int blocks = 0;

    g_assert_cmpuint(size, >, 512);
    genet_socket_read_packet(socket_fd, client, capacity);
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 69);
    g_assert_cmphex(lduw_be_p(tftp), ==, 1);
    g_assert_cmpstr((const char *)tftp + 2, ==, filename);
    g_assert_cmpuint(sizeof(request), >=, capacity);
    memcpy(request, client, capacity);

    chunk = 512;
    server_size = test_make_tftp_data(
        server, sizeof(server), request, block, file, chunk);
    genet_socket_send_packet(socket_fd, server, server_size);
    genet_socket_read_packet(socket_fd, client, capacity);
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(lduw_be_p(tftp), ==, 4);
    g_assert_cmphex(lduw_be_p(tftp + 2), ==, block);

    qtest_clock_step(qts, INT64_C(500) * 1000 * 1000);
    genet_socket_read_packet(socket_fd, client, capacity);
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(lduw_be_p(tftp), ==, 4);
    g_assert_cmphex(lduw_be_p(tftp + 2), ==, block);

    offset = chunk;
    block++;
    blocks++;

    server_size = test_make_tftp_data(
        server, sizeof(server), client, block, file + offset, 1);
    stw_be_p(server + 14 + 20, 23456);
    genet_socket_send_packet(socket_fd, server, server_size);
    genet_socket_read_packet(socket_fd, client, capacity);
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 23456);
    g_assert_cmphex(lduw_be_p(tftp), ==, 5);
    g_assert_cmphex(lduw_be_p(tftp + 2), ==, 5);
    g_assert_cmpstr((const char *)tftp + 4, ==, "Unknown transfer ID");

    do {
        chunk = MIN(size - offset, (size_t)512);
        server_size = test_make_tftp_data(
            server, sizeof(server), request, block, file + offset, chunk);
        genet_socket_send_packet(socket_fd, server, server_size);
        genet_socket_read_packet(socket_fd, client, capacity);
        ip = client + 14;
        udp = ip + (ip[0] & 0xf) * 4;
        tftp = udp + 8;
        g_assert_cmphex(lduw_be_p(tftp), ==, 4);
        g_assert_cmphex(lduw_be_p(tftp + 2), ==, block);
        offset += chunk;
        block++;
        blocks++;
    } while (chunk == 512);
    return blocks;
}
#endif
#ifndef _WIN32
unsigned int test_complete_response_tftp(
    QTestState *qts, int socket_fd, uint8_t *client, size_t capacity,
    const uint8_t *start, size_t start_size, uint32_t expected_server_ip,
    bool finish_dally, bool begin_at_config)
{
    static const uint8_t config[] =
        TEST_NETWORK_CONFIG "os_prefix=os/\n";
    static const uint8_t include[] = TEST_NETWORK_INCLUDE;
    static const uint8_t initramfs_a[] = TEST_NETWORK_INITRAMFS_A;
    static const uint8_t initramfs_b[] = TEST_NETWORK_INITRAMFS_B;
    static const uint8_t fixup[] = "QEMU USB fixup fixture";
    static const uint8_t cmdline[] =
        "console=ttyAMA0 root=/dev/sda2 rootwait\n";
    static const char * const config_filenames[] = {
        "config.txt", "extra.txt",
    };
    static const char * const firmware_filenames[] = {
        "start.elf", "fixup.dat",
    };
    static const char * const payload_filenames[] = {
        "kernel8.img", "bcm2711-rpi-4-b.dtb", "cmdline.txt",
        "initramfs-a", "initramfs-b",
    };
    static const char * const overlay_filenames[] = {
        "overlays/test.dtbo",
    };
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree uint8_t *overlay = NULL;
    const uint8_t *config_files[] = { config, include };
    const size_t config_sizes[] = {
        sizeof(config) - 1, sizeof(include) - 1,
    };
    const uint8_t *firmware_files[] = { start, fixup };
    const size_t firmware_sizes[] = { start_size, sizeof(fixup) };
    const uint8_t *payload_files[] = {
        kernel, NULL, cmdline, initramfs_a, initramfs_b,
    };
    size_t payload_sizes[] = {
        TEST_KERNEL_SIZE, 0, sizeof(cmdline), sizeof(initramfs_a),
        sizeof(initramfs_b),
    };
    const uint8_t *overlay_files[1];
    size_t overlay_sizes[1];
    uint8_t server[576];
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *tftp;
    size_t server_size;
    unsigned int blocks;
    uint16_t final_block;
    size_t final_offset;
    size_t final_chunk;
    g_autofree char *state = NULL;

    if (!begin_at_config) {
        for (unsigned int attempt = 0; attempt < 2; attempt++) {
            genet_socket_read_packet(socket_fd, client, capacity);
            if (lduw_be_p(client + 12) != 0x8035) {
                break;
            }
        }
        g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
        g_assert_cmphex(ldl_be_p(client + 14 + 24), ==,
                        expected_server_ip);
        server_size = test_make_arp_reply(server, client);
        genet_socket_send_packet(socket_fd, server, server_size);
        test_reject_optional_self_update(socket_fd, client, capacity);
    }
    genet_socket_read_packet(socket_fd, client, capacity);
    g_assert_cmpstr((const char *)client + 14 + 20 + 8 + 2, ==,
                    "config.txt");
    qtest_clock_step(qts, INT64_C(500) * 1000 * 1000);

    device_tree = make_device_tree(&payload_sizes[1]);
    payload_files[1] = device_tree;
    overlay = make_device_tree_overlay(&overlay_sizes[0]);
    overlay_files[0] = overlay;
    blocks = test_transfer_tftp_files(
        socket_fd, client, capacity, config_filenames, config_files,
        config_sizes, ARRAY_SIZE(config_files));
    genet_socket_read_packet(socket_fd, client, capacity);
    g_assert_cmpstr((const char *)client + 14 + 20 + 8 + 2, ==,
                    "start4.elf");
    server_size = test_make_tftp_not_found(server, sizeof(server), client);
    genet_socket_send_packet(socket_fd, server, server_size);
    if (start_size > 512) {
        blocks += test_transfer_tftp_file_with_dropped_ack(
            qts, socket_fd, client, capacity, firmware_filenames[0],
            firmware_files[0], firmware_sizes[0]);
        blocks += test_transfer_tftp_files(
            socket_fd, client, capacity, &firmware_filenames[1],
            &firmware_files[1], &firmware_sizes[1], 1);
    } else {
        blocks += test_transfer_tftp_files(
            socket_fd, client, capacity, firmware_filenames, firmware_files,
            firmware_sizes, ARRAY_SIZE(firmware_files));
    }
    genet_socket_read_packet(socket_fd, client, capacity);
    g_assert_cmpstr((const char *)client + 14 + 20 + 8 + 2, ==,
                    "os/kernel8.img");
    server_size = test_make_tftp_not_found(server, sizeof(server), client);
    genet_socket_send_packet(socket_fd, server, server_size);
    blocks += test_transfer_tftp_files(
        socket_fd, client, capacity, payload_filenames, payload_files,
        payload_sizes, ARRAY_SIZE(payload_files));

    genet_socket_read_packet(socket_fd, client, capacity);
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(lduw_be_p(tftp), ==, 1);
    g_assert_cmpstr((const char *)tftp + 2, ==,
                    "overlays/overlay_map.dtb");
    server_size = test_make_tftp_not_found(
        server, sizeof(server), client);
    genet_socket_send_packet(socket_fd, server, server_size);
    blocks += test_transfer_tftp_files(
        socket_fd, client, capacity, overlay_filenames, overlay_files,
        overlay_sizes, ARRAY_SIZE(overlay_files));
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-dally");
    if (!finish_dally) {
        return blocks;
    }

    final_block = overlay_sizes[0] / 512 + 1;
    final_offset = (final_block - 1) * 512;
    final_chunk = overlay_sizes[0] - final_offset;
    server_size = test_make_tftp_data(
        server, sizeof(server), client, final_block,
        overlay + final_offset, final_chunk);
    genet_socket_send_packet(socket_fd, server, server_size);
    genet_socket_read_packet(socket_fd, client, capacity);
    tftp = client + 14 + 20 + 8;
    g_assert_cmphex(lduw_be_p(tftp), ==, 4);
    g_assert_cmphex(lduw_be_p(tftp + 2), ==, final_block);

    qtest_clock_step(qts, INT64_C(499) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-dally");
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000);
    return blocks;
}
#endif
#ifndef _WIN32
void test_transfer_response_tftp_tail(int socket_fd, uint8_t *client,
                                             size_t capacity)
{
    static const uint8_t initramfs_a[] = TEST_NETWORK_INITRAMFS_A;
    static const uint8_t initramfs_b[] = TEST_NETWORK_INITRAMFS_B;
    static const uint8_t cmdline[] =
        "console=ttyAMA0 root=/dev/sda2 rootwait\n";
    static const char * const filenames[] = {
        "bcm2711-rpi-4-b.dtb", "cmdline.txt",
        "initramfs-a", "initramfs-b",
    };
    static const char * const overlay_filenames[] = {
        "overlays/test.dtbo",
    };
    g_autofree uint8_t *device_tree = NULL;
    g_autofree uint8_t *overlay = NULL;
    const uint8_t *files[] = {
        NULL, cmdline, initramfs_a, initramfs_b,
    };
    size_t sizes[] = {
        0, sizeof(cmdline), sizeof(initramfs_a), sizeof(initramfs_b),
    };
    const uint8_t *overlay_files[1];
    size_t overlay_sizes[1];
    uint8_t server[576];
    size_t server_size;

    device_tree = make_device_tree(&sizes[0]);
    files[0] = device_tree;
    overlay = make_device_tree_overlay(&overlay_sizes[0]);
    overlay_files[0] = overlay;
    test_transfer_tftp_files(socket_fd, client, capacity, filenames, files,
                             sizes, ARRAY_SIZE(files));
    genet_socket_read_packet(socket_fd, client, capacity);
    g_assert_cmpstr((const char *)client + 14 + 20 + 8 + 2, ==,
                    "overlays/overlay_map.dtb");
    server_size = test_make_tftp_not_found(server, sizeof(server), client);
    genet_socket_send_packet(socket_fd, server, server_size);
    test_transfer_tftp_files(socket_fd, client, capacity,
                             overlay_filenames, overlay_files,
                             overlay_sizes, ARRAY_SIZE(overlay_files));
}
#endif
#ifndef _WIN32
void genet_wait_register(QTestState *qts, uint64_t address,
                                uint32_t expected)
{
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        if (qtest_readl(qts, address) == expected) {
            return;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readl(qts, address), ==, expected);
}
#endif
void bcm2711_property_call_response_bytes(
    QTestState *qts, uint32_t tag, uint32_t *payload, size_t words,
    size_t response_bytes)
{
    uint8_t request[8 + 12 + 6 * sizeof(uint32_t) + sizeof(uint32_t)] = { 0 };
    size_t payload_size = words * sizeof(uint32_t);
    size_t total_size = 8 + 12 + payload_size + sizeof(uint32_t);

    g_assert_cmpuint(words, <=, 6);
    g_assert_cmpuint(response_bytes, <=, payload_size);
    stl_le_p(request, total_size);
    stl_le_p(request + 8, tag);
    stl_le_p(request + 12, payload_size);
    stl_le_p(request + 16, 0);
    for (size_t i = 0; i < words; i++) {
        stl_le_p(request + 20 + i * sizeof(uint32_t), payload[i]);
    }
    qtest_memwrite(qts, PROPERTY_BUFFER_ADDRESS, request, total_size);
    qtest_writel(qts, BCM2711_MAILBOX_WRITE,
                 PROPERTY_BUFFER_ADDRESS | PROPERTY_CHANNEL);
    g_assert_cmphex(qtest_readl(qts, BCM2711_MAILBOX_READ), ==,
                    PROPERTY_BUFFER_ADDRESS | PROPERTY_CHANNEL);
    qtest_memread(qts, PROPERTY_BUFFER_ADDRESS, request, total_size);
    g_assert_cmpuint((uint32_t)ldl_le_p(request + 4), ==, 0x80000000U);
    g_assert_cmpuint((uint32_t)ldl_le_p(request + 8), ==, tag);
    g_assert_cmpuint((uint32_t)ldl_le_p(request + 16), ==,
                     0x80000000U | response_bytes);
    for (size_t i = 0; i < words; i++) {
        payload[i] = ldl_le_p(request + 20 + i * sizeof(uint32_t));
    }
}
void bcm2711_property_call_response(QTestState *qts, uint32_t tag,
                                           uint32_t *payload, size_t words,
                                           size_t response_words)
{
    bcm2711_property_call_response_bytes(
        qts, tag, payload, words, response_words * sizeof(uint32_t));
}
void bcm2711_property_call(QTestState *qts, uint32_t tag,
                                  uint32_t *payload, size_t words)
{
    bcm2711_property_call_response(qts, tag, payload, words, words);
}
void bcm2711_property_raw_exchange(QTestState *qts, uint8_t *request,
                                           size_t size)
{
    qtest_memwrite(qts, PROPERTY_BUFFER_ADDRESS, request, size);
    qtest_writel(qts, BCM2711_MAILBOX_WRITE,
                 PROPERTY_BUFFER_ADDRESS | PROPERTY_CHANNEL);
    g_assert_cmphex(qtest_readl(qts, BCM2711_MAILBOX_READ), ==,
                    PROPERTY_BUFFER_ADDRESS | PROPERTY_CHANNEL);
    qtest_memread(qts, PROPERTY_BUFFER_ADDRESS, request, size);
}
uint32_t bcm2711_firmware_edid_call(
    QTestState *qts, uint32_t tag, uint32_t block, uint32_t port,
    uint8_t edid[128])
{
    uint8_t request[160];

    memset(request, 0xa5, sizeof(request));
    stl_le_p(request, sizeof(request));
    stl_le_p(request + 4, 0);
    stl_le_p(request + 8, tag);
    stl_le_p(request + 12, 136);
    stl_le_p(request + 16, 0);
    stl_le_p(request + 20, block);
    stl_le_p(request + 24, port);
    stl_le_p(request + 156, RPI_FWREQ_PROPERTY_END);
    bcm2711_property_raw_exchange(qts, request, sizeof(request));

    g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
    g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000088);
    g_assert_cmphex((uint32_t)ldl_le_p(request + 20), ==, block);
    g_assert_cmphex((uint32_t)ldl_le_p(request + 156), ==,
                    RPI_FWREQ_PROPERTY_END);
    memcpy(edid, request + 28, 128);
    return ldl_le_p(request + 24);
}
bool bcm2711_hdmi_i2c_transfer(QTestState *qts, uint64_t base,
                                      uint8_t address, bool read,
                                      uint8_t *data, size_t length)
{
    uint32_t status;

    g_assert_cmpuint(length, <=, 32);
    qtest_writel(qts, base + HDMI_I2C_CHIP_ADDRESS,
                 address << 1 | read);
    qtest_writel(qts, base + HDMI_I2C_COUNT, length);
    qtest_writel(qts, base + HDMI_I2C_CONTROL,
                 read ? HDMI_I2C_READ : 0);
    if (!read) {
        for (size_t offset = 0; offset < length; offset += 4) {
            uint32_t word = 0;

            for (size_t byte = 0; byte < MIN((size_t)4, length - offset);
                 byte++) {
                word |= (uint32_t)data[offset + byte] << (byte * 8);
            }
            qtest_writel(qts, base + HDMI_I2C_DATA_IN + offset, word);
        }
    }
    qtest_writel(qts, base + HDMI_I2C_ENABLE, 1);
    status = qtest_readl(qts, base + HDMI_I2C_ENABLE);
    g_assert_true(status & HDMI_I2C_INTERRUPT);
    if (!(status & HDMI_I2C_NOACK) && read) {
        for (size_t offset = 0; offset < length; offset += 4) {
            uint32_t word = qtest_readl(
                qts, base + HDMI_I2C_DATA_OUT + offset);

            for (size_t byte = 0; byte < MIN((size_t)4, length - offset);
                 byte++) {
                data[offset + byte] = extract32(word, byte * 8, 8);
            }
        }
    }
    qtest_writel(qts, base + HDMI_I2C_ENABLE, 0);
    return !(status & HDMI_I2C_NOACK);
}
void bcm2711_hdmi_i2c_set_pointer(QTestState *qts, uint64_t base,
                                         uint8_t segment, uint8_t offset)
{
    g_assert_true(bcm2711_hdmi_i2c_transfer(
        qts, base, 0x30, false, &segment, 1));
    g_assert_true(bcm2711_hdmi_i2c_transfer(
        qts, base, 0x50, false, &offset, 1));
}
void bcm2711_hdmi_i2c_read(QTestState *qts, uint64_t base,
                                  uint8_t segment, uint8_t offset,
                                  uint8_t *data, size_t length)
{
    bcm2711_hdmi_i2c_set_pointer(qts, base, segment, offset);
    while (length) {
        size_t chunk = MIN(length, (size_t)32);

        g_assert_true(bcm2711_hdmi_i2c_transfer(
            qts, base, 0x50, true, data, chunk));
        data += chunk;
        length -= chunk;
    }
}
void make_firmware_display_timing(
    uint8_t timing[36], uint8_t display_id, uint32_t clock,
    uint16_t hdisplay, uint16_t hfront, uint16_t hsync, uint16_t htotal,
    uint16_t vdisplay, uint16_t vfront, uint16_t vsync, uint16_t vtotal,
    uint32_t flags)
{
    memset(timing, 0, 36);
    timing[0] = display_id;
    stl_le_p(timing + 4, clock);
    stw_le_p(timing + 8, hdisplay);
    stw_le_p(timing + 10, hdisplay + hfront);
    stw_le_p(timing + 12, hdisplay + hfront + hsync);
    stw_le_p(timing + 14, htotal);
    stw_le_p(timing + 18, vdisplay);
    stw_le_p(timing + 20, vdisplay + vfront);
    stw_le_p(timing + 22, vdisplay + vfront + vsync);
    stw_le_p(timing + 24, vtotal);
    stw_le_p(timing + 28,
             ((uint64_t)clock * 1000 + (uint64_t)htotal * vtotal / 2) /
             ((uint64_t)htotal * vtotal));
    stl_le_p(timing + 32, flags);
}
void bcm2711_firmware_display_timing_call(QTestState *qts,
                                                 uint32_t tag,
                                                 uint8_t timing[36])
{
    uint8_t request[60] = { 0 };

    stl_le_p(request, sizeof(request));
    stl_le_p(request + 8, tag);
    stl_le_p(request + 12, 36);
    memcpy(request + 20, timing, 36);
    bcm2711_property_raw_exchange(qts, request, sizeof(request));

    g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
    g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000024);
    g_assert_cmphex((uint32_t)ldl_le_p(request + 56), ==,
                    RPI_FWREQ_PROPERTY_END);
    memcpy(timing, request + 20, 36);
}
void bcm2711_firmware_display_timing_get(QTestState *qts,
                                                uint8_t display_id,
                                                uint8_t timing[36])
{
    memset(timing, 0, 36);
    timing[0] = display_id;
    bcm2711_firmware_display_timing_call(
        qts, RPI_FWREQ_GET_DISPLAY_TIMING, timing);
}
uint32_t bcm2711_get_throttled(QTestState *qts)
{
    uint32_t payload[1] = { 0 };

    bcm2711_property_call(
        qts, RPI_FWREQ_GET_THROTTLED, payload, G_N_ELEMENTS(payload));
    return payload[0];
}
uint32_t bcm2711_get_firmware_temperature(QTestState *qts,
                                                  uint32_t tag)
{
    uint32_t payload[2] = { 0 };

    bcm2711_property_call(qts, tag, payload, G_N_ELEMENTS(payload));
    g_assert_cmphex(payload[0], ==, 0);
    return payload[1];
}
uint32_t bcm2711_firmware_clock_call(QTestState *qts, uint32_t tag,
                                             uint32_t clock_id,
                                             uint32_t value)
{
    uint32_t payload[3] = { clock_id, value, 1 };
    size_t words = tag == RPI_FWREQ_SET_CLOCK_RATE ? 3 : 2;

    bcm2711_property_call_response(qts, tag, payload, words, 2);
    g_assert_cmphex(payload[0], ==, clock_id);
    return payload[1];
}
uint32_t bcm2711_firmware_power_call(QTestState *qts, uint32_t tag,
                                             uint32_t device_id,
                                             uint32_t value)
{
    uint32_t payload[2] = { device_id, value };

    bcm2711_property_call_response(
        qts, tag, payload, G_N_ELEMENTS(payload), G_N_ELEMENTS(payload));
    g_assert_cmphex(payload[0], ==, device_id);
    return payload[1];
}
uint32_t bcm2711_firmware_framebuffer_scalar(
    QTestState *qts, uint32_t tag, uint32_t value)
{
    bcm2711_property_call(qts, tag, &value, 1);
    return value;
}
int64_t bcm2711_firmware_vsync_submit(QTestState *qts,
                                             uint32_t refresh_hz)
{
    uint8_t request[28] = { 0 };
    int64_t period = DIV_ROUND_UP(INT64_C(1000000000), refresh_hz);
    int64_t now = qtest_clock_step(qts, 1);
    int64_t deadline = DIV_ROUND_UP(now + 1, period) * period;

    stl_le_p(request, sizeof(request));
    stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_SET_VSYNC);
    stl_le_p(request + 12, sizeof(uint32_t));
    stl_le_p(request + 20, 0);
    qtest_memwrite(qts, PROPERTY_BUFFER_ADDRESS, request, sizeof(request));
    qtest_writel(qts, BCM2711_MAILBOX_WRITE,
                 PROPERTY_BUFFER_ADDRESS | PROPERTY_CHANNEL);
    g_assert_true(qtest_readl(qts, BCM2711_MAILBOX_STATUS) &
                  BCM2711_MAILBOX_EMPTY);
    return deadline - now;
}
void bcm2711_firmware_vsync_receive(QTestState *qts)
{
    uint8_t request[28];

    g_assert_cmphex(qtest_readl(qts, BCM2711_MAILBOX_READ), ==,
                    PROPERTY_BUFFER_ADDRESS | PROPERTY_CHANNEL);
    qtest_memread(qts, PROPERTY_BUFFER_ADDRESS, request, sizeof(request));
    g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
    g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000004);
    g_assert_cmpuint((uint32_t)ldl_le_p(request + 20), ==, 0);
}
uint32_t bcm2711_firmware_display_power(
    QTestState *qts, uint32_t display_id, uint32_t state)
{
    uint32_t payload[2] = { display_id, state };

    bcm2711_property_call(
        qts, RPI_FWREQ_SET_DISPLAY_POWER,
        payload, G_N_ELEMENTS(payload));
    g_assert_cmpuint(payload[0], ==, display_id);
    return payload[1];
}
void assert_bcm2711_framebuffer_surface(
    QTestState *qts, const char *path, uint32_t width, uint32_t height,
    const uint8_t *expected)
{
    g_autofree char *header = g_strdup_printf(
        "P6\n%" PRIu32 " %" PRIu32 "\n255\n", width, height);
    g_autofree char *screendump = NULL;
    size_t row_stride = QEMU_ALIGN_UP(width * 3, 4);
    size_t screendump_size;

    qtest_qmp_assert_success(
        qts,
        "{ 'execute': 'screendump',"
        "  'arguments': { 'filename': %s } }",
        path);
    g_assert_true(g_file_get_contents(
        path, &screendump, &screendump_size, NULL));
    g_assert_cmpuint(screendump_size, ==,
                     strlen(header) + row_stride * height);
    g_assert_cmpmem(screendump, strlen(header), header, strlen(header));
    for (uint32_t row = 0; row < height; row++) {
        g_assert_cmpmem(
            screendump + strlen(header) + row * row_stride, width * 3,
            expected + row * width * 3, width * 3);
    }
}
void bcm2711_firmware_overscan_call(QTestState *qts, uint32_t tag,
                                            uint32_t values[4])
{
    bcm2711_property_call(
        qts, tag, values, 4);
}
void assert_bcm2711_framebuffer_pixel(
    QTestState *qts, const char *path, const uint8_t expected[3])
{
    static const char ppm_header[] = "P6\n1 1\n255\n";
    g_autofree char *screendump = NULL;
    size_t screendump_size;

    qtest_qmp_assert_success(
        qts,
        "{ 'execute': 'screendump',"
        "  'arguments': { 'filename': %s } }",
        path);
    g_assert_true(g_file_get_contents(
        path, &screendump, &screendump_size, NULL));
    g_assert_cmpuint(screendump_size, ==, strlen(ppm_header) + 4);
    g_assert_cmpmem(screendump, strlen(ppm_header),
                    ppm_header, strlen(ppm_header));
    g_assert_cmpmem(screendump + strlen(ppm_header), 3, expected, 3);
}
void configure_pwm_clock(QTestState *qts)
{
    /* 19.2 MHz XOSC divided by 192 gives a deterministic 100 kHz. */
    qtest_writel(qts, BCM2711_CPRMAN_BASE + CPRMAN_PWM_DIV,
                 CPRMAN_PASSWORD | (192 << 12));
    qtest_writel(qts, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | BIT(4) | 1);
    g_assert_cmpuint(qom_path_get_uint32(
                         qts, PWM_QOM_PATH, "clock-frequency"), ==, 100000);
}
void program_dma_paced_word(QTestState *qts, unsigned channel,
                                    uint32_t cb_addr, uint32_t source_addr,
                                    uint32_t dest_addr, unsigned permap,
                                    unsigned priority,
                                    unsigned panic_priority,
                                    uint32_t sample)
{
    uint32_t channel_base = BCM2711_DMA_BASE + channel * 0x100;

    qtest_writel(qts, source_addr, sample);
    qtest_writel(qts, cb_addr,
                 DMA_TI_D_DREQ | (permap << 16));
    qtest_writel(qts, cb_addr + 4, source_addr);
    qtest_writel(qts, cb_addr + 8, dest_addr);
    qtest_writel(qts, cb_addr + 12, sizeof(sample));
    qtest_writel(qts, cb_addr + 16, 0);
    qtest_writel(qts, cb_addr + 20, 0);
    qtest_writel(qts, channel_base + DMA_CB_ADDR, cb_addr);
    qtest_writel(qts, channel_base + DMA_CS,
                 DMA_CS_ACTIVE | DMA_CS_PRIORITY(priority) |
                 DMA_CS_PANIC_PRIORITY(panic_priority));
}
#ifndef _WIN32
uint32_t wait_for_dwc2_channel(QTestState *qts,
                                      unsigned int channel)
{
    uint32_t status = 0;

    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        status = qtest_readl(qts, BCM2711_DWC2_BASE + HCINT(channel));
        if (status & HCINTMSK_XFERCOMPL) {
            return status;
        }
        g_assert_cmphex(status &
                        (HCINTMSK_STALL | HCINTMSK_BBLERR |
                         HCINTMSK_XACTERR | HCINTMSK_AHBERR), ==, 0);
        qtest_clock_step(qts, 250000);
    }
    g_error("DWC2 host channel %u did not complete: HCINT=0x%08x",
            channel, status);
}
#endif
#ifndef _WIN32
void run_cm4_rpiboot_dwc2_enumeration(bool secure_provision,
                                              bool trust_bootcode)
{
    static const uint8_t get_device[] = {
        USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_DEVICE,
        0x00, 0x00, 18, 0x00,
    };
    static const uint8_t set_address[] = {
        0x00, USB_REQ_SET_ADDRESS, 23, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t get_configuration_out[] = {
        0x00, USB_REQ_GET_CONFIGURATION, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t set_address_in[] = {
        USB_DIR_IN, USB_REQ_SET_ADDRESS, 23, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t get_configuration[] = {
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_CONFIGURATION, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00,
    };
    static const uint8_t set_configuration[] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t set_configuration_zero[] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t set_configuration_invalid[] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t get_interface[] = {
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
        USB_REQ_GET_INTERFACE, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00,
    };
    static const uint8_t get_status[] = {
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_STATUS, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00,
    };
    static const uint8_t set_endpoint_halt[] = {
        USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
        USB_REQ_SET_FEATURE, USB_ENDPOINT_HALT, 0x00,
        0x01, 0x00, 0x00, 0x00,
    };
    static const uint8_t clear_endpoint_halt[] = {
        USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
        USB_REQ_CLEAR_FEATURE, USB_ENDPOINT_HALT, 0x00,
        0x01, 0x00, 0x00, 0x00,
    };
    static const uint8_t get_endpoint_status[] = {
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
        USB_REQ_GET_STATUS, 0x00, 0x00,
        0x01, 0x00, 0x02, 0x00,
    };
    static const uint8_t expected_device[] = {
        18, USB_DT_DEVICE, 0x00, 0x02, 0xff, 0x00, 0x00, 64,
        0x5c, 0x0a, 0x11, 0x27, 0x00, 0x01, 1, 2, 0, 1,
    };
    uint8_t vendor_setup[8] = { 0x40, 0 };
    uint8_t boot_message[24] = { 0 };
    uint8_t bootcode[1024];
    g_autofree uint8_t *config = NULL;
    g_autofree uint8_t *boot_img = NULL;
    g_autofree uint8_t *initial_eeprom = NULL;
    g_autofree uint8_t *initial_otp = NULL;
    size_t config_size;
    size_t boot_img_size;
    uint8_t file_message[260];
    uint8_t descriptor[64] = { 0 };
    g_autofree char *command = NULL;
    g_autofree char *expected_bootcode_hash = NULL;
    g_autofree char *actual_bootcode_hash = NULL;
    g_autofree char *expected_config_hash = NULL;
    g_autofree char *actual_config_hash = NULL;
    g_autofree char *expected_boot_img_hash = NULL;
    g_autofree char *actual_boot_img_hash = NULL;
    g_autofree char *state = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *drive_arguments = NULL;
    int sockets[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    if (secure_provision) {
        static const char provision_config[] = "program_pubkey=1\n";

        config_size = strlen(provision_config);
        config = g_memdup2(provision_config, config_size);
        boot_img = make_signed_eeprom(
            false, false, false, 0, TEST_EEPROM_DUPLICATE_NONE);
        boot_img_size = EEPROM_SIZE;
        initial_eeprom = make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        initial_otp = make_otp_image(0, 0, CM4_BOARD_REVISION, false);
        write_temp_image("cm4-rpiboot-eeprom-XXXXXX",
                         initial_eeprom, EEPROM_SIZE, &eeprom_path);
        write_temp_image("cm4-rpiboot-otp-XXXXXX",
                         initial_otp, OTP_SIZE, &otp_path);
        drive_arguments = g_strdup_printf(
            ",eeprom-drive=pieeprom,otp-drive=piotp"
            TEST_BOOTSYS_MACHINE_OPTION " "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=piotp,format=raw,file=%s,"
            "file.locking=off ",
            eeprom_path, otp_path);
    } else {
        config_size = 37;
        boot_img_size = 1024;
        config = g_malloc(config_size);
        boot_img = g_malloc(boot_img_size);
        drive_arguments = g_strdup(" ");
    }
    for (size_t i = 0; i < sizeof(bootcode); i++) {
        bootcode[i] = i * 29 + 7;
    }
    expected_bootcode_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, bootcode, sizeof(bootcode));
    command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,nrpiboot=on,"
        "rpiboot-bootcode-trusted-sha256=%s%s"
        "-chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev",
        trust_bootcode ? expected_bootcode_hash :
        "0000000000000000000000000000000000000000000000000000000000000000",
        drive_arguments, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "rpiboot-wait");
    g_assert_false(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_CURMODE_HOST);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_CONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        get_configuration_out,
                        sizeof(get_configuration_out), NULL, 0), ==,
                    sizeof(get_configuration_out));
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DOEPCTL(0)) &
                  DXEPCTL_STALL);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        set_address_in, sizeof(set_address_in), NULL, 0), ==,
                    sizeof(set_address_in));
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DIEPCTL(0)) &
                  DXEPCTL_STALL);
    g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCFG) &
                    DCFG_DEVADDR_MASK, ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);

    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], get_configuration,
                         descriptor, sizeof(descriptor)), ==, 1);
    g_assert_cmpuint(descriptor[0], ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-configuration"), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        set_configuration, sizeof(set_configuration),
                        NULL, 0), ==, sizeof(set_configuration));
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DOEPCTL(0)) &
                  DXEPCTL_STALL);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-configuration"), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        get_device, sizeof(get_device), NULL, 0), ==,
                    sizeof(get_device));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0,
                        sizeof(descriptor), NULL, 0,
                        descriptor, sizeof(descriptor)), ==,
                    sizeof(expected_device));
    g_assert_cmpmem(descriptor, sizeof(expected_device),
                    expected_device, sizeof(expected_device));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        set_address, sizeof(set_address), NULL, 0), ==,
                    sizeof(set_address));
    g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCFG) &
                    DCFG_DEVADDR_MASK, ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCFG) &
                    DCFG_DEVADDR_MASK, ==, DCFG_DEVADDR(23));

    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], set_configuration,
                         descriptor, sizeof(descriptor)), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-configuration"), ==, 1);
    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], get_configuration,
                         descriptor, sizeof(descriptor)), ==, 1);
    g_assert_cmpuint(descriptor[0], ==, 1);
    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], get_interface,
                         descriptor, sizeof(descriptor)), ==, 1);
    g_assert_cmpuint(descriptor[0], ==, 0);
    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], get_status,
                         descriptor, sizeof(descriptor)), ==, 2);
    g_assert_cmphex(lduw_le_p(descriptor), ==,
                    BIT(USB_DEVICE_SELF_POWERED));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        set_endpoint_halt, sizeof(set_endpoint_halt),
                        NULL, 0), ==, sizeof(set_endpoint_halt));
    g_assert_false(qtest_readl(
                       qts, BCM2711_DWC2_BASE + DOEPCTL(1)) &
                   DXEPCTL_STALL);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DOEPCTL(1)) &
                  DXEPCTL_STALL);
    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], get_endpoint_status,
                         descriptor, sizeof(descriptor)), ==, 2);
    g_assert_cmphex(lduw_le_p(descriptor), ==, 1);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        clear_endpoint_halt, sizeof(clear_endpoint_halt),
                        NULL, 0), ==, sizeof(clear_endpoint_halt));
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DOEPCTL(1)) &
                  DXEPCTL_STALL);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    g_assert_false(qtest_readl(
                       qts, BCM2711_DWC2_BASE + DOEPCTL(1)) &
                   DXEPCTL_STALL);
    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], get_endpoint_status,
                         descriptor, sizeof(descriptor)), ==, 2);
    g_assert_cmphex(lduw_le_p(descriptor), ==, 0);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        set_configuration_invalid,
                        sizeof(set_configuration_invalid), NULL, 0), ==,
                    sizeof(set_configuration_invalid));
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DOEPCTL(0)) &
                  DXEPCTL_STALL);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-configuration"), ==, 1);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-configuration"), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);

    stl_le_p(boot_message, sizeof(bootcode));
    stw_le_p(vendor_setup + 2, sizeof(boot_message));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        vendor_setup, sizeof(vendor_setup), NULL, 0), ==,
                    sizeof(vendor_setup));
    g_assert_true(qtest_readl(
                      qts, BCM2711_DWC2_BASE + DOEPCTL(0)) &
                  DXEPCTL_STALL);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        boot_message, sizeof(boot_message), NULL, 0), ==,
                    -EAGAIN);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-transfer-received"),
                     ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    dwc2_rpiboot_configure(sockets[0], 23);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        vendor_setup, sizeof(vendor_setup), NULL, 0), ==,
                    sizeof(vendor_setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        boot_message, sizeof(boot_message), NULL, 0), ==,
                    sizeof(boot_message));

    stw_le_p(vendor_setup + 2, sizeof(bootcode));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        vendor_setup, sizeof(vendor_setup), NULL, 0), ==,
                    sizeof(vendor_setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        bootcode, 512, NULL, 0), ==, 512);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-transfer-received"),
                     ==, 512);
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         sockets[0], set_configuration_zero,
                         descriptor, sizeof(descriptor)), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-configuration"), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-transfer-received"),
                     ==, 0);
    g_assert_cmphex(qtest_readl(
                        qts, BCM2711_DWC2_BASE + DOEPCTL(1)), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-transfer-received"),
                     ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    dwc2_rpiboot_configure(sockets[0], 23);

    stw_le_p(vendor_setup + 2, sizeof(boot_message));
    stw_le_p(vendor_setup + 4, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        vendor_setup, sizeof(vendor_setup), NULL, 0), ==,
                    sizeof(vendor_setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        boot_message, sizeof(boot_message), NULL, 0), ==,
                    sizeof(boot_message));
    stw_le_p(vendor_setup + 2, sizeof(bootcode));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        vendor_setup, sizeof(vendor_setup), NULL, 0), ==,
                    sizeof(vendor_setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 0);
    for (size_t offset = 0; offset < sizeof(bootcode); offset += 512) {
        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                            bootcode + offset, 512, NULL, 0), ==, 512);
    }
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-bootcode-size"), ==,
                     sizeof(bootcode));
    actual_bootcode_hash = qom_get_string(qts, "rpiboot-bootcode-sha256");
    g_assert_cmpstr(actual_bootcode_hash, ==, expected_bootcode_hash);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "rpiboot-bootcode-trust");
    g_assert_cmpstr(state, ==, trust_bootcode ? "trusted" : "mismatch");

    memset(vendor_setup, 0, sizeof(vendor_setup));
    vendor_setup[0] = USB_DIR_IN | USB_TYPE_VENDOR;
    stw_le_p(vendor_setup + 6, 4);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        vendor_setup, sizeof(vendor_setup), NULL, 0), ==,
                    sizeof(vendor_setup));
    memset(descriptor, 0xff, sizeof(descriptor));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 0, 4,
                        NULL, 0, descriptor, sizeof(descriptor)), ==, 4);
    g_assert_cmphex(ldl_le_p(descriptor), ==, trust_bootcode ? 0 : 1);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, trust_bootcode ?
                    "rpiboot-bootcode-ready" :
                    "rpiboot-bootcode-untrusted");
    if (!trust_bootcode) {
        qtest_quit(qts);
        close(sockets[0]);
        return;
    }

    if (!secure_provision) {
        for (size_t i = 0; i < config_size; i++) {
            config[i] = i * 5 + 3;
        }
        for (size_t i = 0; i < boot_img_size; i++) {
            boot_img[i] = i * 11 + 9;
        }
    }

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_DISCONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_CONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    dwc2_rpiboot_configure(sockets[0], 24);

    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 0);
    g_assert_cmpstr((char *)file_message + 4, ==, "config.txt");
    dwc2_rpiboot_announce(sockets[0], config_size);
    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 1);
    g_assert_cmpstr((char *)file_message + 4, ==, "config.txt");
    dwc2_rpiboot_announce(sockets[0], config_size);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        config, config_size, NULL, 0), ==, config_size);

    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 0);
    g_assert_cmpstr((char *)file_message + 4, ==,
                    secure_provision ? "pieeprom.bin" : "boot.img");
    dwc2_rpiboot_announce(sockets[0], boot_img_size);
    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 1);
    g_assert_cmpstr((char *)file_message + 4, ==,
                    secure_provision ? "pieeprom.bin" : "boot.img");
    dwc2_rpiboot_announce(sockets[0], boot_img_size);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        boot_img, 512, NULL, 0), ==, 512);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-transfer-received"),
                     ==, 512);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-transfer-received"),
                     ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    dwc2_rpiboot_configure(sockets[0], 24);

    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 0);
    g_assert_cmpstr((char *)file_message + 4, ==, "config.txt");
    dwc2_rpiboot_announce(sockets[0], config_size);
    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 1);
    g_assert_cmpstr((char *)file_message + 4, ==, "config.txt");
    dwc2_rpiboot_announce(sockets[0], config_size);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        config, config_size, NULL, 0), ==, config_size);
    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 0);
    g_assert_cmpstr((char *)file_message + 4, ==,
                    secure_provision ? "pieeprom.bin" : "boot.img");
    dwc2_rpiboot_announce(sockets[0], boot_img_size);
    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 1);
    g_assert_cmpstr((char *)file_message + 4, ==,
                    secure_provision ? "pieeprom.bin" : "boot.img");
    dwc2_rpiboot_announce(sockets[0], boot_img_size);
    for (size_t offset = 0; offset < boot_img_size; offset += 512) {
        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                            boot_img + offset, 512, NULL, 0), ==, 512);
    }

    dwc2_rpiboot_get_file_message(sockets[0], file_message);
    g_assert_cmphex(ldl_le_p(file_message), ==, 2);
    g_assert_cmpstr((char *)file_message + 4, ==, "done");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==,
                    secure_provision ? "rpiboot-provisioned" :
                                       "rpiboot-boot-image-invalid");
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-config-size"), ==,
                     config_size);
    g_assert_cmpuint(qom_get_uint32(qts, "rpiboot-boot-img-size"), ==,
                     boot_img_size);
    expected_config_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, config, config_size);
    actual_config_hash = qom_get_string(qts, "rpiboot-config-sha256");
    g_assert_cmpstr(actual_config_hash, ==, expected_config_hash);
    expected_boot_img_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, boot_img, boot_img_size);
    actual_boot_img_hash = qom_get_string(qts, "rpiboot-boot-img-sha256");
    g_assert_cmpstr(actual_boot_img_hash, ==, expected_boot_img_hash);

    qtest_quit(qts);
    close(sockets[0]);
    if (secure_provision) {
        g_autofree char *otp_contents = NULL;
        size_t otp_size;

        assert_file_equals(eeprom_path, boot_img, EEPROM_SIZE);
        g_assert_true(g_file_get_contents(
            otp_path, &otp_contents, &otp_size, NULL));
        g_assert_cmpuint(otp_size, ==, OTP_SIZE);
        g_assert_cmphex(ldl_le_p(
                            otp_contents + (17 - 1) * sizeof(uint32_t)),
                        ==, BIT(15));
        g_assert_cmphex(ldl_le_p(
                            otp_contents + (18 - 1) * sizeof(uint32_t)),
                        ==, BIT(15));
        g_assert_cmphex(ldl_le_p(
                            otp_contents + (55 - 1) * sizeof(uint32_t)),
                        ==, 0x81);
        unlink(eeprom_path);
        unlink(otp_path);
    }
}
#endif
void assert_firmware_memory_nodes(QTestState *qts,
                                         uint64_t lower_size,
                                         uint64_t upper_size)
{
    uint64_t middle_size = MIN(
        upper_size, RASPI4_LOW_RAM_END - 1 * GiB);
    uint64_t high_size = upper_size - middle_size;
    uint64_t dt_address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t dt_size = qom_get_uint64(
        qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *dt = g_malloc(dt_size);
    const uint8_t *reg;
    int reg_length;
    int node;

    qtest_memread(qts, dt_address, dt, dt_size);
    g_assert_cmpint(fdt_check_header(dt), ==, 0);
    node = fdt_path_offset(dt, "/memory@0");
    g_assert_cmpint(node, >=, 0);
    reg = fdt_getprop(dt, node, "reg", &reg_length);
    g_assert_nonnull(reg);
    g_assert_cmpint(reg_length, ==, 4 * sizeof(uint32_t));
    g_assert_cmphex(ldq_be_p(reg), ==, 0);
    g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==, lower_size);

    node = fdt_path_offset(dt, "/memory@40000000");
    if (!middle_size) {
        g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
    } else {
        g_assert_cmpint(node, >=, 0);
        reg = fdt_getprop(dt, node, "reg", &reg_length);
        g_assert_nonnull(reg);
        g_assert_cmpint(reg_length, ==, 4 * sizeof(uint32_t));
        g_assert_cmphex(ldq_be_p(reg), ==, 1 * GiB);
        g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==, middle_size);
    }

    node = fdt_path_offset(dt, "/memory@100000000");
    if (!high_size) {
        g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
    } else {
        g_assert_cmpint(node, >=, 0);
        reg = fdt_getprop(dt, node, "reg", &reg_length);
        g_assert_nonnull(reg);
        g_assert_cmpint(reg_length, ==, 4 * sizeof(uint32_t));
        g_assert_cmphex(ldq_be_p(reg), ==, RASPI4_HIGH_RAM_BASE);
        g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==, high_size);
    }
}
void assert_no_firmware_initramfs(QTestState *qts)
{
    g_autofree uint8_t *dt = test_read_handoff_dtb(qts);
    int chosen = fdt_path_offset(dt, "/chosen");

    g_assert_cmpint(chosen, >=, 0);
    g_assert_null(fdt_getprop(dt, chosen, "linux,initrd-start", NULL));
    g_assert_null(fdt_getprop(dt, chosen, "linux,initrd-end", NULL));
}
