/* Test build: the SDK example attestation (DAC / PAI / CD of VID 0xFFF1, PID 0x8001) instead of the Quectel factory data,
 * everything else as in ProjectConfig.h. Commissioning passcode and discriminator still come from the factory data */
#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID 0xFFF1
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID 0x8001
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "ESPurna test"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "ESPurna Matter plug"

#include "ProjectConfig.h"
