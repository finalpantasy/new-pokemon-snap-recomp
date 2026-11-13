"""Read-only: dump PS reflection (samplers, uniforms with block/offset) of nushdb files.
Usage: python -I scripts/hd/shader_refl.py <ids or files>   (id = CharacterShader number)"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import nushdb_parse as n
R = 'extracted/drp-v262144/nx/Pack/Resident/master/Resident.drp/CharactersMaterials_CharacterShader_%s.nushdb'
for a in sys.argv[1:]:
    p = a if a.endswith('.nushdb') else R % a
    for pr in n.parse_file(p):
        bl = pr['blob']
        if bl['sph_stage'] != 5 and 'PS' not in pr['name']:
            continue
        rf = bl['refl']
        print('==', pr['name'], 'insn', bl['insn'], 'kills', bl.get('kills'))
        ubos = {i: u for i, u in enumerate(rf['ubos'])}
        print(' samplers', [(s['name'], s['binding_per_stage'][bl['nvn_stage']]) for s in rf['samplers']])
        for u in rf['uniforms']:
            bn = rf['ubos'][u['block']]['name'] if 0 <= u['block'] < len(rf['ubos']) else u['block']
            print('  %-28s %-45s off=0x%x type=%d cnt=%d' % (bn, u['name'], u['offset'], u['type'], u['count']))
