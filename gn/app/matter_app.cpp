/* ESPurna-facing entry of the Matter plug.
 * Device identity: the factory data of the stock firmware (Quectel) when the board has it. Boards without one (Tuya
 * CB3S / CBU, 2 MiB) make an own identity once and keep it in the key-value store (namespace "espurna-id"): random
 * setup passcode, discriminator and salt, and a DAC with its own key, signed by the public development PAI
 * "Matter Dev PAI 0xFFF1 no PID" (VID 0xFFF1 / PID 0x8000, the SDK example certification declaration). Controllers
 * that accept the SDK example devices accept it; it is not a certified identity.
 * ESPurna -> Matter: matter_app_start(), matter_set_onoff(), matter_onboarding_codes(), matter_is_commissioned()
 * Matter -> ESPurna: matter_onoff_changed() (weak), and the bk_* platform API of matter_pal.h, which ESPurna implements
 * (key-value store, Wi-Fi status and events, entropy)
 * Root node + one On/Off Plug-in Unit (0x010A) on endpoint 1 for a board with one relay, or a bridge for more relays:
 * Aggregator on endpoint 2 and a Bridged Node plug per relay on endpoints 3..6 (matter_plug_count(), EnablePlugs()).
 * On-network commissioning only (no BLE).
 * Device attestation from the factory data the stock firmware left in flash (namespace "chip-factory"). */
#include "BekenAppServer.h"
#include "CHIPDeviceManager.h"
#include "CommonDeviceCallbacks.h"
#include "OTAHelper.h"

// matter_pal.h of the platform, implemented by ESPurna
extern "C" int bk_read_data(const char * ns, const char * name, char * buf, uint32_t len, uint32_t * out_len);
extern "C" int bk_write_data(const char * ns, const char * name, const char * buf, uint32_t len);

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
#include <crypto/CHIPCryptoPAL.h>
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

// ---------------------------------------------------------------------------------------------------------------------
// Own identity, for boards without factory data

