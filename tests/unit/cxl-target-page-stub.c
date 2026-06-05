/*
 * Unit-test target-page definition for migration helpers linked outside a
 * complete system binary.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "exec/page-vary.h"

const TargetPageBits target_page = {
    .decided = true,
    .bits = 12,
    .mask = -1ULL << 12,
};
