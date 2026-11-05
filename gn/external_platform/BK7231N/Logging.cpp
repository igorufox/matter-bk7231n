/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *          Provides implementations for the CHIP logging functions
 *          on the Beken platform.
 */
/* this file behaves like a config.h, comes first */
#include <platform/logging/LogV.h>

#include <lib/core/CHIPConfig.h>
#include <lib/support/logging/Constants.h>

#include <stdio.h>
extern "C" void bk_printf(const char * fmt, ...);

#ifdef LOG_LOCAL_LEVEL
#undef LOG_LOCAL_LEVEL
#endif

namespace chip {
namespace Logging {
namespace Platform {

// ESPurna prints the lines from its loop (matter_beken.cpp), bk_printf when built without it
extern "C" void matter_log(const char * line) __attribute__((weak));

void LogV(const char * module, uint8_t category, const char * msg, va_list v)
{
    char line[CHIP_CONFIG_LOG_MESSAGE_MAX_SIZE + 16];

    const int prefix = snprintf(line, sizeof(line), "%s%s ", (category == kLogCategory_Error) ? "E " : "", module);
    if ((prefix > 0) && (static_cast<size_t>(prefix) < sizeof(line)))
    {
        vsnprintf(line + prefix, sizeof(line) - prefix, msg, v);
    }

    if (matter_log)
    {
        matter_log(line);
    }
    else
    {
        bk_printf("chip %s\r\n", line);
    }
}

} // namespace Platform
} // namespace Logging
} // namespace chip