namespace own {

constexpr char Namespace[]      = "espurna-id";
constexpr char KeyIdentity[]    = "identity";
constexpr char KeyCertificate[] = "dac";

constexpr uint16_t VendorId  = 0xFFF1;
constexpr uint16_t ProductId = 0x8000;

// SDK development DAC FFF1/8000, the part to be signed. Serial number, public key and its key identifier are patched in
constexpr uint8_t DacTbs[] = {
    0x30, 0x82, 0x01, 0x8e, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x08, 0x23, 0x8a, 0x64, 0x7b, 0xbc, 0x4c, 0x30, 0xdd, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x3d, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55,
    0x04, 0x03, 0x0c, 0x1c, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x44, 0x65, 0x76, 0x20, 0x50, 0x41, 0x49, 0x20, 0x30,
    0x78, 0x46, 0x46, 0x46, 0x31, 0x20, 0x6e, 0x6f, 0x20, 0x50, 0x49, 0x44, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06,
    0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c, 0x04, 0x46, 0x46, 0x46, 0x31, 0x30, 0x20, 0x17, 0x0d, 0x32, 0x32,
    0x30, 0x32, 0x30, 0x35, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x18, 0x0f, 0x39, 0x39, 0x39, 0x39, 0x31, 0x32, 0x33,
    0x31, 0x32, 0x33, 0x35, 0x39, 0x35, 0x39, 0x5a, 0x30, 0x53, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c,
    0x1c, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x44, 0x65, 0x76, 0x20, 0x44, 0x41, 0x43, 0x20, 0x30, 0x78, 0x46, 0x46,
    0x46, 0x31, 0x2f, 0x30, 0x78, 0x38, 0x30, 0x30, 0x30, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01,
    0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c, 0x04, 0x46, 0x46, 0x46, 0x31, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01,
    0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x02, 0x0c, 0x04, 0x38, 0x30, 0x30, 0x30, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04,
    0x62, 0xdb, 0x16, 0xba, 0xde, 0xa3, 0x26, 0xa6, 0xdb, 0x84, 0x81, 0x4a, 0x06, 0x3f, 0xc6, 0xc7, 0xe9, 0xe2, 0xb1, 0x01,
    0xb7, 0x21, 0x64, 0x8e, 0xba, 0x4e, 0x5a, 0xc8, 0x40, 0xf5, 0xda, 0x30, 0x1e, 0xe6, 0x18, 0x12, 0x4e, 0xb4, 0x18, 0x0e,
    0x2f, 0xc3, 0xa2, 0x04, 0x7a, 0x56, 0x4b, 0xa9, 0xbc, 0xfa, 0x0b, 0xf7, 0x1f, 0x60, 0xce, 0x89, 0x30, 0xf1, 0xe7, 0xf6,
    0x6e, 0xc8, 0xd7, 0x28, 0xa3, 0x60, 0x30, 0x5e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x02,
    0x30, 0x00, 0x30, 0x0e, 0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x07, 0x80, 0x30, 0x1d,
    0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0xbc, 0xf7, 0xb0, 0x07, 0x49, 0x70, 0x63, 0x60, 0x6a, 0x26, 0xbe,
    0x4e, 0x08, 0x7c, 0x59, 0x56, 0x87, 0x74, 0x5a, 0x5a, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16,
    0x80, 0x14, 0x63, 0x54, 0x0e, 0x47, 0xf6, 0x4b, 0x1c, 0x38, 0xd1, 0x38, 0x84, 0xa4, 0x62, 0xd1, 0x6c, 0x19, 0x5d, 0x8f,
    0xfb, 0x3c,
};

constexpr size_t SerialOffset    = 11;
constexpr size_t SerialLength    = 8;
constexpr size_t PublicKeyOffset = 239;
constexpr size_t KeyIdOffset     = 349;

// ecdsa-with-SHA256
constexpr uint8_t SignatureAlgorithm[] = { 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };

// Matter-Development-PAI-FFF1-noPID key pair (public development credentials of connectedhomeip)
constexpr uint8_t PaiPublicKey[] = {
    0x04, 0x41, 0x9a, 0x93, 0x15, 0xc2, 0x17, 0x3e, 0x0c, 0x8c, 0x87, 0x6d, 0x03, 0xcc, 0xfc, 0x94, 0x48, 0x52, 0x64,
    0x7f, 0x7f, 0xec, 0x5e, 0x50, 0x82, 0xf4, 0x05, 0x99, 0x28, 0xec, 0xa8, 0x94, 0xc5, 0x94, 0x15, 0x13, 0x09, 0xac,
    0x63, 0x1e, 0x4c, 0xb0, 0x33, 0x92, 0xaf, 0x68, 0x4b, 0x0b, 0xaf, 0xb7, 0xe6, 0x5b, 0x3b, 0x81, 0x62, 0xc2, 0xf5,
    0x2b, 0xf9, 0x31, 0xb8, 0xe7, 0x7a, 0xaa, 0x82,
};

constexpr uint8_t PaiPrivateKey[] = {
    0xc1, 0xab, 0x5f, 0xe2, 0x84, 0xb4, 0xc1, 0x89, 0x40, 0xa5, 0x31, 0x61, 0xf8, 0x06, 0x94, 0x40,
    0x50, 0xa1, 0x69, 0x7b, 0x71, 0x76, 0x1d, 0x38, 0x53, 0x37, 0xa8, 0xa3, 0xcd, 0x09, 0x5c, 0x34,
};

// connectedhomeip credentials/examples: development PAI "Matter Dev PAI 0xFFF1 no PID" and the certification
// declaration of the SDK examples (VID 0xFFF1, PID 0x8000..0x8063)
constexpr uint8_t PaiCertificate[] = {
    0x30, 0x82, 0x01, 0xcb, 0x30, 0x82, 0x01, 0x71, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x08, 0x56, 0xad, 0x82, 0x22, 0xad,
    0x94, 0x5b, 0x64, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x30, 0x31, 0x18, 0x30,
    0x16, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0f, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20,
    0x50, 0x41, 0x41, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c,
    0x04, 0x46, 0x46, 0x46, 0x31, 0x30, 0x20, 0x17, 0x0d, 0x32, 0x32, 0x30, 0x32, 0x30, 0x35, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x5a, 0x18, 0x0f, 0x39, 0x39, 0x39, 0x39, 0x31, 0x32, 0x33, 0x31, 0x32, 0x33, 0x35, 0x39, 0x35, 0x39, 0x5a, 0x30,
    0x3d, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x1c, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x44,
    0x65, 0x76, 0x20, 0x50, 0x41, 0x49, 0x20, 0x30, 0x78, 0x46, 0x46, 0x46, 0x31, 0x20, 0x6e, 0x6f, 0x20, 0x50, 0x49, 0x44,
    0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c, 0x04, 0x46, 0x46,
    0x46, 0x31, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x41, 0x9a, 0x93, 0x15, 0xc2, 0x17, 0x3e, 0x0c, 0x8c, 0x87, 0x6d,
    0x03, 0xcc, 0xfc, 0x94, 0x48, 0x52, 0x64, 0x7f, 0x7f, 0xec, 0x5e, 0x50, 0x82, 0xf4, 0x05, 0x99, 0x28, 0xec, 0xa8, 0x94,
    0xc5, 0x94, 0x15, 0x13, 0x09, 0xac, 0x63, 0x1e, 0x4c, 0xb0, 0x33, 0x92, 0xaf, 0x68, 0x4b, 0x0b, 0xaf, 0xb7, 0xe6, 0x5b,
    0x3b, 0x81, 0x62, 0xc2, 0xf5, 0x2b, 0xf9, 0x31, 0xb8, 0xe7, 0x7a, 0xaa, 0x82, 0xa3, 0x66, 0x30, 0x64, 0x30, 0x12, 0x06,
    0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x08, 0x30, 0x06, 0x01, 0x01, 0xff, 0x02, 0x01, 0x00, 0x30, 0x0e, 0x06,
    0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x01, 0x06, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e,
    0x04, 0x16, 0x04, 0x14, 0x63, 0x54, 0x0e, 0x47, 0xf6, 0x4b, 0x1c, 0x38, 0xd1, 0x38, 0x84, 0xa4, 0x62, 0xd1, 0x6c, 0x19,
    0x5d, 0x8f, 0xfb, 0x3c, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x6a, 0xfd, 0x22,
    0x77, 0x1f, 0x51, 0x1f, 0xec, 0xbf, 0x16, 0x41, 0x97, 0x67, 0x10, 0xdc, 0xdc, 0x31, 0xa1, 0x71, 0x7e, 0x30, 0x0a, 0x06,
    0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45, 0x02, 0x21, 0x00, 0xb2, 0xef, 0x27,
    0xf4, 0x9a, 0xe9, 0xb5, 0x0f, 0xb9, 0x1e, 0xea, 0xc9, 0x4c, 0x4d, 0x0b, 0xdb, 0xb8, 0xd7, 0x92, 0x9c, 0x6c, 0xb8, 0x8f,
    0xac, 0xe5, 0x29, 0x36, 0x8d, 0x12, 0x05, 0x4c, 0x0c, 0x02, 0x20, 0x65, 0x5d, 0xc9, 0x2b, 0x86, 0xbd, 0x90, 0x98, 0x82,
    0xa6, 0xc6, 0x21, 0x77, 0xb8, 0x25, 0xd7, 0xd0, 0x5e, 0xdb, 0xe7, 0xc2, 0x2f, 0x9f, 0xea, 0x71, 0x22, 0x0e, 0x7e, 0xa7,
    0x03, 0xf8, 0x91,
};

constexpr uint8_t CertificationDeclaration[] = {
    0x30, 0x82, 0x02, 0x17, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02, 0xa0, 0x82, 0x02, 0x08, 0x30,
    0x82, 0x02, 0x04, 0x02, 0x01, 0x03, 0x31, 0x0d, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
    0x01, 0x30, 0x82, 0x01, 0x70, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01, 0xa0, 0x82, 0x01, 0x61,
    0x04, 0x82, 0x01, 0x5d, 0x15, 0x24, 0x00, 0x01, 0x25, 0x01, 0xf1, 0xff, 0x36, 0x02, 0x05, 0x00, 0x80, 0x05, 0x01, 0x80,
    0x05, 0x02, 0x80, 0x05, 0x03, 0x80, 0x05, 0x04, 0x80, 0x05, 0x05, 0x80, 0x05, 0x06, 0x80, 0x05, 0x07, 0x80, 0x05, 0x08,
    0x80, 0x05, 0x09, 0x80, 0x05, 0x0a, 0x80, 0x05, 0x0b, 0x80, 0x05, 0x0c, 0x80, 0x05, 0x0d, 0x80, 0x05, 0x0e, 0x80, 0x05,
    0x0f, 0x80, 0x05, 0x10, 0x80, 0x05, 0x11, 0x80, 0x05, 0x12, 0x80, 0x05, 0x13, 0x80, 0x05, 0x14, 0x80, 0x05, 0x15, 0x80,
    0x05, 0x16, 0x80, 0x05, 0x17, 0x80, 0x05, 0x18, 0x80, 0x05, 0x19, 0x80, 0x05, 0x1a, 0x80, 0x05, 0x1b, 0x80, 0x05, 0x1c,
    0x80, 0x05, 0x1d, 0x80, 0x05, 0x1e, 0x80, 0x05, 0x1f, 0x80, 0x05, 0x20, 0x80, 0x05, 0x21, 0x80, 0x05, 0x22, 0x80, 0x05,
    0x23, 0x80, 0x05, 0x24, 0x80, 0x05, 0x25, 0x80, 0x05, 0x26, 0x80, 0x05, 0x27, 0x80, 0x05, 0x28, 0x80, 0x05, 0x29, 0x80,
    0x05, 0x2a, 0x80, 0x05, 0x2b, 0x80, 0x05, 0x2c, 0x80, 0x05, 0x2d, 0x80, 0x05, 0x2e, 0x80, 0x05, 0x2f, 0x80, 0x05, 0x30,
    0x80, 0x05, 0x31, 0x80, 0x05, 0x32, 0x80, 0x05, 0x33, 0x80, 0x05, 0x34, 0x80, 0x05, 0x35, 0x80, 0x05, 0x36, 0x80, 0x05,
    0x37, 0x80, 0x05, 0x38, 0x80, 0x05, 0x39, 0x80, 0x05, 0x3a, 0x80, 0x05, 0x3b, 0x80, 0x05, 0x3c, 0x80, 0x05, 0x3d, 0x80,
    0x05, 0x3e, 0x80, 0x05, 0x3f, 0x80, 0x05, 0x40, 0x80, 0x05, 0x41, 0x80, 0x05, 0x42, 0x80, 0x05, 0x43, 0x80, 0x05, 0x44,
    0x80, 0x05, 0x45, 0x80, 0x05, 0x46, 0x80, 0x05, 0x47, 0x80, 0x05, 0x48, 0x80, 0x05, 0x49, 0x80, 0x05, 0x4a, 0x80, 0x05,
    0x4b, 0x80, 0x05, 0x4c, 0x80, 0x05, 0x4d, 0x80, 0x05, 0x4e, 0x80, 0x05, 0x4f, 0x80, 0x05, 0x50, 0x80, 0x05, 0x51, 0x80,
    0x05, 0x52, 0x80, 0x05, 0x53, 0x80, 0x05, 0x54, 0x80, 0x05, 0x55, 0x80, 0x05, 0x56, 0x80, 0x05, 0x57, 0x80, 0x05, 0x58,
    0x80, 0x05, 0x59, 0x80, 0x05, 0x5a, 0x80, 0x05, 0x5b, 0x80, 0x05, 0x5c, 0x80, 0x05, 0x5d, 0x80, 0x05, 0x5e, 0x80, 0x05,
    0x5f, 0x80, 0x05, 0x60, 0x80, 0x05, 0x61, 0x80, 0x05, 0x62, 0x80, 0x05, 0x63, 0x80, 0x18, 0x24, 0x03, 0x16, 0x2c, 0x04,
    0x13, 0x43, 0x53, 0x41, 0x30, 0x30, 0x30, 0x30, 0x30, 0x53, 0x57, 0x43, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2d, 0x30, 0x30,
    0x24, 0x05, 0x00, 0x24, 0x06, 0x00, 0x24, 0x07, 0x01, 0x24, 0x08, 0x00, 0x18, 0x31, 0x7c, 0x30, 0x7a, 0x02, 0x01, 0x03,
    0x80, 0x14, 0xfe, 0x34, 0x3f, 0x95, 0x99, 0x47, 0x76, 0x3b, 0x61, 0xee, 0x45, 0x39, 0x13, 0x13, 0x38, 0x49, 0x4f, 0xe6,
    0x7d, 0x8e, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x30, 0x0a, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x04, 0x46, 0x30, 0x44, 0x02, 0x20, 0x4a, 0x12, 0xf8, 0xd4, 0x2f, 0x90, 0x23,
    0x5c, 0x05, 0xa7, 0x71, 0x21, 0xcb, 0xeb, 0xae, 0x15, 0xd5, 0x90, 0x14, 0x65, 0x58, 0xe9, 0xc9, 0xb4, 0x7a, 0x1a, 0x38,
    0xf7, 0xa3, 0x6a, 0x7d, 0xc5, 0x02, 0x20, 0x20, 0xa4, 0x74, 0x28, 0x97, 0xc3, 0x0a, 0xed, 0xa0, 0xa5, 0x6b, 0x36, 0xe1,
    0x4e, 0xbb, 0xc8, 0x5b, 0xbd, 0xb7, 0x44, 0x93, 0xf9, 0x93, 0x58, 0x1e, 0xb0, 0x44, 0x4e, 0xd6, 0xca, 0x94, 0x0b,
};

// SHA-1 of the public key, the key identifier of the certificate (mbedTLS of the library is built without SHA-1)
void Sha1(const uint8_t * data, size_t length, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t block[64];
    const uint64_t bits = static_cast<uint64_t>(length) * 8;
    size_t total = ((length + 8) / 64 + 1) * 64;

    for (size_t offset = 0; offset < total; offset += 64)
    {
        for (size_t i = 0; i < 64; ++i)
        {
            const size_t index = offset + i;
            if (index < length)
                block[i] = data[index];
            else if (index == length)
                block[i] = 0x80;
            else if (index >= total - 8)
                block[i] = static_cast<uint8_t>(bits >> (8 * (total - 1 - index)));
            else
                block[i] = 0;
        }

        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) |
                uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
        {
            const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i]             = (v << 1) | (v >> 31);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)
            {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e                = d;
            d                = c;
            c                = (b << 30) | (b >> 2);
            b                = a;
            a                = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    for (int i = 0; i < 5; ++i)
    {
        out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

constexpr uint32_t Magic       = 0x45494d31; // "EMI1"
constexpr uint32_t Iterations  = 1000;
constexpr size_t SaltLength    = 32;
constexpr size_t KeypairLength = Crypto::kP256_PublicKey_Length + Crypto::kP256_PrivateKey_Length;

struct Stored
{
    uint32_t magic;
    uint32_t passcode;
    uint16_t discriminator;
    uint16_t reserved;
    uint32_t iterations;
    uint8_t salt[SaltLength];
    uint8_t verifier[Crypto::kSpake2p_VerifierSerialized_Length];
    uint8_t serial[SerialLength];
    uint8_t keypair[KeypairLength];
};

Stored sStored;
bool sActive = false;

bool Load()
{
    uint32_t length = 0;
    return (bk_read_data(Namespace, KeyIdentity, reinterpret_cast<char *>(&sStored), sizeof(sStored), &length) == 0) &&
        (length == sizeof(sStored)) && (sStored.magic == Magic);
}

// DAC: the development template with this board's serial, key and key identifier, signed with the development PAI key
CHIP_ERROR MakeCertificate(Crypto::P256Keypair & key)
{
    // static: the Matter task has a 6 KiB stack, the crypto below needs most of it
    static uint8_t tbs[sizeof(DacTbs)];
    memcpy(tbs, DacTbs, sizeof(tbs));
    memcpy(&tbs[SerialOffset], sStored.serial, SerialLength);

    const auto & pub = key.Pubkey();
    memcpy(&tbs[PublicKeyOffset], pub.ConstBytes(), Crypto::kP256_PublicKey_Length);

    uint8_t keyId[20];
    Sha1(pub.ConstBytes(), Crypto::kP256_PublicKey_Length, keyId);
    memcpy(&tbs[KeyIdOffset], keyId, sizeof(keyId));

    Crypto::P256Keypair pai;
    Crypto::P256SerializedKeypair paiSerialized;
    memcpy(paiSerialized.Bytes(), PaiPublicKey, sizeof(PaiPublicKey));
    memcpy(paiSerialized.Bytes() + sizeof(PaiPublicKey), PaiPrivateKey, sizeof(PaiPrivateKey));
    paiSerialized.SetLength(sizeof(PaiPublicKey) + sizeof(PaiPrivateKey));
    ReturnErrorOnFailure(pai.Deserialize(paiSerialized));

    Crypto::P256ECDSASignature raw;
    ReturnErrorOnFailure(pai.ECDSA_sign_msg(tbs, sizeof(tbs), raw));

    static uint8_t signature[Crypto::kMax_ECDSA_Signature_Length_Der];
    MutableByteSpan signatureSpan(signature);
    ReturnErrorOnFailure(
        Crypto::EcdsaRawSignatureToAsn1(Crypto::kP256_FE_Length, ByteSpan(raw.ConstBytes(), raw.Length()), signatureSpan));

    // SEQUENCE { tbs, signature algorithm, BIT STRING { 0 unused bits, signature } }
    const size_t bits = 1 + signatureSpan.size();
    const size_t body = sizeof(tbs) + sizeof(SignatureAlgorithm) + 2 + bits;

    static uint8_t certificate[4 + sizeof(DacTbs) + sizeof(SignatureAlgorithm) + 3 + sizeof(signature)];
    size_t length         = 0;
    certificate[length++] = 0x30;
    certificate[length++] = 0x82;
    certificate[length++] = static_cast<uint8_t>((body >> 8) & 0xff);
    certificate[length++] = static_cast<uint8_t>(body & 0xff);
    memcpy(&certificate[length], tbs, sizeof(tbs));
    length += sizeof(tbs);
    memcpy(&certificate[length], SignatureAlgorithm, sizeof(SignatureAlgorithm));
    length += sizeof(SignatureAlgorithm);
    certificate[length++] = 0x03;
    certificate[length++] = static_cast<uint8_t>(bits);
    certificate[length++] = 0x00;
    memcpy(&certificate[length], signatureSpan.data(), signatureSpan.size());
    length += signatureSpan.size();

    if (bk_write_data(Namespace, KeyCertificate, reinterpret_cast<const char *>(certificate), static_cast<uint32_t>(length)) != 0)
    {
        return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
    }

    return CHIP_NO_ERROR;
}

// Once per board: codes, salt, verifier, key and certificate
CHIP_ERROR MakeIdentity();

// Key, two signatures and PBKDF2 take seconds on this chip. The Matter task (priority 5) would keep the ESPurna loop,
// which feeds the 10 s hardware watchdog, from running all that time: drop below it until done
CHIP_ERROR Make()
{
    const UBaseType_t priority = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, tskIDLE_PRIORITY + 1);
    const CHIP_ERROR err = MakeIdentity();
    vTaskPrioritySet(nullptr, priority);
    return err;
}

CHIP_ERROR MakeIdentity()
{
    static Stored made;
    made       = Stored{};
    made.magic = Magic;

    do
    {
        ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&made.passcode), sizeof(made.passcode)));
        made.passcode = (made.passcode % 99999998u) + 1u;
    } while (!SetupPayload::IsValidSetupPIN(made.passcode));

    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&made.discriminator), sizeof(made.discriminator)));
    made.discriminator = static_cast<uint16_t>(made.discriminator & 0xfff);

    made.iterations = Iterations;
    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(made.salt, sizeof(made.salt)));

    Crypto::Spake2pVerifier verifier;
    ReturnErrorOnFailure(verifier.Generate(made.iterations, ByteSpan(made.salt), made.passcode));
    MutableByteSpan verifierSpan(made.verifier);
    ReturnErrorOnFailure(verifier.Serialize(verifierSpan));

    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(made.serial, sizeof(made.serial)));
    // positive, and minimal as a DER integer
    made.serial[0] = static_cast<uint8_t>((made.serial[0] & 0x3f) | 0x40);

    Crypto::P256Keypair key;
    ReturnErrorOnFailure(key.Initialize(Crypto::ECPKeyTarget::ECDSA));
    Crypto::P256SerializedKeypair serialized;
    ReturnErrorOnFailure(key.Serialize(serialized));
    VerifyOrReturnError(serialized.Length() == sizeof(made.keypair), CHIP_ERROR_INTERNAL);
    memcpy(made.keypair, serialized.ConstBytes(), sizeof(made.keypair));
    memset(serialized.Bytes(), 0, serialized.Capacity());

    sStored = made;
    memset(&made, 0, sizeof(made));
    ReturnErrorOnFailure(MakeCertificate(key));

    if (bk_write_data(Namespace, KeyIdentity, reinterpret_cast<const char *>(&sStored), sizeof(sStored)) != 0)
    {
        return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
    }

    return CHIP_NO_ERROR;
}

