// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "execution/i_rest_client.hpp"

#include <cstdlib>

namespace spreadara::execution {

bool credentials_present() {
    const char* k = std::getenv("SPREADARA_API_KEY");
    const char* s = std::getenv("SPREADARA_API_SECRET");
    return k != nullptr && s != nullptr && *k != '\0' && *s != '\0';
}

}
