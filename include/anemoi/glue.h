/*
 * Narrow QEMU migration glue entry points.
 */

#ifndef ANEMOI_GLUE_H
#define ANEMOI_GLUE_H

#include "qapi/error.h"

int anemoi_migration_switchover_hook(Error **errp);

#endif /* ANEMOI_GLUE_H */
