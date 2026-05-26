#pragma once

#ifndef MEMORY_POOL_VERSION_MAJOR
#define MEMORY_POOL_VERSION_MAJOR 0
#endif

#ifndef MEMORY_POOL_VERSION_MINOR
#define MEMORY_POOL_VERSION_MINOR 1
#endif

#ifndef MEMORY_POOL_VERSION_PATCH
#define MEMORY_POOL_VERSION_PATCH 0
#endif

#define MEMORY_POOL_DETAIL_STRINGIFY(value) #value
#define MEMORY_POOL_DETAIL_EXPAND_AND_STRINGIFY(value) MEMORY_POOL_DETAIL_STRINGIFY(value)

#ifndef MEMORY_POOL_VERSION_STRING
#define MEMORY_POOL_VERSION_STRING                                                                        \
    MEMORY_POOL_DETAIL_EXPAND_AND_STRINGIFY(MEMORY_POOL_VERSION_MAJOR) "."                                \
        MEMORY_POOL_DETAIL_EXPAND_AND_STRINGIFY(MEMORY_POOL_VERSION_MINOR) "."                            \
            MEMORY_POOL_DETAIL_EXPAND_AND_STRINGIFY(MEMORY_POOL_VERSION_PATCH)
#endif

#ifndef MEMORY_POOL_ENABLE_TRACKING_BY_DEFAULT
#define MEMORY_POOL_ENABLE_TRACKING_BY_DEFAULT 1
#endif

namespace memory_pool {
    inline constexpr int version_major = MEMORY_POOL_VERSION_MAJOR;
    inline constexpr int version_minor = MEMORY_POOL_VERSION_MINOR;
    inline constexpr int version_patch = MEMORY_POOL_VERSION_PATCH;
    inline constexpr const char *version_string = MEMORY_POOL_VERSION_STRING;
    inline constexpr bool tracking_enabled_by_default = MEMORY_POOL_ENABLE_TRACKING_BY_DEFAULT != 0;
} // namespace memory_pool
