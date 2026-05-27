/*
 * Anemoi userfaultfd service.
 */

#ifndef ANEMOI_UFFD_H
#define ANEMOI_UFFD_H

#include "anemoi/cache.h"
#include "qapi/error.h"

typedef struct AnemoiFaultService AnemoiFaultService;

typedef struct AnemoiFaultRange {
    void *host;
    uint64_t length;
    const char *idstr;
} AnemoiFaultRange;

typedef struct AnemoiFaultServiceConfig {
    AnemoiCache *cache;
    const AnemoiFaultRange *ranges;
    uint32_t nr_ranges;
    bool discard_on_start;
} AnemoiFaultServiceConfig;

AnemoiFaultService *anemoi_fault_service_start(
    const AnemoiFaultServiceConfig *cfg, Error **errp);
int anemoi_fault_service_quiesce(AnemoiFaultService *service, Error **errp);
void anemoi_fault_service_stop(AnemoiFaultService *service);

int anemoi_fault_service_fd(const AnemoiFaultService *service);
bool anemoi_fault_service_quiesced(const AnemoiFaultService *service);
bool anemoi_fault_service_failed(const AnemoiFaultService *service);

#endif /* ANEMOI_UFFD_H */
