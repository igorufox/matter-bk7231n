/* RAM profile for BK7231N inside ESPurna. Used both as the project config (CHIP_CONFIG_*, CHIP_IM_*) and as the
 * device project config (CHIP_DEVICE_CONFIG_*): gn args chip_project_config_include and
 * chip_device_project_config_include.
 * Bring-up profile: 2 fabrics (spec: 5) with the per-fabric minimums the SDK asserts (3 subscriptions of 3 paths,
 * 1 read of 9 paths per fabric). ESPurna leaves ~48 KB of heap
 * and the default pools take more than that. To be raised once it runs and the real heap use is known. */
#pragma once

#define CHIP_CONFIG_MAX_FABRICS 2
#define CHIP_CONFIG_SECURE_SESSION_POOL_SIZE 4
#define CHIP_CONFIG_UNAUTHENTICATED_CONNECTION_POOL_SIZE 2
#define CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS 6
#define CHIP_CONFIG_MAX_GROUP_DATA_PEERS 2
#define CHIP_CONFIG_MAX_GROUP_CONTROL_PEERS 1
#define CHIP_CONFIG_DEVICE_MAX_ACTIVE_DEVICES 2
#define CHIP_CONFIG_EXAMPLE_ACCESS_CONTROL_MAX_ENTRIES_PER_FABRIC 2

#define CHIP_IM_MAX_NUM_SUBSCRIPTIONS 6
#define CHIP_IM_MAX_NUM_READS 2
#define CHIP_IM_SERVER_MAX_NUM_PATH_GROUPS_FOR_SUBSCRIPTIONS 18
#define CHIP_IM_SERVER_MAX_NUM_PATH_GROUPS_FOR_READS 18
#define CHIP_IM_SERVER_MAX_NUM_DIRTY_SET 4
#define CHIP_IM_MAX_NUM_COMMAND_HANDLER 2
#define CHIP_IM_MAX_NUM_WRITE_HANDLER 2
#define CHIP_IM_MAX_NUM_WRITE_CLIENT 0
#define CHIP_IM_MAX_NUM_TIMED_HANDLER 2
#define CHIP_IM_MAX_REPORTS_IN_FLIGHT 2

#define CHIP_DEVICE_CONFIG_MAX_EVENT_QUEUE_SIZE 64
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_CRIT_BUFFER_SIZE 512
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_INFO_BUFFER_SIZE 256
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_DEBUG_BUFFER_SIZE 128

/* Identity of the Sparkleiot MINI (XH-CB2S) factory data: the DAC, PAI and certification declaration were issued
 * for Quectel VID 0x1410 / PID 0x03EA, a commissioner rejects the device if Basic Information reports other ids */
#ifndef CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID 0x1410
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID 0x03EA
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "Quectel"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "Quectel_Matter_Product"
/* the version the stock firmware reports (chip-config/software-version 0, "0.0" in its key-value store): Matter
 * certification is per software version, a controller may check it against the DCL record of the VID / PID */
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 0
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "0.0"
#endif


/* Commissioning ran out of heap at AddNOC (mbedTLS MPI alloc failed with 2.2 KB left): 2 KB of the 8 KB task stack
 * back to the heap. The task stack is not measured yet (no uxTaskGetStackHighWaterMark in the BDK FreeRTOS) */
#define CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE 6144

/* Basic Information hardware version: "TEST_VERSION" is the SDK placeholder (the ESP32 lab board that the hub keeps reports 1 / "1") */
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION 1
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "1"
