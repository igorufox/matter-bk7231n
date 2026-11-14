/*
 * The platform DNS-SD of Matter on both chips (chip_mdns = "platform"): ESPurna's one mDNS responder
 * (espurna/libs/mdns_responder.h of ESPurna, its mdns module) publishes Matter's services; the library has no mDNS of
 * its own. Before, the minimal mDNS of connectedhomeip held UDP 5353 and ESPurna's responder could not run next to it
 * (no host name, no _http._tcp with the matter module on).
 *
 * A device only publishes: browsing and resolving (a controller's job) are not there.
 */

#include <lib/dnssd/platform/Dnssd.h>
#include <lib/support/CodeUtils.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
// ESPurna's mdns module
void espurna_mdns_update(const char* owner);
bool espurna_mdns_add(const char* owner, const char* instance, const char* type, bool tcp, uint16_t port, uint32_t ttl,
    const char* const* txt, size_t txt_count, const char* const* subtypes, size_t subtypes_count);
void espurna_mdns_commit(const char* owner);
}

namespace chip {
namespace Dnssd {
namespace {

constexpr char Owner[] = "matter";

bool updating = false;

void begin()
{
    if (!updating)
    {
        espurna_mdns_update(Owner);
        updating = true;
    }
}

} // namespace

CHIP_ERROR ChipDnssdInit(DnssdAsyncReturnCallback initCallback, DnssdAsyncReturnCallback, void * context)
{
    initCallback(context, CHIP_NO_ERROR);
    return CHIP_NO_ERROR;
}

void ChipDnssdShutdown()
{
    espurna_mdns_update(Owner);
    espurna_mdns_commit(Owner);
    updating = false;
}

CHIP_ERROR ChipDnssdRemoveServices()
{
    begin();
    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdPublishService(const DnssdService * service, DnssdPublishCallback callback, void * context)
{
    VerifyOrReturnError(service != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    begin();

    std::vector<std::string> entries;
    entries.reserve(service->mTextEntrySize);
    for (size_t index = 0; index < service->mTextEntrySize; ++index)
    {
        const auto & entry = service->mTextEntries[index];
        std::string text(entry.mKey ? entry.mKey : "");
        if (entry.mData && entry.mDataSize)
        {
            text += '=';
            text.append(reinterpret_cast<const char *>(entry.mData), entry.mDataSize);
        }
        entries.push_back(std::move(text));
    }

    std::vector<const char *> txt;
    txt.reserve(entries.size());
    for (const auto & entry : entries)
    {
        txt.push_back(entry.c_str());
    }

    const bool tcp = (service->mProtocol == DnssdServiceProtocol::kDnssdProtocolTcp);
    const bool ok  = espurna_mdns_add(Owner, service->mName, service->mType, tcp, service->mPort, service->mTtlSeconds,
                                     txt.data(), txt.size(), service->mSubTypes, service->mSubTypeSize);
    const CHIP_ERROR error = ok ? CHIP_NO_ERROR : CHIP_ERROR_NO_MEMORY;
    if (callback != nullptr)
    {
        callback(context, service->mType, service->mName, error);
    }

    return error;
}

CHIP_ERROR ChipDnssdFinalizeServiceUpdate()
{
    if (updating)
    {
        espurna_mdns_commit(Owner);
        updating = false;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdBrowse(const char *, DnssdServiceProtocol, Inet::IPAddressType, Inet::InterfaceId, DnssdBrowseCallback,
                           void *, intptr_t *)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ChipDnssdStopBrowse(intptr_t)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ChipDnssdResolve(DnssdService *, Inet::InterfaceId, DnssdResolveCallback, void *)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

void ChipDnssdResolveNoLongerNeeded(const char *) {}

CHIP_ERROR ChipDnssdReconfirmRecord(const char *, Inet::IPAddress, Inet::InterfaceId)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

} // namespace Dnssd
} // namespace chip
