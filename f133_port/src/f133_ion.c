#include "f133_ion.h"

#include "ion_mem_alloc.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool f133_ion_is_ready(const f133_ion_t *ion)
{
    return ion != NULL && ion->opened && ion->memops != NULL;
}

void f133_ion_buffer_clear(f133_ion_buffer_t *buffer)
{
    if(buffer == NULL) return;
    memset(buffer, 0, sizeof(*buffer));
}

bool f133_ion_buffer_is_valid(const f133_ion_buffer_t *buffer)
{
    return buffer != NULL && buffer->virt != NULL && buffer->phys != 0U && buffer->size != 0U;
}

bool f133_ion_init(f133_ion_t *ion)
{
    if(ion == NULL) return false;
    if(f133_ion_is_ready(ion)) return true;

    ion->memops = GetMemAdapterOpsS();
    if(ion->memops == NULL) {
        fprintf(stderr, "f133_ion: GetMemAdapterOpsS failed\n");
        ion->opened = false;
        return false;
    }

    if(SunxiMemOpen(ion->memops) < 0) {
        perror("f133_ion: SunxiMemOpen failed");
        ion->memops = NULL;
        ion->opened = false;
        return false;
    }

    ion->opened = true;
    return true;
}

void f133_ion_deinit(f133_ion_t *ion)
{
    if(ion == NULL) return;

    if(ion->opened && ion->memops != NULL) {
        SunxiMemClose(ion->memops);
    }

    ion->memops = NULL;
    ion->opened = false;
}

uintptr_t f133_ion_get_phys(f133_ion_t *ion, void *virt)
{
    if(!f133_ion_is_ready(ion) || virt == NULL) return 0U;
    return (uintptr_t)SunxiMemGetPhysicAddressCpu(ion->memops, virt);
}

bool f133_ion_alloc(f133_ion_t *ion, size_t size, f133_ion_buffer_t *out)
{
    if(out != NULL) f133_ion_buffer_clear(out);
    if(!f133_ion_is_ready(ion) || out == NULL || size == 0U || size > (size_t)INT_MAX) {
        return false;
    }

    void *virt = SunxiMemPalloc(ion->memops, (int)size);
    if(virt == NULL) {
        fprintf(stderr, "f133_ion: SunxiMemPalloc failed, size=%lu\n", (unsigned long)size);
        return false;
    }

    uintptr_t phys = f133_ion_get_phys(ion, virt);
    if(phys == 0U) {
        fprintf(stderr, "f133_ion: SunxiMemGetPhysicAddressCpu failed, virt=%p\n", virt);
        SunxiMemPfree(ion->memops, virt);
        return false;
    }

    out->virt = virt;
    out->phys = phys;
    out->size = size;
    return true;
}

void f133_ion_free(f133_ion_t *ion, f133_ion_buffer_t *buffer)
{
    if(buffer == NULL) return;

    if(buffer->virt == NULL) {
        f133_ion_buffer_clear(buffer);
        return;
    }

    if(!f133_ion_is_ready(ion)) {
        fprintf(stderr, "f133_ion: skip free because ion context is not ready, virt=%p\n", buffer->virt);
        return;
    }

    SunxiMemPfree(ion->memops, buffer->virt);
    f133_ion_buffer_clear(buffer);
}

void f133_ion_flush_range(f133_ion_t *ion, void *virt, size_t size)
{
    if(!f133_ion_is_ready(ion) || virt == NULL || size == 0U) return;

    unsigned char *cursor = (unsigned char *)virt;
    size_t remaining = size;
    while(remaining > 0U) {
        int chunk = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
        SunxiMemFlushCache(ion->memops, cursor, chunk);
        cursor += chunk;
        remaining -= (size_t)chunk;
    }
}

void f133_ion_flush_cache(f133_ion_t *ion, const f133_ion_buffer_t *buffer)
{
    if(!f133_ion_buffer_is_valid(buffer)) return;
    f133_ion_flush_range(ion, buffer->virt, buffer->size);
}
