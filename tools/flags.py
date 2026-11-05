import json, shlex, sys, collections
d = json.load(open(sys.argv[1]))
def args(e):
    return shlex.split(e['command'].replace(chr(92), '/'), posix=True)
for e in d:
    if e['file'].endswith('relay.cpp'):
        a = args(e)
        inc = [x for x in a if x.startswith('-I')]
        print(len(inc), 'includes')
        print('\n'.join(x for x in inc if 'espurna' not in x and 'lib_' not in x and 'libdeps' not in x))
        break
c = collections.Counter()
for e in d:
    f = e['file'].replace(chr(92), '/')
    c['/'.join(f.split('/')[:7]) if f.startswith('C:') else f.split('/')[0]] += 1
for k, v in c.most_common(30): print(v, k)
