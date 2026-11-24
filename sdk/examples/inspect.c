/* SPDX-License-Identifier: MIT
 * Read-only example: exact service protocol/identity, no automatic claim. */
#include <extgpu/extgpu.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: inspect EXACT_IOREGISTRY_ID\n"); return 2; }
    errno = 0; char *end = NULL; uint64_t id = strtoull(argv[1], &end, 0);
    if (errno || !end || *end || !id || argv[1][0] == '-') return 2;
    extgpu_session *session = NULL; extgpu_device *device = NULL;
    extgpu_status st = extgpu_session_create(&session);
    if (st == EXTGPU_OK) st = extgpu_device_open(session, id, &device);
    if (st == EXTGPU_OK) {
        extgpu_protocol_info protocol;
        st = extgpu_device_protocol(device, &protocol);
        if (st == EXTGPU_OK) printf("protocol=%u.%u generation=%" PRIu64 " capabilities=0x%" PRIx64 " claimed=false\n",
                                    protocol.major, protocol.minor, protocol.generation, protocol.capabilities);
    }
    if (st != EXTGPU_OK) fprintf(stderr, "%s\n", extgpu_status_string(st));
    if (device && extgpu_device_close(&device) != EXTGPU_OK) return 1;
    if (session && extgpu_session_destroy(&session) != EXTGPU_OK) return 1;
    return st == EXTGPU_OK ? 0 : 1;
}