class CommissionableData : public CommissionableDataProvider
{
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t & out) override
    {
        out = sStored.discriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t & out) override
    {
        out = sStored.iterations;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pSalt(MutableByteSpan & out) override { return CopySpanToMutableSpan(ByteSpan(sStored.salt), out); }
    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan & out, size_t & length) override
    {
        length = sizeof(sStored.verifier);
        return CopySpanToMutableSpan(ByteSpan(sStored.verifier), out);
    }
    CHIP_ERROR GetSetupPasscode(uint32_t & out) override
    {
        out = sStored.passcode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};

CommissionableData sCommissionableData;

// VID / PID / names of the certificate, the rest from the provider of the platform
class DeviceInstanceInfo : public DeviceInstanceInfoProvider
{
public:
    void SetBase(DeviceInstanceInfoProvider * base) { mBase = base; }

    CHIP_ERROR GetVendorName(char * buf, size_t size) override { return CopyString("ESPurna", buf, size); }
    CHIP_ERROR GetVendorId(uint16_t & out) override
    {
        out = VendorId;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetProductName(char * buf, size_t size) override { return CopyString("ESPurna plug", buf, size); }
    CHIP_ERROR GetProductId(uint16_t & out) override
    {
        out = ProductId;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetPartNumber(char * buf, size_t size) override { return mBase->GetPartNumber(buf, size); }
    CHIP_ERROR GetProductURL(char * buf, size_t size) override { return mBase->GetProductURL(buf, size); }
    CHIP_ERROR GetProductLabel(char * buf, size_t size) override { return mBase->GetProductLabel(buf, size); }
    CHIP_ERROR GetSerialNumber(char * buf, size_t size) override { return mBase->GetSerialNumber(buf, size); }
    CHIP_ERROR GetManufacturingDate(uint16_t & year, uint8_t & month, uint8_t & day) override
    {
        return mBase->GetManufacturingDate(year, month, day);
    }
    CHIP_ERROR GetHardwareVersion(uint16_t & out) override { return mBase->GetHardwareVersion(out); }
    CHIP_ERROR GetHardwareVersionString(char * buf, size_t size) override { return mBase->GetHardwareVersionString(buf, size); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan & out) override { return mBase->GetRotatingDeviceIdUniqueId(out); }

private:
    static CHIP_ERROR CopyString(const char * value, char * buf, size_t size)
    {
        const size_t length = strlen(value);
        VerifyOrReturnError(length < size, CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(buf, value, length + 1);
        return CHIP_NO_ERROR;
    }

    DeviceInstanceInfoProvider * mBase = nullptr;
};

DeviceInstanceInfo sDeviceInstanceInfo;

// Own DAC and key; development PAI and the certification declaration of the SDK examples
class Attestation : public Credentials::DeviceAttestationCredentialsProvider
{
public:
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan & out) override
    {
        return CopySpanToMutableSpan(ByteSpan(CertificationDeclaration), out);
    }
    CHIP_ERROR GetFirmwareInformation(MutableByteSpan & out) override
    {
        out.reduce_size(0);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan & out) override
    {
        return CopySpanToMutableSpan(ByteSpan(PaiCertificate), out);
    }
    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan & out) override
    {
        uint32_t length = 0;
        if (bk_read_data(Namespace, KeyCertificate, reinterpret_cast<char *>(out.data()), static_cast<uint32_t>(out.size()),
                         &length) != 0)
        {
            return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
        }
        VerifyOrReturnError(length <= out.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
        out.reduce_size(length);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan & message, MutableByteSpan & out) override
    {
        Crypto::P256SerializedKeypair serialized;
        memcpy(serialized.Bytes(), sStored.keypair, sizeof(sStored.keypair));
        serialized.SetLength(sizeof(sStored.keypair));

        Crypto::P256Keypair key;
        CHIP_ERROR err = key.Deserialize(serialized);
        memset(serialized.Bytes(), 0, serialized.Capacity());
        ReturnErrorOnFailure(err);

        Crypto::P256ECDSASignature signature;
        ReturnErrorOnFailure(key.ECDSA_sign_msg(message.data(), message.size(), signature));
        return CopySpanToMutableSpan(ByteSpan(signature.ConstBytes(), signature.Length()), out);
    }
};

Attestation sOwnAttestation;

// Factory identity: VID / PID / names from the factory data, the certification declaration is issued for them; the rest
// from the provider of the platform. The Quectel table holds the values of the build configuration anyway, Tuya's
// Matter firmware (MOES ...) its own vendor, with the DAC of Tuya as the DAC origin of the declaration
class FactoryDeviceInstanceInfo : public DeviceInstanceInfoProvider
{
public:
    void SetBase(DeviceInstanceInfoProvider * base) { mBase = base; }

    CHIP_ERROR GetVendorName(char * buf, size_t size) override
    {
        return (ReadText("vendor-name", buf, size) == CHIP_NO_ERROR) ? CHIP_NO_ERROR : mBase->GetVendorName(buf, size);
    }
    CHIP_ERROR GetVendorId(uint16_t & out) override
    {
        return ReadNumber("vendor-id", out) ? CHIP_NO_ERROR : mBase->GetVendorId(out);
    }
    CHIP_ERROR GetProductName(char * buf, size_t size) override
    {
        return (ReadText("product-name", buf, size) == CHIP_NO_ERROR) ? CHIP_NO_ERROR : mBase->GetProductName(buf, size);
    }
    CHIP_ERROR GetProductId(uint16_t & out) override
    {
        return ReadNumber("product-id", out) ? CHIP_NO_ERROR : mBase->GetProductId(out);
    }
    CHIP_ERROR GetPartNumber(char * buf, size_t size) override { return mBase->GetPartNumber(buf, size); }
    CHIP_ERROR GetProductURL(char * buf, size_t size) override { return mBase->GetProductURL(buf, size); }
    CHIP_ERROR GetProductLabel(char * buf, size_t size) override { return mBase->GetProductLabel(buf, size); }
    CHIP_ERROR GetSerialNumber(char * buf, size_t size) override { return mBase->GetSerialNumber(buf, size); }
    CHIP_ERROR GetManufacturingDate(uint16_t & year, uint8_t & month, uint8_t & day) override
    {
        return mBase->GetManufacturingDate(year, month, day);
    }
    CHIP_ERROR GetHardwareVersion(uint16_t & out) override { return mBase->GetHardwareVersion(out); }
    CHIP_ERROR GetHardwareVersionString(char * buf, size_t size) override { return mBase->GetHardwareVersionString(buf, size); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan & out) override { return mBase->GetRotatingDeviceIdUniqueId(out); }

private:
    static CHIP_ERROR ReadText(const char * name, char * buf, size_t size)
    {
        VerifyOrReturnError(size > 1, CHIP_ERROR_BUFFER_TOO_SMALL);
        MutableByteSpan span(reinterpret_cast<uint8_t *>(buf), size - 1);
        ReturnErrorOnFailure(ReadFactory(name, span));
        VerifyOrReturnError(span.size() > 0, CHIP_ERROR_NOT_FOUND);
        buf[span.size()] = '\0';
        return CHIP_NO_ERROR;
    }

    // 2 bytes, little endian
    static bool ReadNumber(const char * name, uint16_t & out)
    {
        uint8_t value[2];
        MutableByteSpan span(value);
        if ((ReadFactory(name, span) != CHIP_NO_ERROR) || (span.size() != sizeof(value)))
        {
            return false;
        }
        out = static_cast<uint16_t>(value[0] | (value[1] << 8));
        return true;
    }

    DeviceInstanceInfoProvider * mBase = nullptr;
};

FactoryDeviceInstanceInfo sFactoryDeviceInstanceInfo;

// Factory DAC there: the factory identity is used, VID / PID / names of the factory data. None: load the own identity
// (or make it) and use it instead of the platform providers
void Install()
{
    uint8_t probe[1];
    MutableByteSpan probeSpan(probe);
    const CHIP_ERROR factory = ReadFactory("dac-cert", probeSpan);
    if ((factory == CHIP_NO_ERROR) || (factory == CHIP_ERROR_BUFFER_TOO_SMALL))
    {
        sFactoryDeviceInstanceInfo.SetBase(GetDeviceInstanceInfoProvider());
        SetDeviceInstanceInfoProvider(&sFactoryDeviceInstanceInfo);
        uint16_t vendor  = 0;
        uint16_t product = 0;
        sFactoryDeviceInstanceInfo.GetVendorId(vendor);
        sFactoryDeviceInstanceInfo.GetProductId(product);
        ChipLogProgress(AppServer, "Factory identity 0x%04X/0x%04X", vendor, product);
        return;
    }

    if (!Load())
    {
        ChipLogProgress(AppServer, "No factory data, making the own identity of this board");
        const CHIP_ERROR err = Make();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(AppServer, "Own identity failed: %" CHIP_ERROR_FORMAT, err.Format());
            return;
        }
    }

    sDeviceInstanceInfo.SetBase(GetDeviceInstanceInfoProvider());
    SetDeviceInstanceInfoProvider(&sDeviceInstanceInfo);
    SetCommissionableDataProvider(&sCommissionableData);
    sActive = true;
    ChipLogProgress(AppServer, "Own identity, development DAC 0x%04X/0x%04X", VendorId, ProductId);
}

} // namespace own

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
    own::Install();
    BekenAppServer::Init(&sCommissioningDelegate);
    EnablePlugs();
    PlatformMgr().AddEventHandler(OnDeviceEvent, 0);
#if CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID == 0xFFF1
    // test build: SDK example DAC / PAI / CD of VID 0xFFF1, commissioners treat it as an uncertified device
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());
#else
    if (own::sActive)
    {
        Credentials::SetDeviceAttestationCredentialsProvider(&own::sOwnAttestation);
    }
    else
    {
        Credentials::SetDeviceAttestationCredentialsProvider(&sAttestation);
    }
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
