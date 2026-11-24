/* Tests the real native SDK without opening, claiming or programming a GPU. */
#include <extgpu/extgpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)

int main(void)
{
    extgpu_session *session = NULL;
    size_t count = 0;
    CHECK(extgpu_session_create(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_session_create(&session) == EXTGPU_OK && session != NULL);
    CHECK(extgpu_enumerate(NULL, NULL, 0, &count) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_enumerate(session, NULL, 0, NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_enumerate(session, NULL, 0, &count) == EXTGPU_OK);
    CHECK(count <= 4096);
    extgpu_device_info *items = calloc(count ? count : 1, sizeof(*items));
    CHECK(items != NULL);
    size_t returned = 0;
    extgpu_status status = extgpu_enumerate(session, items, count, &returned);
    CHECK(status == EXTGPU_OK || status == EXTGPU_E_CAPACITY);
    CHECK(status != EXTGPU_OK || returned <= count);
    for (size_t i = 0; status == EXTGPU_OK && i < returned; ++i) {
        CHECK(items[i].registry_id != 0);
        for (size_t j = 0; j < i; ++j) CHECK(items[i].registry_id != items[j].registry_id);
    }
    free(items);
    CHECK(extgpu_device_claim(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_device_release(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_device_close(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_mapping_unmap(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_buffer_unregister(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_irq_close(NULL) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_session_destroy(&session) == EXTGPU_OK && session == NULL);
    CHECK(extgpu_session_destroy(&session) == EXTGPU_E_ARGUMENT);
    puts("PASS real IOKit enumeration and native API validation; no device opened or claimed");
    return 0;
}
