r"""Writes the local 'surround-bus' mod pack: Init.bnk with the mixer bus (id 508372457, parent of bgm/se/voice) channel
config changed from stereo (0x3102) to 0 = same as parent, so Wwise mixes 5.1 when the host reports 6 channels.
Read-only on extracted data; the result goes to %APPDATA%\SnapRecomp\mods\surround-bus (never commit it).
Run: python -I scripts\audio_make_surround_pack.py"""
import json, os, struct
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'extracted', 'drp-v262144', 'nx', 'Sound', 'master', 'Init.drp', 'Init.bnk')
d = bytearray(open(src, 'rb').read())
k = d.find(struct.pack('<II', 508372457, 3803692087))   # bus id, parent = Master Audio Bus
assert k > 0, 'mixer bus not found'
off = k + 44                                            # id, parent, 6 props, 5 bytes, then the u32 channel config
assert d[off:off + 4] == struct.pack('<I', 0x3102), 'unexpected channel config'
d[off:off + 4] = b'\0\0\0\0'
root = os.path.join(os.environ['APPDATA'], 'SnapRecomp', 'mods', 'surround-bus')
out = os.path.join(root, 'nx', 'Sound', 'master', 'Init.drp')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'Init.bnk'), 'wb').write(d)
json.dump({"name": "Surround master bus", "version": "1.0.0", "author": "snaprecomp",
           "description": "Init.bnk mixer bus channel config stereo -> same as parent (5.1 capable)."}, open(os.path.join(root, 'pack.json'), 'w'))
print('wrote', out)
