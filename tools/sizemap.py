"""Per-component text/data/bss from a GNU ld map file (sizing link of libMatterApp.a).
usage: py -3 -I sizemap.py link.map"""
import re, sys, collections

COMPONENTS = [
    # complete_static_lib flattens objects to "<gn target>.<file>.o": classify by target name (and file for the app glue)
    ('platform BK7231N (port, copied Beken code)', ('(BK7231N.', '(bk7231n_port.')),
    ('mbedTLS 2.28', ('(libmbedtls.',)),
    ('crypto PAL (mbedTLS) + ASN1', ('(libChipCrypto.', '(cryptopal_mbedtls.', '(libASN1.')),
    ('credentials (certs, DAC, fabric table)', ('(libCredentials.',)),
    ('clusters + data model (zap/codegen, ember)', ('(plug-data-model', '(libClusterObjects.', '(libCHIPDataModel.', 'network-commissioning.',
        '-server.', '(data-model-provider.', '(attribute-access.', '(server-cluster.', '(global-attributes.', '(wifi-response-encoding.',
        '(software-fault-listener.', '(attribute-persistence.', '(persistence.', '(storage-wrapper.', '(path-expansion.',
        '(encode-decode.', '(types.', '(registry.', '(callbacks.')),
    ('interaction model + app server + ACL', ('(interaction-model.', '(MessageDef.', '(libCHIPAppServer.', '(libaccess.', '(command-handler',
        '(default_policy.', '(status-response.', '(libInteractionModel.', '(libBdx.')),
    ('secure channel (PASE/CASE) / transport / messaging', ('(libSecureChannel', '(libTransportLayer.', '(libMessagingLayer.',
        '(libRawTransport.', '(libChipProtocols.')),
    ('mDNS minimal + address resolve', ('(dnssd.', '(minimal_mdns.', '(address_resolve.', '(responders.', '(records.', '(core.',
        '(address_policy.', '(naming.')),
    ('system / inet (lwIP glue)', ('(libSystemLayer.', '(libInetLayer.')),
    ('device layer (generic platform)', ('(libDeviceLayer.',)),
    ('lib core/support/TLV/setup payload/logging', ('(libSupportLayer.', '(libChipCore.', '(libSetupPayload.', '(error.',
        '(text_only_logging.', '(memory.')),
    ('app glue (matter_app, OTA helper, device mgr)', ('(libMatterApp.',)),
    ('libstdc++/libsupc++', ('libstdc++', 'libsupc++')),
    ('libc/libm (newlib-nano)', ('libc_nano', 'libc.a', 'libm.a', 'libg_nano', 'libnosys')),
    ('libgcc', ('libgcc',)),
]


def classify(path):
    p = path.replace('\\', '/')
    for name, keys in COMPONENTS:
        if any(k in p for k in keys):
            return name
    return 'other: ' + p.split('/')[-1][:60]


def kind(sec):
    if sec.startswith(('.text', '.rodata', '.glue', '.vfp11', '.v4_bx', '.ARM.extab', '.ARM.exidx', '.init_array', '.fini_array', '.preinit', '.init', '.fini', '.ctors', '.dtors')):
        return 'text'
    if sec.startswith('.data'):
        return 'data'
    if sec.startswith(('.bss', 'COMMON', '.sbss')):
        return 'bss'
    return None


def main(mapfile):
    tot = collections.defaultdict(lambda: collections.Counter())
    lines = open(mapfile, encoding='utf-8', errors='replace').read().split('\n')
    start = next(i for i, l in enumerate(lines) if l.startswith('Linker script and memory map'))
    pending = None
    rx_full = re.compile(r'^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$')
    rx_cont = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$')
    for l in lines[start:]:
        if l.startswith('/DISCARD/'):
            break
        m = rx_full.match(l)
        if m:
            sec, addr, size, path = m.groups()
            pending = None
        else:
            if pending is not None:
                m2 = rx_cont.match(l)
                if m2:
                    sec = pending
                    addr, size, path = m2.groups()
                    pending = None
                else:
                    pending = None
                    continue
            else:
                m3 = re.match(r'^ (\.\S+|COMMON)\s*$', l)
                if m3:
                    pending = m3.group(1)
                continue
        k = kind(sec)
        size = int(size, 16)
        if k and size and int(addr, 16):
            tot[classify(path)][k] += size
    rows = sorted(tot.items(), key=lambda kv: -(kv[1]['text'] + kv[1]['data']))
    S = collections.Counter()
    print('%-46s %9s %7s %7s' % ('component', 'text+ro', 'data', 'bss'))
    for name, c in rows:
        print('%-46s %9d %7d %7d' % (name, c['text'], c['data'], c['bss']))
        S.update(c)
    print('%-46s %9d %7d %7d' % ('TOTAL', S['text'], S['data'], S['bss']))


main(sys.argv[1])
