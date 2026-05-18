#pragma once
#include "config_store.h"

// Runs the captive-portal setup UI on a SoftAP. Blocks until the user
// hits "Save & restart". Reboots the chip on save (does not return).
[[noreturn]] void runSetupPortal(AppConfig& cfg);
