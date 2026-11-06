/* RAM profile for BK7231N inside ESPurna. Used both as the project config (CHIP_CONFIG_*, CHIP_IM_*) and as the
 * device project config (CHIP_DEVICE_CONFIG_*): gn args chip_project_config_include and
 * chip_device_project_config_include.
 * 3 fabrics (the hub, matter-server and one more; 5 with their pools cost the heap a pairing needs: a 4-relay bridge
 * had 3.5 KB left while the hub commissioned) with the per-fabric minimums the SDK asserts
 * (3 subscriptions of 3 paths, 1 read of 9 paths per fabric). The bring-up profile had 2 fabrics, 4 sessions and
 * 6 exchanges ("mRetransTable Already Full" at CommissioningComplete); since the static data of the library is in the
 * data TCM (ESPurna scripts/beken_tcm.py) the heap has ~40 KB to spare with Matter running. */
#pragma once

#define CHIP_CONFIG_MAX_FABRICS 3
#define CHIP_CONFIG_SECURE_SESSION_POOL_SIZE 8
#define CHIP_CONFIG_UNAUTHENTICATED_CONNECTION_POOL_SIZE 4
#define CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS 12
#define CHIP_CONFIG_MAX_GROUP_DATA_PEERS 2
#define CHIP_CONFIG_MAX_GROUP_CONTROL_PEERS 1
#define CHIP_CONFIG_DEVICE_MAX_ACTIVE_DEVICES 2
#define CHIP_CONFIG_EXAMPLE_ACCESS_CONTROL_MAX_ENTRIES_PER_FABRIC 4

#define CHIP_IM_MAX_NUM_SUBSCRIPTIONS 9
#define CHIP_IM_MAX_NUM_READS 3
#define CHIP_IM_SERVER_MAX_NUM_PATH_GROUPS_FOR_SUBSCRIPTIONS 27
#define CHIP_IM_SERVER_MAX_NUM_PATH_GROUPS_FOR_READS 27
#define CHIP_IM_SERVER_MAX_NUM_DIRTY_SET 4
#define CHIP_IM_MAX_NUM_COMMAND_HANDLER 2
#define CHIP_IM_MAX_NUM_WRITE_HANDLER 2
#define CHIP_IM_MAX_NUM_WRITE_CLIENT 0
#define CHIP_IM_MAX_NUM_TIMED_HANDLER 2
#define CHIP_IM_MAX_REPORTS_IN_FLIGHT 4

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


/* The SDK default 8 KB again: 6 KB was a cut when commissioning ran out of heap at AddNOC (before the TCM move). The task
 * stack is not measured (no uxTaskGetStackHighWaterMark in the BDK FreeRTOS) */
#define CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE 8192

/* Basic Information hardware version: "TEST_VERSION" is the SDK placeholder (the ESP32 lab board that the hub keeps reports 1 / "1") */
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION 1
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "1"
