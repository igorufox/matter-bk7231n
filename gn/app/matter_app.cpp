/* ESPurna-facing entry of the Matter plug: the data model (plug.zap) and the server, nothing of the board's identity.
 * Device identity is ESPurna's (matter_identity.cpp of the fork, one code with the ESP32): the factory data of the
 * stock firmware (Quectel / Tuya) when the board has it, or an own identity made once per board (namespace
 * "espurna-id" of the key-value store). ESPurna sets its providers in matter_identity_install(), called before the
 * server init; without it the platform ones stay (SDK test values).
 * ESPurna -> Matter: matter_app_start(), matter_set_onoff(), matter_onboarding_codes(), matter_is_commissioned()
 * Matter -> ESPurna: matter_onoff_changed(), matter_identity_install() (weak), and the bk_* platform API of
 * matter_pal.h, which ESPurna implements (key-value store, Wi-Fi status and events, entropy)
 * Root node + one On/Off Plug-in Unit (0x010A) on endpoint 1 for a board with one relay, or a bridge for more relays:
 * Aggregator on endpoint 2 and a Bridged Node plug per relay on endpoints 3..6 (matter_plug_count(), EnablePlugs()).
 * On-network commissioning only (no BLE). */
#include "BekenAppServer.h"
#include "CHIPDeviceManager.h"
#include "CommonDeviceCallbacks.h"
#include "OTAHelper.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

#include <app/AttributeValueDecoder.h>
#include <app/clusters/basic-information/BasicInformationCluster.h>
#include <app/data-model-provider/OperationTypes.h>
#include <app/persistence/AttributePersistence.h>
#include <app/persistence/AttributePersistenceProviderInstance.h>
#include <app/persistence/String.h>
#include <lib/core/TLV.h>

#include <cstring>
#include <string>

#include <FreeRTOS.h>
#include <task.h>

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;
using namespace ::chip::DeviceManager;

extern "C" void matter_onoff_changed(uint8_t endpoint, bool on) __attribute__((weak));
// device name for Basic Information NodeLabel while it is empty (ESPurna hostname)
extern "C" const char * matter_node_label(void) __attribute__((weak));
// commissioning in progress: ESPurna gives the heap back (web socket clients) until it ends
extern "C" void matter_commissioning(bool active) __attribute__((weak));
// pairing window opened / closed
extern "C" void matter_window(bool open) __attribute__((weak));
// relays of the board = plugs in use (1..kPlugs); without it a single plug
extern "C" uint8_t matter_plug_count(void) __attribute__((weak));
// name (Bridged Device Basic Information NodeLabel) and unique id of plug 1..kPlugs when the device is a bridge
extern "C" bool matter_plug_name(uint8_t plug, char * label, size_t label_size, char * unique, size_t unique_size)
    __attribute__((weak));
// device type of the relay endpoints when the factory certification declaration names another one than the
// On/Off Plug-in Unit of plug.zap (MOES round 2-relay board: On/Off Light 0x0100); 0 keeps the plug
extern "C" uint16_t matter_device_type(void) __attribute__((weak));
// the identity of the board (ESPurna, matter_identity.cpp): sets the attestation, commissionable data and instance info
// providers; without it the ones of the platform (SDK test values)
extern "C" void matter_identity_install(void) __attribute__((weak));

