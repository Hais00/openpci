/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Public SDK only. Enumeration is the default; ownership requires two flags.
 */
#include <extgpu/extgpu.h>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <unistd.h>

namespace {
constexpr std::size_t maximum_devices = 4096;
constexpr std::size_t maximum_operations = 64;
volatile std::sig_atomic_t deferred_signal = 0;

void defer_exit_signal(int number) { deferred_signal = number; }

void json_string(const char *text, std::size_t limit = 4096)
{
    std::putchar('"');
    if (text) {
        for (std::size_t i = 0; i < limit && text[i]; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '"' || c == '\\') std::printf("\\%c", c);
            else if (c < 0x20 || c >= 0x7f) std::printf("\\u%04x", c);
            else std::putchar(c);
        }
    }
    std::putchar('"');
}

bool parse_id(const char *text, std::uint64_t &value)
{
    if (!text || !*text) return false;
    unsigned base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
        if (!*text) return false;
    }
    value = 0;
    for (; *text; ++text) {
        const unsigned char c = static_cast<unsigned char>(*text);
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (digit >= base || value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            return false;
        value = value * base + digit;
    }
    return value != 0;
}

struct Operation {
    const char *name = nullptr;
    extgpu_status status = EXTGPU_OK;
    bool accepted = false;
    bool has_device_error = false;
    extgpu_error_info error{};
};

struct Report {
    const char *scope = "enumeration_only";
    std::uint64_t requested_id = 0;
    bool failed = false, opened = false, claimed = false;
    bool claim_attempted = false, release_attempted = false;
    bool release_acknowledged = false, ownership_absent = false;
    bool close_acknowledged = false, session_destroyed = false;
    bool recovery_hold = false;
    bool protocol_valid = false, identity_valid = false;
    extgpu_protocol_info protocol{};
    extgpu_pci_identity identity{};
    std::uint32_t header[16]{};
    unsigned header_count = 0;
    struct Bar {
        unsigned number = 0;
        extgpu_status status = EXTGPU_OK;
        extgpu_bar_info info{};
    } bars[6];
    unsigned bar_count = 0;
    Operation operations[maximum_operations];
    std::size_t operation_count = 0;

    void record(const char *name, extgpu_status status, extgpu_device *device = nullptr,
                bool accepted = false)
    {
        if (operation_count >= maximum_operations) { failed = true; return; }
        Operation &op = operations[operation_count++];
        op.name = name; op.status = status;
        op.accepted = status == EXTGPU_OK || accepted;
        if (!op.accepted) failed = true;
        // Capture this failure before cleanup calls can replace SDK last_error.
        if (status != EXTGPU_OK && device)
            op.has_device_error = extgpu_device_last_error(device, &op.error) == EXTGPU_OK;
    }
};

int usage(const char *error, int code)
{
    std::printf("{\"scope\":\"arguments_only\",\"result\":");
    json_string(code == 0 ? "help" : "invalid_arguments");
    std::printf(",\"error\":"); json_string(error);
    std::printf(",\"usage\":\"inspect [--inspect-id UINT64 [--claim-pristine]]\","
                "\"device_open_attempted\":false,\"claim_attempted\":false}\n");
    return code;
}
} // namespace

