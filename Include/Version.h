#pragma once

// Keep public CI and local test-build numbering on the same fork revision scale.
#define VER_REVISION_BASE 2844
#define VER_REVISION_BASE_COMMIT 250608d03e3f0030423ee03924498a3999726231

#include "../revision.h"

#ifndef REV_DATE
#define REV_DATE       0
#endif

#ifndef REV_HASH
#define REV_HASH       0
#endif

#ifndef REV_NUM
#define REV_NUM        0
#endif

#ifndef REV_BRANCH
#define REV_BRANCH     LOCAL
#endif

#define DO_MAKE_STR(x) #x
#define MAKE_STR(x)    DO_MAKE_STR(x)

#define VER_RELEASE    1

#define VER_MAJOR      0
#define VER_MINOR      10
#define VER_BUILD      9

#define VERSION_NUM    VER_MAJOR,VER_MINOR,VER_BUILD,REV_NUM
#define VERSION_STR    MAKE_STR(VER_MAJOR) "." MAKE_STR(VER_MINOR) "." MAKE_STR(VER_BUILD) "." MAKE_STR(REV_NUM)

// Numeric FileVersion remains compatible with installed updater shortcuts.
// Local/test ProductVersion and visible labels identify the exact source state.
#ifndef REV_PRODUCT_VERSION
#define REV_PRODUCT_VERSION VERSION_STR
#endif
#ifndef REV_SOURCE_ID
#define REV_SOURCE_ID "unknown"
#endif
#ifndef REV_COMMIT
#define REV_COMMIT "unknown"
#endif
#ifndef REV_IS_LOCAL
#define REV_IS_LOCAL 0
#endif
#define VERSION_DISPLAY REV_PRODUCT_VERSION

#define BRANCH_STR     MAKE_STR(REV_BRANCH)