namespace {

// Endpoints of the data model (plug.zap): a plain plug for boards with one relay, or a gateway (Aggregator) with up to
// kPlugs Bridged Node plugs (ref. EnablePlugs())
constexpr uint8_t kPlugs           = 4;
constexpr EndpointId kSinglePlug   = 1;
constexpr EndpointId kAggregator   = 2;
constexpr EndpointId kFirstBridged = 3;

// Set by EnablePlugs() in the Matter task, before any OnOff work item of ESPurna
bool sBridge   = false;
uint8_t sPlugs = 1;

// OnOff changes reach ESPurna only after the server init (ref. PlugCallbacks)
bool sServerReady = false;

void Commissioning(bool active)
{
    if (matter_commissioning)
    {
        matter_commissioning(active);
    }
}

// From the first PASE message to the end of commissioning (complete, failed, fail-safe expired)
class CommissioningDelegate : public AppDelegate
{
public:
    void OnCommissioningSessionEstablishmentStarted() override { Commissioning(true); }
    void OnCommissioningSessionEstablishmentError(CHIP_ERROR) override { Commissioning(false); }
    // not OnCommissioningWindowClosed: the window closes as soon as PASE is established, in the middle of commissioning
    void OnCommissioningSessionStopped() override { Commissioning(false); }

