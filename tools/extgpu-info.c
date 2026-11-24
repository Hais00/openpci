/* SPDX-License-Identifier: MIT */
#include <extgpu/extgpu.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_string(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}
static int failed(const char *operation, extgpu_status status)
{
    fprintf(stderr, "%s: %s (%" PRId32 ")\n", operation, extgpu_status_string(status), status);
    return 1;
}
int main(int argc, char **argv)
{
    uint64_t inspect_id = 0;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: extgpu-info [--inspect-id EXACT_IOREGISTRY_ID]\n"
             "Default: IORegistry enumeration only; no open or claim.\n"
             "--inspect-id: open exact service and query protocol/identity; no claim.\n"
             "This CLI has no mutation, BAR mapping, DMA, IRQ, reset or install mode.");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--inspect-id") == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(argv[2], &end, 0);
        if (errno || !end || !*argv[2] || *end || argv[2][0] == '-' || !parsed) return failed("registry ID", EXTGPU_E_ARGUMENT);
        inspect_id = (uint64_t)parsed;
    } else if (argc != 1) return failed("arguments; use --help", EXTGPU_E_ARGUMENT);
    if (extgpu_sdk_abi_version() != EXTGPU_SDK_ABI_VERSION) return failed("SDK ABI", EXTGPU_E_PROTOCOL);
    extgpu_session *session = NULL;
    extgpu_status status = extgpu_session_create(&session);
    if (status != EXTGPU_OK) return failed("session", status);
    int result = 0;
    if (inspect_id) {
        extgpu_device *device = NULL;
        status = extgpu_device_open(session, inspect_id, &device);
        if (status != EXTGPU_OK) result = failed("open exact registry service", status);
        else {
            extgpu_protocol_info protocol;
            extgpu_pci_identity identity;
            status = extgpu_device_protocol(device, &protocol);
            if (status == EXTGPU_OK) status = extgpu_device_identity(device, &identity);
            if (status != EXTGPU_OK) result = failed("inspect", status);
            else printf("{\"registryID\":\"0x%" PRIx64 "\",\"protocolMajor\":%u,\"protocolMinor\":%u,"
                        "\"generation\":%" PRIu64 ",\"capabilities\":\"0x%" PRIx64 "\","
                        "\"bus\":%u,\"device\":%u,\"function\":%u,\"vendorID\":%u,\"deviceID\":%u,"
                        "\"claimed\":false,\"hardwareInitialized\":false}\n",
                        inspect_id, protocol.major, protocol.minor, protocol.generation, protocol.capabilities,
                        identity.bus, identity.device, identity.function, identity.vendor_id, identity.device_id);
            status = extgpu_device_close(&device);
            if (status != EXTGPU_OK) result = failed("close", status);
        }
    } else {
        size_t count = 0;
        status = extgpu_enumerate(session, NULL, 0, &count);
        if (status != EXTGPU_OK || count > 16384) result = failed("enumerate count", status != EXTGPU_OK ? status : EXTGPU_E_CAPACITY);
        else {
            size_t capacity = count;
            extgpu_device_info *items = count ? calloc(count, sizeof(*items)) : NULL;
            if (count && !items) result = failed("enumeration allocation", EXTGPU_E_MEMORY);
            else {
                /* A hotplug race returns capacity error; no silently truncated list. */
                status = capacity ? extgpu_enumerate(session, items, capacity, &count) : EXTGPU_OK;
                if (!capacity) count = 0;
                if (status != EXTGPU_OK) result = failed("enumerate", status);
                else {
                    puts("{\"scope\":\"IORegistry enumeration only\",\"devices\":[");
                    for (size_t i = 0; i < count; ++i) {
                        printf("%s{\"registryID\":\"0x%" PRIx64 "\",\"pciRegistryID\":\"0x%" PRIx64 "\","
                               "\"vendorID\":%u,\"deviceID\":%u,\"name\":", i ? ",\n" : "",
                               items[i].registry_id, items[i].pci_registry_id, items[i].vendor_id, items[i].device_id);
                        json_string(items[i].registry_name); printf(",\"hostRoot\":"); json_string(items[i].host_root); putchar('}');
                    }
                    puts("\n],\"claimed\":false,\"hardwareInitialized\":false}");
                }
            }
            free(items);
        }
    }
    status = extgpu_session_destroy(&session);
    if (status != EXTGPU_OK) result = failed("session destroy", status);
    return result;
}
