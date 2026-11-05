import json, shlex, sys
d = json.load(open(sys.argv[1]))
pat = sys.argv[2]
for e in d:
    f = e['file'].replace(chr(92), '/')
    if pat in f:
        a = shlex.split(e['command'].replace(chr(92), '/'), posix=True)
        print('FILE', f)
        for x in a[1:]:
            if x.startswith('-I') and ('ble_5_x_rw' in x or '/ip/' in x): continue
            print(x)
        break