    // pairing window state for the ESPurna WebUI (grey button with a countdown)
    void OnCommissioningWindowOpened() override
    {
        if (matter_window)
        {
            matter_window(true);
        }
    }
    void OnCommissioningWindowClosed() override
    {
        if (matter_window)
        {
            matter_window(false);
        }
    }
};

CommissioningDelegate sCommissioningDelegate;

void OnDeviceEvent(const ChipDeviceEvent * event, intptr_t)
{
    if ((event->Type == DeviceEventType::kCommissioningComplete) || (event->Type == DeviceEventType::kFailSafeTimerExpired))
    {
        Commissioning(false);
    }
}

uint8_t EndpointPlug(EndpointId endpoint);

class PlugCallbacks : public CommonDeviceCallbacks
{
public:
    void PostAttributeChangeCallback(EndpointId endpointId, ClusterId clusterId, AttributeId attributeId, uint8_t type,
                                     uint16_t size, uint8_t * value) override
    {
        // not while the server starts: the OnOff cluster applies StartUpOnOff then, the relays keep the state ESPurna
        // gave them at boot and ESPurna sends it to Matter right after the start
        if (clusterId == Clusters::OnOff::Id && attributeId == Clusters::OnOff::Attributes::OnOff::Id && matter_onoff_changed &&
            sServerReady)
        {
            // ESPurna counts plugs (relay + 1), not endpoints
            const auto plug = EndpointPlug(endpointId);
            if (plug)
            {
                matter_onoff_changed(plug, *value != 0);
            }
        }
    }
};

PlugCallbacks sCallbacks;

void OnIdentifyStart(Identify *) {}
void OnIdentifyStop(Identify *) {}
Identify sIdentify[1 + kPlugs] = {
    { kSinglePlug, OnIdentifyStart, OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator },
    { kFirstBridged, OnIdentifyStart, OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator },
    { kFirstBridged + 1, OnIdentifyStart, OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator },
    { kFirstBridged + 2, OnIdentifyStart, OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator },
    { kFirstBridged + 3, OnIdentifyStart, OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator },
};

// plug (relay + 1) -> endpoint, kInvalidEndpointId when the board does not use it
EndpointId PlugEndpoint(uint8_t plug)
{
    if (sBridge)
    {
        return ((plug >= 1) && (plug <= sPlugs)) ? static_cast<EndpointId>(kFirstBridged + plug - 1) : kInvalidEndpointId;
    }

    return (plug == 1) ? kSinglePlug : kInvalidEndpointId;
}

// endpoint -> plug (relay + 1), 0 when it is not a plug in use
uint8_t EndpointPlug(EndpointId endpoint)
{
    if (sBridge)
    {
        return ((endpoint >= kFirstBridged) && (endpoint < kFirstBridged + sPlugs)) ? static_cast<uint8_t>(endpoint - kFirstBridged + 1)
                                                                                    : 0;
    }

    return (endpoint == kSinglePlug) ? 1 : 0;
}

// Bridged Device Basic Information of a plug: name and unique id from ESPurna (relay name or "<hostname> <n>")
void NamePlug(uint8_t plug)
{
    char label[33] = {};
    char unique[33] = {};
    if (!matter_plug_name || !matter_plug_name(plug, label, sizeof(label), unique, sizeof(unique)))
    {
        snprintf(label, sizeof(label), "Plug %u", static_cast<unsigned>(plug));
        snprintf(unique, sizeof(unique), "plug-%u", static_cast<unsigned>(plug));
    }

    const auto endpoint = PlugEndpoint(plug);
    Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Set(endpoint, CharSpan::fromCharString(label));
    Clusters::BridgedDeviceBasicInformation::Attributes::UniqueID::Set(endpoint, CharSpan::fromCharString(unique));
}

// One relay: a plain plug on endpoint 1 (what the boards had before, their controllers keep it). More relays: a bridge,
// the gateway (Aggregator) on endpoint 2 and a Bridged Node plug per relay from endpoint 3, the way the speaker hub shows
// one gateway with a plug per relay (a device with several plug endpoints showed up as a single plug there). The
// endpoints of the other layout and the plugs above the relay count are disabled, controllers do not see them
void EnablePlugs()
{
    uint8_t used = matter_plug_count ? matter_plug_count() : 1;
    if (used < 1)
    {
        used = 1;
    }
    if (used > kPlugs)
    {
        used = kPlugs;
    }

    sPlugs  = used;
    sBridge = used > 1;

    // A controller that knows the product by its VID/PID expects the device type of its certification declaration on
    // the endpoints (the speaker hub stays at the first step otherwise). Revisions as in plug.zap
    const uint16_t type = matter_device_type ? matter_device_type() : 0;
    if (type && (type != 0x010A))
    {
        static EmberAfDeviceType sSingle[1];
        static EmberAfDeviceType sBridged[2];
        sSingle[0]  = { type, 3 };
        sBridged[0] = { 0x0013, 3 };
        sBridged[1] = { type, 3 };

        emberAfSetDeviceTypeList(kSinglePlug, Span<const EmberAfDeviceType>(sSingle));
        for (uint8_t plug = 1; plug <= kPlugs; ++plug)
        {
            emberAfSetDeviceTypeList(static_cast<EndpointId>(kFirstBridged + plug - 1), Span<const EmberAfDeviceType>(sBridged));
        }
        ChipLogProgress(AppServer, "Device type 0x%04X on the relay endpoints (certification declaration)", type);
    }

    emberAfEndpointEnableDisable(kSinglePlug, !sBridge);
    emberAfEndpointEnableDisable(kAggregator, sBridge);
    for (uint8_t plug = 1; plug <= kPlugs; ++plug)
    {
        emberAfEndpointEnableDisable(static_cast<EndpointId>(kFirstBridged + plug - 1), sBridge && (plug <= used));
    }

    if (sBridge)
    {
        for (uint8_t plug = 1; plug <= used; ++plug)
        {
            NamePlug(plug);
        }
        ChipLogProgress(AppServer, "Bridge: gateway endpoint %u, %u plugs from endpoint %u", kAggregator, used, kFirstBridged);
    }
    else
    {
        ChipLogProgress(AppServer, "Single plug on endpoint %u", kSinglePlug);
    }
}

bool BuildPayload(SetupPayload & payload)
{
    uint32_t passcode      = 0;
    uint16_t discriminator = 0;
    uint16_t vendor        = 0;
    uint16_t product       = 0;
    if ((GetCommissionableDataProvider()->GetSetupPasscode(passcode) != CHIP_NO_ERROR) ||
        (GetCommissionableDataProvider()->GetSetupDiscriminator(discriminator) != CHIP_NO_ERROR) ||
        (GetDeviceInstanceInfoProvider()->GetVendorId(vendor) != CHIP_NO_ERROR) ||
        (GetDeviceInstanceInfoProvider()->GetProductId(product) != CHIP_NO_ERROR))
    {
        return false;
    }

    payload.version           = 0;
    payload.vendorID          = vendor;
    payload.productID         = product;
    payload.commissioningFlow = CommissioningFlow::kStandard;
    payload.rendezvousInformation.SetValue(RendezvousInformationFlags(RendezvousInformationFlag::kOnNetwork));
    payload.discriminator.SetLongValue(discriminator);
    payload.setUpPINCode = passcode;

    return true;
}

// NodeLabel = the ESPurna hostname while no controller has written one
void SetDefaultNodeLabel(const char * name)
{
    const CharSpan wanted(name, strnlen(name, 32));
    // code-driven Basic Information cluster (connectedhomeip 1.5), no ember accessors. Read the stored label, and write
    // the default the way a controller would (memory and persistence of the cluster stay in step)
    Storage::String<32> stored;
    AttributePersistence persistence(*GetAttributePersistenceProvider());
    persistence.LoadString({ kRootEndpointId, Clusters::BasicInformation::Id, Clusters::BasicInformation::Attributes::NodeLabel::Id },
                           stored);
    if (!stored.Content().empty())
    {
        return;
    }

    uint8_t buffer[48];
    TLV::TLVWriter writer;
    writer.Init(buffer);
    if (writer.PutString(TLV::AnonymousTag(), wanted) != CHIP_NO_ERROR || writer.Finalize() != CHIP_NO_ERROR)
    {
        return;
    }

    TLV::TLVReader reader;
    reader.Init(buffer, writer.GetLengthWritten());
    if (reader.Next() != CHIP_NO_ERROR)
    {
        return;
    }

    Access::SubjectDescriptor subject;
    AttributeValueDecoder decoder(reader, subject);
    DataModel::WriteAttributeRequest request;
    request.path = ConcreteDataAttributePath(kRootEndpointId, Clusters::BasicInformation::Id,
                                             Clusters::BasicInformation::Attributes::NodeLabel::Id);
    Clusters::BasicInformationCluster::Instance().WriteAttribute(request, decoder);
}

void InitServer(intptr_t)
{
    // the identity of the board (factory data or its own, ESPurna's matter_identity.cpp): attestation, commissionable
    // data and instance info, before the server reads them
    if (matter_identity_install)
    {
        matter_identity_install();
    }
    BekenAppServer::Init(&sCommissioningDelegate);
    EnablePlugs();
    PlatformMgr().AddEventHandler(OnDeviceEvent, 0);
#if CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID == 0xFFF1
    // test build: SDK example DAC / PAI / CD of VID 0xFFF1, commissioners treat it as an uncertified device
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());
#endif
    OTAHelpers::Instance().InitOTARequestor();
    // An empty NodeLabel is the first thing a controller shows as the device name (the speaker hub keeps the ESP32 lab board
    // that reports one), until a controller writes its own
    if (matter_node_label)
    {
        SetDefaultNodeLabel(matter_node_label());
    }
    // Wi-Fi belongs to ESPurna: Matter only listens to its connected / got IP events (bk_event_register_cb)
    ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Enabled);
    sServerReady = true;
}