int main(int argc, char **argv)
{
    Report report;
    bool inspect = false, claim = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--inspect-id") == 0) {
            if (inspect || i + 1 == argc || !parse_id(argv[++i], report.requested_id))
                return usage("--inspect-id requires one nonzero unsigned decimal or 0x hexadecimal uint64", 2);
            inspect = true;
        } else if (std::strcmp(argv[i], "--claim-pristine") == 0) {
            if (claim) return usage("duplicate --claim-pristine", 2);
            claim = true;
        } else if (std::strcmp(argv[i], "--help") == 0 && argc == 2) {
            return usage("", 0);
        } else return usage("unknown or duplicate argument", 2);
    }
    if (claim && !inspect) return usage("--claim-pristine requires --inspect-id", 2);
    // Once a user client exists, SIGINT/TERM must not silently drop uncertain ownership.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
        std::signal(SIGINT, defer_exit_signal) == SIG_ERR ||
        std::signal(SIGTERM, defer_exit_signal) == SIG_ERR)
        return usage("cannot install ownership-preserving signal handlers", 1);
    report.scope = claim ? "explicit_pristine_claim_header_and_bar_metadata" :
                   inspect ? "exact_service_protocol_and_identity" : "enumeration_only";

    extgpu_session *session = nullptr;
    extgpu_device *device = nullptr;
    std::unique_ptr<extgpu_device_info[]> devices;
    std::size_t device_count = 0;
    const std::uint32_t abi = extgpu_sdk_abi_version();
    report.record("sdk_abi_version", abi == EXTGPU_SDK_ABI_VERSION ? EXTGPU_OK : EXTGPU_E_PROTOCOL);
    if (!report.failed) report.record("session_create", extgpu_session_create(&session));

    if (!report.failed && !inspect) {
        std::size_t count = 0;
        report.record("enumerate_count", extgpu_enumerate(session, nullptr, 0, &count));
        if (!report.failed && count > maximum_devices)
            report.record("enumerate_count_limit", EXTGPU_E_CAPACITY);
        if (!report.failed && count) {
            // Bounded retries accommodate registry changes without selecting a device.
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                if (count > maximum_devices) {
                    report.record("enumerate_count_limit", EXTGPU_E_CAPACITY); break;
                }
                const std::size_t capacity = count;
                devices.reset(new (std::nothrow) extgpu_device_info[capacity]{});
                if (!devices) { report.record("enumerate_allocate", EXTGPU_E_MEMORY); break; }
                const extgpu_status st = extgpu_enumerate(session, devices.get(), capacity, &count);
                const bool retry = st == EXTGPU_E_CAPACITY && count > capacity && attempt < 2;
                report.record("enumerate_items", st, nullptr, retry);
                if (retry) continue;
                if (st == EXTGPU_OK && count <= capacity) device_count = count;
                else if (st == EXTGPU_OK) report.record("enumerate_count_bounds", EXTGPU_E_PROTOCOL);
                break;
            }
        }
    }
    if (!report.failed && inspect) {
        const extgpu_status open_status = extgpu_device_open(session, report.requested_id, &device);
        report.record("device_open_exact_id", open_status, device);
        report.opened = device != nullptr;
        if (!report.failed) {
            const extgpu_status st = extgpu_device_protocol(device, &report.protocol);
            report.record("device_protocol", st, device); report.protocol_valid = st == EXTGPU_OK;
        }
        if (!report.failed) {
            const extgpu_status st = extgpu_device_identity(device, &report.identity);
            report.record("device_identity", st, device); report.identity_valid = st == EXTGPU_OK;
        }
        if (!report.failed && claim) {
            report.claim_attempted = true;
            const extgpu_status st = extgpu_device_claim(device);
            report.record("device_claim", st, device); report.claimed = st == EXTGPU_OK;
            if (st != EXTGPU_OK) report.recovery_hold = true;
        }
        if (!report.failed && report.claimed) {
            for (unsigned i = 0; i < 16 && !report.failed; ++i) {
                const extgpu_status st = extgpu_config_read(device, i * 4, 4, &report.header[i]);
                report.record("config_read_header_dword", st, device);
                if (st == EXTGPU_OK) ++report.header_count;
            }
            // Only a standard type-0 header defines six device BAR registers.
            if (!report.failed && ((report.header[3] >> 16) & 0x7f) != 0)
                report.record("type_zero_pci_header_required", EXTGPU_E_UNSUPPORTED);
            for (unsigned bar = 0; bar < 6 && !report.failed; ++bar) {
                Report::Bar &entry = report.bars[report.bar_count++];
                entry.number = bar;
                entry.status = extgpu_bar_get_info(device, bar, &entry.info);
                report.record("bar_get_info", entry.status, device,
                              entry.status == EXTGPU_E_NOT_FOUND || entry.status == EXTGPU_E_UNSUPPORTED);
                // The next register is an address high half, not another BAR.
                if (!(report.header[4 + bar] & 1) && (report.header[4 + bar] & 6) == 4) {
                    if (bar == 5) report.record("invalid_64_bit_bar_slot", EXTGPU_E_PROTOCOL);
                    else ++bar;
                }
            }
        }
    }

    // Any failed claim/release/close needs explicit recovery; never drop its client.
    bool may_close = !report.claim_attempted;
    if (device && report.claim_attempted && !report.recovery_hold) {
        report.release_attempted = true;
        const extgpu_status st = extgpu_device_release(device);
        report.record("device_release", st, device);
        report.release_acknowledged = st == EXTGPU_OK;
        report.ownership_absent = st == EXTGPU_E_NOT_CLAIMED;
        may_close = report.release_acknowledged;
        if (!may_close) report.recovery_hold = true;
    }
    if (device && may_close && !report.recovery_hold) {
        const extgpu_status st = extgpu_device_close(&device);
        report.record("device_close", st, device);
        report.close_acknowledged = st == EXTGPU_OK && device == nullptr;
        if (st == EXTGPU_OK && device) report.record("device_close_retained_handle", EXTGPU_E_PROTOCOL);
        if (!report.close_acknowledged) report.recovery_hold = true;
    }
    // Never force-close after a failed release or destroy a parent with a live child.
    if (session && !device && !report.recovery_hold) {
        const extgpu_status st = extgpu_session_destroy(&session);
        report.record("session_destroy", st);
        report.session_destroyed = st == EXTGPU_OK && session == nullptr;
        if (st == EXTGPU_OK && session) report.record("session_destroy_retained_handle", EXTGPU_E_PROTOCOL);
        if (!report.session_destroyed) report.recovery_hold = true;
    }

    std::printf("{\"schema\":\"openpci.public-sdk-inspector.v1\",\"scope\":"); json_string(report.scope);
    std::printf(",\"result\":"); json_string(report.recovery_hold ? "recovery_needed_process_held" : report.failed ? "failed" : "success");
    std::printf(",\"runtime_abi\":%u,\"requested_registry_id\":\"0x%016" PRIx64 "\",\"devices\":[", abi, report.requested_id);
    for (std::size_t i = 0; i < device_count; ++i) {
        if (i) std::putchar(',');
        const extgpu_device_info &d = devices[i];
        std::printf("{\"registry_id\":\"0x%016" PRIx64 "\",\"pci_registry_id\":\"0x%016" PRIx64
                    "\",\"vendor_id\":%u,\"device_id\":%u,\"name\":", d.registry_id, d.pci_registry_id, d.vendor_id, d.device_id);
        json_string(d.registry_name, sizeof(d.registry_name)); std::printf(",\"host_root\":");
        json_string(d.host_root, sizeof(d.host_root)); std::putchar('}');
    }
    std::printf("],\"protocol\":");
    if (report.protocol_valid) std::printf("{\"major\":%u,\"minor\":%u,\"generation\":\"0x%016" PRIx64
        "\",\"capabilities\":\"0x%016" PRIx64 "\"}", report.protocol.major, report.protocol.minor, report.protocol.generation, report.protocol.capabilities);
    else std::printf("null");
    std::printf(",\"identity\":");
    if (report.identity_valid) std::printf("{\"bus\":%u,\"device\":%u,\"function\":%u,\"vendor_id\":%u,\"device_id\":%u,\"class_code\":%u}",
        report.identity.bus, report.identity.device, report.identity.function, report.identity.vendor_id, report.identity.device_id, report.identity.class_code);
    else std::printf("null");
    std::printf(",\"header_dwords\":[");
    for (unsigned i = 0; i < report.header_count; ++i) {
        if (i) std::putchar(','); std::printf("{\"offset\":%u,\"value\":%u}", i * 4, report.header[i]);
    }
    std::printf("],\"bars\":[");
    for (unsigned i = 0; i < report.bar_count; ++i) {
        if (i) std::putchar(','); const Report::Bar &b = report.bars[i];
        std::printf("{\"bar\":%u,\"status\":%" PRId32 ",\"metadata\":", b.number, b.status);
        if (b.status == EXTGPU_OK) std::printf("{\"memory_index\":%u,\"type\":%u,\"size\":%" PRIu64 "}", b.info.memory_index, b.info.type, b.info.size);
        else std::printf("null"); std::putchar('}');
    }
    std::printf("],\"operations\":[");
    for (std::size_t i = 0; i < report.operation_count; ++i) {
        if (i) std::putchar(','); const Operation &op = report.operations[i];
        std::printf("{\"operation\":"); json_string(op.name);
        std::printf(",\"status\":%" PRId32 ",\"status_name\":", op.status); json_string(extgpu_status_string(op.status));
        std::printf(",\"accepted\":%s,\"device_error\":", op.accepted ? "true" : "false");
        if (op.has_device_error) {
            std::printf("{\"status\":%" PRId32 ",\"native_code\":%" PRId32 ",\"operation\":", op.error.status, op.error.native_code);
            json_string(op.error.operation, sizeof(op.error.operation)); std::putchar('}');
        } else std::printf("null"); std::putchar('}');
    }
    std::printf("],\"actions\":{\"device_opened\":%s,\"claim_attempted\":%s,\"claim_succeeded\":%s,"
                "\"release_attempted\":%s,\"release_acknowledged\":%s,\"ownership_absent_reply\":%s,"
                "\"close_acknowledged\":%s,\"session_destroyed\":%s,\"device_handle_live\":%s,"
                "\"session_handle_live\":%s,\"bar_maps\":0,\"config_writes\":0,"
                "\"mmio_operations\":0,\"dma_operations\":0,\"irq_operations\":0,\"resets\":0},"
                "\"gpu_initialization_proven\":false,\"process_exit_proves_safe_release\":false,"
                "\"recovery_hold\":%s,\"pid\":%ld,\"last_deferred_signal\":%d}\n",
                report.opened ? "true" : "false", report.claim_attempted ? "true" : "false", report.claimed ? "true" : "false",
                report.release_attempted ? "true" : "false", report.release_acknowledged ? "true" : "false", report.ownership_absent ? "true" : "false",
                report.close_acknowledged ? "true" : "false", report.session_destroyed ? "true" : "false", device ? "true" : "false", session ? "true" : "false",
                report.recovery_hold ? "true" : "false", static_cast<long>(getpid()), static_cast<int>(deferred_signal));
    std::fflush(stdout);
    if (report.recovery_hold) {
        std::fprintf(stderr, "OpenPCI inspector recovery needed: PID %ld retains its handles; SIGINT/SIGTERM defer exit. Recover the device before explicit external termination.\n", static_cast<long>(getpid()));
        std::fflush(stderr);
        // No automatic release retries, destructors, forced close or parent destruction.
        for (;;) pause();
    }
    return report.failed ? 1 : 0;
}
