/* ESPurna-facing entry of the Matter plug.
 * ESPurna -> Matter: matter_app_start(), matter_set_onoff(), matter_onboarding_codes(), matter_is_commissioned()
 * Matter -> ESPurna: matter_onoff_changed() (weak), and the bk_* platform API of matter_pal.h, which ESPurna implements
 * (key-value store, Wi-Fi status and events, entropy)
 * Root node + On/Off Plug-in Unit (0x010A) on endpoint 1, on-network commissioning only (no BLE).
 * Device attestation from the factory data the stock firmware left in flash (namespace "chip-factory"). */
#include "BekenAppServer.h"
#include "CHIPDeviceManager.h"
#include "CommonDeviceCallbacks.h"
#include "OTAHelper.h"

// matter_pal.h of the platform, implemented by ESPurna
extern "C" int bk_read_data(const char * ns, const char * name, char * buf, uint32_t len, uint32_t * out_len);

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include <app/AttributeValueDecoder.h>
#include <app/clusters/basic-information/BasicInformationCluster.h>
#include <app/data-model-provider/OperationTypes.h>
#include <app/persistence/AttributePersistence.h>
#include <app/persistence/AttributePersistenceProviderInstance.h>
#include <app/persistence/String.h>
#include <lib/core/TLV.h>

#include <cstring>
#include <string>

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;
using namespace ::chip::DeviceManager;

extern "C" void matter_onoff_changed(uint8_t endpoint, bool on) __attribute__((weak));
// device name for Basic Information NodeLabel while it is empty (ESPurna hostname)
extern "C" const char * matter_node_label(void) __attribute__((weak));
// commissioning in progress: ESPurna gives the heap back (web socket clients) until it ends
extern "C" void matter_commissioning(bool active) __attribute__((weak));

namespace {

constexpr char FactoryNamespace[] = "chip-factory";

CHIP_ERROR ReadFactory(const char * name, MutableByteSpan & out)
{
    uint32_t length = 0;
    if (bk_read_data(FactoryNamespace, name, reinterpret_cast<char *>(out.data()), out.size(), &length) != 0)
    {
        return CHIP_ERROR_NOT_FOUND;
    }
    if (length > out.size())
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    out.reduce_size(length);
    return CHIP_NO_ERROR;
}

// dac-cert, pai-cert, cert-dclrn, dac-pub-key and dac-key as Quectel stored them
class FactoryAttestationProvider : public Credentials::DeviceAttestationCredentialsProvider
{
public:
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan & out) override { return ReadFactory("cert-dclrn", out); }
    CHIP_ERROR GetFirmwareInformation(MutableByteSpan & out) override
    {
        out.reduce_size(0);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan & out) override { return ReadFactory("dac-cert", out); }
    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan & out) override { return ReadFactory("pai-cert", out); }

    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan & message, MutableByteSpan & signature) override
    {
        Crypto::P256SerializedKeypair serialized;
        MutableByteSpan pub(serialized.Bytes(), Crypto::kP256_PublicKey_Length);
        MutableByteSpan priv(serialized.Bytes() + Crypto::kP256_PublicKey_Length, Crypto::kP256_PrivateKey_Length);

        CHIP_ERROR err = ReadFactory("dac-pub-key", pub);
        if (err == CHIP_NO_ERROR)
        {
            err = ReadFactory("dac-key", priv);
        }
        if ((err == CHIP_NO_ERROR) &&
            ((pub.size() != Crypto::kP256_PublicKey_Length) || (priv.size() != Crypto::kP256_PrivateKey_Length)))
        {
            err = CHIP_ERROR_INVALID_ARGUMENT;
        }

        Crypto::P256ECDSASignature result;
        if (err == CHIP_NO_ERROR)
        {
            serialized.SetLength(Crypto::kP256_PublicKey_Length + Crypto::kP256_PrivateKey_Length);
            Crypto::P256Keypair keypair;
            err = keypair.Deserialize(serialized);
            if (err == CHIP_NO_ERROR)
            {
                err = keypair.ECDSA_sign_msg(message.data(), message.size(), result);
            }
        }

        memset(serialized.Bytes(), 0, serialized.Capacity());
        ReturnErrorOnFailure(err);

        return CopySpanToMutableSpan(ByteSpan{ result.ConstBytes(), result.Length() }, signature);
    }
};

FactoryAttestationProvider sAttestation;

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
};

CommissioningDelegate sCommissioningDelegate;

void OnDeviceEvent(const ChipDeviceEvent * event, intptr_t)
{
    if ((event->Type == DeviceEventType::kCommissioningComplete) || (event->Type == DeviceEventType::kFailSafeTimerExpired))
    {
        Commissioning(false);
    }
}

class PlugCallbacks : public CommonDeviceCallbacks
{
public:
    void PostAttributeChangeCallback(EndpointId endpointId, ClusterId clusterId, AttributeId attributeId, uint8_t type,
                                     uint16_t size, uint8_t * value) override
    {
        if (clusterId == Clusters::OnOff::Id && attributeId == Clusters::OnOff::Attributes::OnOff::Id && matter_onoff_changed)
        {
            matter_onoff_changed(static_cast<uint8_t>(endpointId), *value != 0);
        }
    }
};

PlugCallbacks sCallbacks;

void OnIdentifyStart(Identify *) {}
void OnIdentifyStop(Identify *) {}
Identify sIdentify1 = { EndpointId{ 1 }, OnIdentifyStart, OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator };

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
    BekenAppServer::Init(&sCommissioningDelegate);
    PlatformMgr().AddEventHandler(OnDeviceEvent, 0);
#if CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID == 0xFFF1
    // test build: SDK example DAC / PAI / CD of VID 0xFFF1, commissioners treat it as an uncertified device
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());
#else
    Credentials::SetDeviceAttestationCredentialsProvider(&sAttestation);
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
}

void SetOnOff(intptr_t arg)
{
    const auto endpoint = static_cast<EndpointId>(arg >> 1);
    Clusters::OnOff::Attributes::OnOff::Set(endpoint, (arg & 1) != 0);
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

extern "C" void matter_set_onoff(uint8_t endpoint, bool on)
{
    PlatformMgr().ScheduleWork(SetOnOff, (static_cast<intptr_t>(endpoint) << 1) | (on ? 1 : 0));
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