// arg: plug (relay + 1) << 1 | on
void SetOnOff(intptr_t arg)
{
    const auto endpoint = PlugEndpoint(static_cast<uint8_t>(arg >> 1));
    if (endpoint != kInvalidEndpointId)
    {
        Clusters::OnOff::Attributes::OnOff::Set(endpoint, (arg & 1) != 0);
    }
}

void CopyString(const std::string & in, char * out, size_t size)
{
    if (size)
    {
        const size_t length = (in.size() < size) ? in.size() : (size - 1);
        memcpy(out, in.data(), length);
        out[length] = '\0';
    }
}

} // namespace

extern "C" int matter_app_start(void)
{
    if (CHIPDeviceManager::GetInstance().Init(&sCallbacks) != CHIP_NO_ERROR)
        return -1;
    PlatformMgr().ScheduleWork(InitServer, 0);
    return 0;
}

// Plug endpoints the library has (ESPurna links older libraries with a single one, it asks through a weak reference)
extern "C" uint8_t matter_app_plugs(void)
{
    return kPlugs;
}

// plug = relay + 1 (the endpoint was the same number before the bridge layout)
extern "C" void matter_set_onoff(uint8_t plug, bool on)
{
    PlatformMgr().ScheduleWork(SetOnOff, (static_cast<intptr_t>(plug) << 1) | (on ? 1 : 0));
}

