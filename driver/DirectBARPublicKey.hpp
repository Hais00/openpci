#pragma once

#include <stdint.h>

#if __has_include("DirectBARPublicKey.local.hpp")
#include "DirectBARPublicKey.local.hpp"
#else
static constexpr bool openpci_direct_bar_key_configured = false;
static constexpr uint8_t openpci_direct_bar_public_key[32] = {};
#endif
