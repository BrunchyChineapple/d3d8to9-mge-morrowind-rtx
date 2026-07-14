#pragma once

#ifdef MGE_RTX

#include <cstdint>

struct remixapi_Interface;

namespace RemixAPITest {
    bool initialize();
    bool isInitialized();
    remixapi_Interface* getInterface();
    void shutdown();
}

#endif // MGE_RTX