// Manual pairing code and QR payload. They carry the setup passcode: only for the device owner
extern "C" bool matter_onboarding_codes(char * manual, size_t manual_size, char * qr, size_t qr_size)
{
    SetupPayload payload;
    if (!BuildPayload(payload))
    {
        return false;
    }

    std::string out;
    if (ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(out) != CHIP_NO_ERROR)
    {
        return false;
    }
    CopyString(out, manual, manual_size);

    out.clear();
    if (QRCodeSetupPayloadGenerator(payload).payloadBase38Representation(out) != CHIP_NO_ERROR)
    {
        return false;
    }
    CopyString(out, qr, qr_size);

    return true;
}

// Liveness check of the Matter task: a work item that calls back into ESPurna
extern "C" void matter_pong(void) __attribute__((weak));

extern "C" void matter_ping(void)
{
    PlatformMgr().ScheduleWork(
        [](intptr_t) {
            ChipLogProgress(AppServer, "ping");
            if (matter_pong)
                matter_pong();
        },
        0);
}

// Commissioning window again (it closes after 15 minutes, or after the device was commissioned)
extern "C" void matter_open_window(void)
{
    PlatformMgr().ScheduleWork(
        [](intptr_t) {
            auto & manager = Server::GetInstance().GetCommissioningWindowManager();
            if (!manager.IsCommissioningWindowOpen())
            {
                CHIP_ERROR err = manager.OpenBasicCommissioningWindow();
                ChipLogProgress(AppServer, "Commissioning window open: %" CHIP_ERROR_FORMAT, err.Format());
            }
        },
        0);
}

extern "C" int matter_fabric_count(void)
{
    return Server::GetInstance().GetFabricTable().FabricCount();
}

// Controllers the device was added to (fabrics), for the list of the ESPurna WebUI: the n-th one (0..), false after the
// last. label gets the fabric label, cut to label_size - 1
extern "C" bool matter_fabric_info(int n, uint8_t * index, uint16_t * vendor, uint64_t * node, char * label, size_t label_size)
{
    bool found = false;
    PlatformMgr().LockChipStack();
    int position = 0;
    for (const auto & fabric : Server::GetInstance().GetFabricTable())
    {
        if (position++ != n)
        {
            continue;
        }

        *index  = fabric.GetFabricIndex();
        *vendor = static_cast<uint16_t>(fabric.GetVendorId());
        *node   = fabric.GetNodeId();
        if (label_size)
        {
            const auto text   = fabric.GetFabricLabel();
            const size_t size = (text.size() < label_size) ? text.size() : (label_size - 1);
            memcpy(label, text.data(), size);
            label[size] = '\0';
        }
        found = true;
        break;
    }
    PlatformMgr().UnlockChipStack();
    return found;
}

// The device leaves a controller, like on its RemoveFabric command (the server drops the sessions, access entries and
// group keys of the fabric, and opens the pairing window when it was the last one). In the Matter task
extern "C" bool matter_remove_fabric(uint8_t index)
{
    PlatformMgr().LockChipStack();
    const bool found = Server::GetInstance().GetFabricTable().FindFabricWithIndex(index) != nullptr;
    PlatformMgr().UnlockChipStack();
    if (!found)
    {
        return false;
    }

    PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            const auto fabric = static_cast<FabricIndex>(arg);
            CHIP_ERROR err    = Server::GetInstance().GetFabricTable().Delete(fabric);
            ChipLogProgress(AppServer, "Fabric %u removed: %" CHIP_ERROR_FORMAT, static_cast<unsigned>(fabric), err.Format());
        },
        static_cast<intptr_t>(index));
    return true;
}
