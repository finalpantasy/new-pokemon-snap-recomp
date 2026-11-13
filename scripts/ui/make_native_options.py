"""Build the local "native-options" mod pack: the PC settings as extra tabs of the game's own Settings screen (option.lmb).

The game's Settings screen is data driven (scripts/ui/lmb.py has the format): five tabs (states Game, Camera, Button, Sound,
Others), each with up to six row slots (selector_01..06_mc). A slot shows a two-choice row (lb_common), a pulldown row
(lb_pulldown, its popup pulldown_0N_mc lists up to 10 items) or a 0..10 dot slider (lb_number). This script adds ten PC
tabs in two sets of five, reached with R past the game's last tab ("Other"):

  Quality, Textures, Effects, Output, View  |  Display, Controller, Mouse, Audio, Photos

Every PC tab is a new state cloned from "Others" (same tab bar, arrows, Restore Default button, sounds and help bar) whose
rows are generated from TABS below; every pulldown row gets its own popup state cloned from the game's "NaviFrequency".
Nothing original is rewritten except the five game tab states, which also set the tab texts again (the PC tabs rename them),
The game's Lumen player does every state switch itself (element f2 = target state with f6 = 1, f6 = 4 back to the
previous state; L/R open the state the page's Prev/Next entries name, which the host points at the PC tabs).

The host side (host/mods/nativeopt.cpp) fills in each PC page as it opens (current values) and stores changes in the
same places the F1 overlay uses (runtime/hle/settings.cpp keys, the controls mapping, the audio layer). It reads the row
table from rows.txt, which this script writes next to the pack (our own text, no game data).

Input: the user's extracted option.lmb. Output: <out>/native-options/{pack.json, rows.txt,
common/UI/master/Scenes/option.drp/option.lmb}. The output is game-derived: it stays local (mods folder or build/), never in
git. host/play.ps1 runs this script when the pack is missing or older than this script.

  python -I scripts/ui/make_native_options.py [--src <option.lmb>] [--out <mods root>]
"""
import argparse, hashlib, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lmb as L

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'extracted', 'drp-v262144', 'common', 'UI', 'master', 'Scenes', 'option.drp', 'option.lmb')
VERSION = '0.2.0'

R = ' Applies after restart.'
ONOFF = (('default', 'On'), ('off', 'Off'))      # render pass rows: "default" = the game's own pass
TOGGLE = (('1', 'On'), ('0', 'Off'))             # settings toggles stored as 1/0


def row(label, widget, store, key, default, choices, help):
    """widget: 'c' two choices, 'p' pulldown (3 to 10 items), 'n' 0..10 dot slider. store: 'opt' (a settings key through the
    F1 option table), 'map' (a controls-mapping field), 'vol' (an audio volume). choices: ((stored value, label), ...)."""
    assert widget != 'c' or len(choices) == 2, label
    assert widget != 'p' or 2 <= len(choices) <= 10, label
    assert len(help) <= 112, (label, len(help))
    return dict(label=label, widget=widget, store=store, key=key, default=default, choices=choices, help=help)


def nums(*vals, fmt='{}', suffix=''):
    return tuple((v, fmt.format(v) + suffix) for v in vals)


TABS = [
    ('Quality', 'Picture quality for this PC version.', [
        row('Quality preset', 'p', 'opt', 'gfx_preset', 'normal',
            (('verylow', 'Very Low'), ('low', 'Low'), ('normal', 'Normal'), ('high', 'High'), ('ultra', 'Ultra'),
             ('maximum', 'Maximum'), ('custom', 'Custom')),
            'Sets this tab and the next together. Very Low suits weak PCs, Maximum a very fast GPU.' + R),
        row('Render scale', 'p', 'opt', 'gfx_render_scale', '1.00',
            (('0.50', '0.5x'), ('0.75', '0.75x'), ('1.00', '1x (1080p)'), ('1.25', '1.25x'), ('1.50', '1.5x'),
             ('2.00', '2x (4K)'), ('2.50', '2.5x'), ('3.00', '3x'), ('4.00', '4x (8K)')),
            'How sharp the 3D picture is drawn. Higher is crisper and slower.' + R),
        row('Anti-aliasing', 'p', 'opt', 'gfx_aa', 'default',
            (('default', 'FXAA (original)'), ('smaa', 'SMAA'), ('ssaa2', '2x SSAA + FXAA'), ('ssaa2,smaa', '2x SSAA + SMAA'),
             ('off', 'Off'), ('none', 'None')),
            'Smooths jagged edges. FXAA is the original, SMAA keeps more detail, SSAA costs the most.' + R),
        row('Sharpening', 'n', 'opt', 'gfx_sharpen', '0.00', (),
            'A gentle sharpening pass after anti-aliasing. 0 is off.' + R),
        row('Upscaler', 'p', 'opt', 'gfx_upscaler', 'off',
            (('off', 'Off'), ('dlaa', 'DLAA'), ('dlss', 'DLSS'), ('fsr', 'FSR'), ('xess', 'XeSS')),
            'Rebuilds a sharp picture from fewer pixels. DLSS and DLAA need an NVIDIA RTX card.' + R),
        row('Upscaler quality', 'p', 'opt', 'gfx_upscale_quality', 'native',
            (('native', 'Native'), ('quality', 'Quality'), ('balanced', 'Balanced'), ('performance', 'Performance')),
            'How many pixels are drawn before the upscaler enlarges them. Lower modes run faster.' + R),
    ]),
    ('Textures', 'Texture, shadow and shading detail.', [
        row('Anisotropic filtering', 'p', 'opt', 'gfx_aniso', '16',
            (('0', 'Off'), ('2', '2x'), ('4', '4x'), ('8', '8x'), ('16', '16x')),
            'Keeps ground and walls sharp at shallow angles. 16x costs almost nothing on a PC.' + R),
        row('Texture sharpness', 'p', 'opt', 'gfx_lod_bias', 'auto',
            (('auto', 'Automatic'), ('-1', 'Sharper'), ('-0.5', 'A little sharper'), ('0', 'Original'), ('0.5', 'Softer'),
             ('1', 'Much softer')),
            'Which texture detail level is used. Sharper settings can shimmer.' + R),
        row('Shadow resolution', 'p', 'opt', 'gfx_shadow_res', 'default',
            (('512', '512'), ('default', '1024 (original)'), ('2048', '2048'), ('4096', '4096'), ('8192', '8192')),
            'Size of the shadow map. Higher gives crisper shadow edges and uses more video memory.' + R),
        row('Ambient occlusion', 'p', 'opt', 'ssao_quality', 'default',
            (('default', 'Original'), ('high', 'High'), ('ultra', 'Ultra')),
            'Soft contact shadows. Only the photo-select screen draws them.' + R),
        row('Occlusion samples', 'p', 'opt', 'gfx_ssao_samples', 'default',
            (('default', 'Original'),) + nums('8', '16', '64', '128', '256', '512'),
            'Samples per pixel for ambient occlusion. Overrides the row above.' + R),
        row('Horizon occlusion', 'p', 'opt', 'gfx_ssao_hbao', 'default',
            (('default', 'Original'), ('12,48', '12 x 48'), ('16,64', '16 x 64'), ('24,96', '24 x 96'), ('32,128', '32 x 128')),
            'Detail of the horizon-based occlusion pass. More is smoother and slower.' + R),
    ]),
    ('Effects', 'Screen effects and color.', [
        row('Bloom', 'c', 'opt', 'gfx_pass_bloom', 'default', ONOFF,
            'The soft glow around bright areas. Off keeps the picture sharper.' + R),
        row('Depth of field', 'c', 'opt', 'gfx_pass_dof', 'default', ONOFF,
            'The blur on out-of-focus distance in some scenes. Off keeps everything sharp.' + R),
        row('Film grain', 'c', 'opt', 'gfx_pass_grain', 'default', ONOFF,
            'The film-grain noise in some scenes. Off removes it.' + R),
        row('Chromatic aberration', 'c', 'opt', 'gfx_pass_aberration', 'default', ONOFF,
            'The red and blue fringing at the screen edges in some scenes. Off removes it.' + R),
        row('Color look', 'p', 'opt', 'look', 'neutral',
            (('neutral', 'Neutral'), ('vivid', 'Vivid'), ('warm', 'Warm'), ('cool', 'Cool')),
            'A color grade on the finished picture. Neutral leaves the game as shipped.' + R),
        row('Look strength', 'p', 'opt', 'look_intensity', '1.00',
            (('0.00', '0'), ('0.25', '0.25'), ('0.50', '0.5'), ('0.75', '0.75'), ('1.00', '1'), ('1.25', '1.25'),
             ('1.50', '1.5')),
            'How strongly the color look is applied. No effect while the look is Neutral.' + R),
    ]),
    ('Output', 'HDR output and frame rate.', [
        row('HDR output', 'c', 'opt', 'gfx_hdr', '0', TOGGLE,
            'Sends an HDR picture to an HDR monitor. Turn on HDR in Windows first.' + R),
        row('HDR peak brightness', 'p', 'opt', 'gfx_hdr_peak', '1000',
            nums('400', '600', '800', '1000', '1500', '2000', '3000', '4000', suffix=' nits'),
            'The brightest highlight to send. Use your monitor\'s rated peak.' + R),
        row('HDR paper white', 'p', 'opt', 'gfx_hdr_paper', '200',
            nums('80', '100', '150', '200', '250', '300', '400', suffix=' nits'),
            'How bright ordinary white is. Lower it if the picture feels too bright in a dark room.' + R),
        row('Frame rate limit', 'p', 'opt', 'frame_cap', '0',
            (('0', 'Off'),) + nums('30', '45', '60', '90', '120', '144', suffix=' FPS'),
            'Stops the game from drawing faster than this. Off keeps the original 60 FPS pacing.'),
    ]),
    ('View', 'How far detail reaches. These rows may change photo scores.', [
        row('Detail distance', 'p', 'opt', 'gfx_lod_dist', '1',
            (('1', '1x (original)'),) + nums('1.5', '2', '3', '4', '6', '8', suffix='x'),
            'Pushes out where models switch to simpler versions. Costs some speed.' + R),
        row('Always full detail', 'c', 'opt', 'gfx_lod_force0', '0', TOGGLE,
            'Every model uses its most detailed version at any distance. Costs speed.' + R),
        row('Draw distance', 'p', 'opt', 'gfx_draw_dist', '1',
            (('1', '1x (original)'),) + nums('1.5', '2', '3', '4', '6', '8', suffix='x'),
            'Pushes out where scenery and Pokémon stop being drawn.' + R),
        row('Full-rate animation', 'c', 'opt', 'gfx_full_rate_anim', '0', TOGGLE,
            'Far-away Pokémon animate every frame instead of every 2nd or 4th.' + R),
    ]),
    ('Display', 'Window and start-up settings.', [
        row('Window mode', 'c', 'opt', 'win_mode', '0', (('0', 'Windowed'), ('1', 'Borderless')),
            'Windowed or borderless fullscreen. F11 and Alt+Enter switch as well.'),
        row('Monitor', 'p', 'opt', 'win_monitor', '-1',
            (('-1', 'Where the window is'), ('0', 'Monitor 1'), ('1', 'Monitor 2'), ('2', 'Monitor 3'), ('3', 'Monitor 4')),
            'Which screen the borderless window covers.'),
        row('Windowed size', 'p', 'opt', 'win_size', '',
            (('', 'Remembered'), ('1280x720', '1280 x 720'), ('1600x900', '1600 x 900'), ('1920x1080', '1920 x 1080'),
             ('2560x1440', '2560 x 1440'), ('3840x2160', '3840 x 2160')),
            'The picture size when windowed. Remembered keeps the size you last dragged it to.' + R),
        row('Menu text sharpness', 'p', 'opt', 'gfx_ui_font', 'default',
            (('default', 'Automatic'), ('1', 'Original'), ('2', '2x'), ('3', '3x'), ('4', '4x')),
            'Draws menu letters sharper on large screens, at the same size.' + R),
        row('Pause when unfocused', 'c', 'opt', 'win_pause', '1', TOGGLE,
            'Freezes the game while you are in another window.'),
        row('Fast boot', 'c', 'opt', 'fast_boot', '0', TOGGLE,
            'Skips the frame limiter while loading, so the title screen comes up sooner.' + R),
    ]),
    ('Controller', 'Controller layout and sticks. Buttons are bound in the F1 menu.', [
        row('Face button layout', 'c', 'map', 'face_layout', '0', (('0', 'By position'), ('1', 'By label')),
            'By position puts A on the right face button like a Switch. By label matches the letters.'),
        row('Swap sticks', 'c', 'map', 'swap_sticks', '0', TOGGLE,
            'The left stick aims the camera and the right stick moves.'),
        row('Invert left stick', 'c', 'map', 'left_invert_y', '0', TOGGLE,
            'Pushing the left stick up acts as down.'),
        row('Invert right stick', 'c', 'map', 'right_invert_y', '0', TOGGLE,
            'Pushing the right stick up acts as down.'),
        row('ZL button', 'c', 'map', 'toggle_zl', '0', (('0', 'Hold'), ('1', 'Toggle')),
            'Toggle: one press holds ZL down until the next press.'),
        row('ZR button', 'c', 'map', 'toggle_zr', '0', (('0', 'Hold'), ('1', 'Toggle')),
            'Toggle: one press holds ZR, the camera shutter, down until the next press.'),
    ]),
    ('Mouse', 'Mouse and cursor.', [
        row('Mouse look', 'c', 'map', 'mouse_look', '0', TOGGLE,
            'Moving the mouse aims the camera. The cursor is hidden while this is on.'),
        row('Mouse sensitivity', 'p', 'map', 'mouse_sensitivity', '1.00',
            nums('0.25', '0.50', '0.75', '1.00', '1.50', '2.00', '3.00', '5.00'),
            'How far the camera turns for a given mouse speed.'),
        row('Invert horizontal', 'c', 'map', 'mouse_invert_x', '0', TOGGLE,
            'Moving the mouse right turns the camera left.'),
        row('Invert vertical', 'c', 'map', 'mouse_invert_y', '0', TOGGLE,
            'Moving the mouse up aims down.'),
        row('Click to touch', 'c', 'map', 'mouse_touch', '1', TOGGLE,
            'Clicking in the window taps the touch screen. Ignored while mouse look is on.'),
        row('Lock cursor', 'c', 'map', 'lock_cursor', '1', TOGGLE,
            'Keeps the cursor inside the game window while you play.'),
    ]),
    ('Audio', 'Overall volume and sound output. Music, effects and voices are on the Sound tab.', [
        row('Master volume', 'n', 'vol', 'master', '10', (),
            'Overall loudness of the game.'),
        row('Output mode', 'p', 'opt', 'audio_output', 'stereo',
            (('stereo', 'Stereo'), ('speakers', 'Speakers (5.1/7.1)'), ('spatial', 'Spatial (headphones)')),
            'Stereo is the original mix. Speakers sends surround, Spatial uses Windows Spatial Sound.' + R),
        row('Mute in background', 'c', 'opt', 'win_mute', '1', TOGGLE,
            'Silences the game while its window is not in front.'),
    ]),
    ('Photos', 'Photo files and expeditions.', [
        row('Save PNG copies', 'c', 'opt', 'photo_export', '1', TOGGLE,
            'Each photo you keep is also saved as a full-quality PNG file.' + R),
        row('NEO-ONE speed', 'p', 'opt', 'neo_speed', '1',
            (('0.5', '0.5x'), ('0.75', '0.75x'), ('1', '1x (original)'), ('1.25', '1.25x'), ('1.5', '1.5x'), ('2', '2x'),
             ('2.5', '2.5x'), ('3', '3x')),
            'How fast the NEO-ONE rides through a course. Photos and scoring are not changed.' + R),
        row('Photos per expedition', 'p', 'opt', 'film', 'default',
            (('default', '72 (original)'),) + nums('100', '150', '200', '300', '500'),
            'How many photos you can take before an expedition ends.' + R),
        row('Skip dialogue', 'c', 'opt', 'skip_cutscenes', '0', TOGGLE,
            'Presses A for you when a dialogue box is waiting. Choices are not answered for you.' + R),
    ]),
]
RESET_NOTE = ' Restore Default Settings resets only this tab.'
GAME_TABS = ('Game', 'Camera', 'Button', 'Sound', 'Others')    # state = tab element names, in tab-bar order
# message ids of the game's tab names (Init sets them once through {Tab1Text}..; only Init has those variables)
GAME_TAB_TEXT = ('Option_TabGame_Tx', 'Option_TabCamera_Tx', 'Option_TabButton_Tx', 'Option_TabSound_Tx', 'Option_Extra_Tx')
SEL = 'selector_parent_mc/selector_{s:02d}_mc'


def state_name(t):
    return 'PcTab%d' % t


def pull_name(t, s):
    return 'PcPull%d_%d' % (t, s)


def el(t, s, kind):
    return 'PcT%dS%d%s' % (t, s, kind)


def var(t, s, kind):
    return '{PcT%dS%d%s}' % (t, s, kind)


def act(trigger, op, path, arg):
    t = L.Table({0: trigger, 1: op, 2: path, 3: arg})
    t.types = {0: 'u32', 1: 'u32', 2: 'off', 3: 'off'}
    return t


def link(direction, group):
    t = L.Table({0: direction, 1: group})
    t.types = {0: 'u32', 1: 'off'}
    return t


def group(name, links, f2=None):
    t = L.Table({0: name, 1: links})
    t.types = {0: 'off', 1: 'off'}
    if f2:
        t.fields[2], t.types[2] = f2, 'off'
    return t


class Gen:
    def __init__(self, data):
        self.l = L.Lmb(data)
        self.states = self.l.states()
        self.others = self.states['Others']
        self.oel = self.l.elements(self.others)
        self.gel = self.l.elements(self.states['Game'])
        self.sel = self.l.elements(self.states['Sound'])
        self.navi = self.states['NaviFrequency']
        self.nel = self.l.elements(self.navi)

    def element(self, tmpl, name, anchor, actions, grp):
        """A new element: the template's scalar fields (widget type, size, button) with our name, anchor, actions, group."""
        e = self.l.to_table(tmpl, L.ELEMENT)
        e.fields[0], e.types[0] = name, 'off'
        e.fields[3], e.types[3] = anchor, 'off'
        e.fields[1], e.types[1] = [act(*a) for a in actions], 'off'
        if grp is None:
            e.fields.pop(11, None)
            e.types.pop(11, None)
        else:
            e.fields[11], e.types[11] = grp, 'off'
        return e

    def tab_element(self, k, slot, current):
        """PC tab k in tab-bar slot 1..5: a copy of the game's "Game" tab element (or, for the open tab, of "Other" as
        seen from its own page: no f2/f6) with the PC state as its name and target."""
        tmpl = self.oel['Others' if current else 'Game']
        key = '{TabOthers}' if current else '{TabGame}'
        e = self.l.to_table(tmpl, L.ELEMENT)
        e.fields[0], e.types[0] = state_name(k), 'off'
        e.fields[3], e.types[3] = self.l.get(tmpl, 3, 'str').replace(key, '%02d' % slot), 'off'
        e.fields[1], e.types[1] = [act(*a) for a in self.retarget(tmpl, TABS[k][1] + RESET_NOTE, **{key: '%02d' % slot})], 'off'
        if not current:
            e.fields[2], e.types[2] = state_name(k), 'off'
        return e

    def retarget(self, tmpl, help_text, **paths):
        """Copy an element's actions, replacing path pieces and the help line."""
        out = []
        for a in self.l.get(tmpl, 1, L.ELEMENT[1]):
            trig, op, path, arg, _ = self.l.action(a)
            path, arg = path or '', arg or ''
            for k, v in paths.items():
                path = path.replace(k, v)
            if op == 7 and path == 'help_mc/dt_help':
                arg = help_text
            out.append((trig, op, path, arg))
        return out

    # ------------------------------------------------------------------ one PC tab
    def tab_state(self, t):
        name, desc, rows = TABS[t]
        base = (t // 5) * 5
        tab_help = desc + RESET_NOTE
        enter = []
        oacts = [self.l.action(a) for a in self.l.get(self.others, 1, L.STATE[1])]
        for trig, op, path, arg, _ in oacts:          # the tab bar part of the game's enter actions (wait, tab highlights)
            if trig == 10 and (path == '_root' or path.startswith('window_mc/')):
                enter.append((trig, op, path, arg))
        for i in range(5):
            enter.append((10, 7, 'window_mc/window_tab{TabNum}_%02d_mc/dt_tab_mc/dt_tab' % (i + 1), TABS[base + i][0]))
        enter.append((10, 1, 'selector_parent_mc', 'lb_list'))
        for s in range(1, 7):
            sel = SEL.format(s=s)
            if s > len(rows):
                enter.append((10, 1, sel, 'lb_hide'))
                continue
            r = rows[s - 1]
            w = r['widget']
            enter.append((10, 1, sel, {'c': 'lb_common', 'p': 'lb_pulldown', 'n': 'lb_number'}[w]))
            enter.append((10, 1, sel + '/selector_base_mc', 'lb_off'))
            enter.append((10, 7, sel + '/selector_base_mc/dt_selector_mc/dt_selector', r['label']))
            if w == 'c':
                for k in (1, 2):
                    c = sel + '/selector_common_%02d_mc' % k
                    enter.append((10, 7, c + '/dt_select_common_mc/dt_select_common', r['choices'][k - 1][1]))
                    enter.append((10, 1, c, var(t, s, 'C%d' % k)))
            elif w == 'p':
                enter.append((10, 7, sel + '/selector_pulldown_mc/dt_select_pulldown_mc/dt_select_pulldown', var(t, s, 'V')))
                pd = 'pulldown_%02d_mc' % s
                enter.append((10, 7, pd + '/dt_selector_mc/dt_selector', r['label']))
                for k, (_, lab) in enumerate(r['choices'], 1):
                    enter.append((10, 7, pd + '/selector_item_%02d_mc/dt_select_pulldown_mc/dt_select_pulldown' % k, lab))
            else:
                num = sel + '/selector_number_mc'
                enter.append((10, 1, num, 'lb_sound' if r['store'] == 'vol' else 'lb_game'))
                for d in range(1, 11):
                    enter.append((10, 1, num + '/number_dot_mc/dot_%02d_mc' % d, var(t, s, 'D%02d' % d)))
                enter.append((10, 7, num + '/dt_select_number_mc/dt_select_number', var(t, s, 'Num')))
        for trig, op, path, arg, _ in oacts:          # help line on the tab bar, cancel animation and sound
            if trig != 10:
                enter.append((trig, op, path, tab_help if (op == 7 and path == 'help_mc/dt_help') else arg))

        elements, groups = [], []
        tab_group = 'TabGroup'
        # The tab bar: the five tabs of this set (decide on one opens it: f2). L/R: host/mods/nativeopt.cpp.
        for i in range(5):
            k = base + i
            if k < len(TABS):
                elements.append(self.tab_element(k, i + 1, k == t))
        for n in ('Prev', 'Next', 'Reset'):
            tmpl = self.oel[n]
            e = self.l.to_table(tmpl, L.ELEMENT)
            e.fields[1], e.types[1] = [act(*a) for a in self.retarget(tmpl, tab_help)], 'off'
            elements.append(e)
        for s in range(1, len(rows) + 1):
            r = rows[s - 1]
            g = 'PcG%d' % s
            sel = SEL.format(s=s)
            if r['widget'] == 'c':
                for k in (1, 2):
                    tmpl = self.gel['Turbo%d' % k]
                    acts = self.retarget(tmpl, r['help'], **{'{LineTurbo}': '%02d' % s})
                    elements.append(self.element(tmpl, el(t, s, 'C%d' % k), sel + '/selector_common_%02d_mc' % k, acts, g))
            elif r['widget'] == 'p':
                tmpl = self.gel['NaviFrequency']
                acts = self.retarget(tmpl, r['help'], **{'{LineNaviFrequency}': '%02d' % s})
                e = self.element(tmpl, el(t, s, 'P'), sel + '/selector_pulldown_mc', acts, g)
                e.fields[2], e.types[2] = pull_name(t, s), 'off'     # decide opens this row's popup state
                elements.append(e)
            else:
                for n, kind in (('BGM', 'N'), ('BGMMute', 'M')):
                    tmpl = self.sel[n]
                    acts = self.retarget(tmpl, r['help'], selector_01_mc='selector_%02d_mc' % s)
                    anchor = self.l.get(tmpl, 3, 'str').replace('selector_01_mc', 'selector_%02d_mc' % s)
                    elements.append(self.element(tmpl, el(t, s, kind), anchor, acts, g))
            ups = [link(1, 'PcG%d' % (s - 1))] if s > 1 else [link(1, tab_group)]
            downs = [link(2, 'PcG%d' % (s + 1))] if s < len(rows) else []
            groups.append(group(g, ups + downs))
        groups.insert(0, group(tab_group, [link(2, 'PcG1')], state_name(t)))

        st = self.l.to_table(self.others, L.STATE)
        st.fields[0], st.types[0] = state_name(t), 'off'
        st.fields[1], st.types[1] = [act(*a) for a in enter], 'off'
        st.fields[2], st.types[2] = elements, 'off'
        st.fields[8], st.types[8] = groups, 'off'
        st.fields[9], st.types[9] = 'PcG1', 'off'
        return st

    # ------------------------------------------------------------------ the popup of one pulldown row
    def pull_state(self, t, s):
        r = TABS[t][2][s - 1]
        n = len(r['choices'])
        pd = 'pulldown_%02d_mc' % s
        rep = {'{LineNaviFrequency}': '%02d' % s}
        enter = []
        for trig, op, path, arg, _ in (self.l.action(a) for a in self.l.get(self.navi, 1, L.STATE[1])):
            for k, v in rep.items():
                path = path.replace(k, v)
            enter.append((trig, op, path, arg))
        enter.insert(1, (10, 1, pd, 'lb_start_list%02d' % (n + 1)))
        enter.append((10, 7, pd + '/selector_main_mc/dt_select_pulldown_mc/dt_select_pulldown', var(t, s, 'V')))
        for k in range(1, n + 1):
            enter.append((10, 1, pd + '/selector_item_%02d_mc' % k, var(t, s, 'I%d' % k)))
        enter.append((1, 1, pd, 'lb_end_list%02d' % (n + 1)))
        enter.append((2, 1, pd, 'lb_end_list%02d' % (n + 1)))
        elements, groups = [], []
        tmpl = self.gel['Turbo1']
        for k in range(1, n + 1):
            a = pd + '/selector_item_%02d_mc' % k
            acts = [(3, 1, a, 'lb_select'), (4, 1, a, 'lb_unselect'), (1, 1, a, 'lb_decide'), (3, 7, 'help_mc/dt_help', r['help']),
                    (3, 10, '', 'Play_out_cursor'), (1, 10, '', 'Play_out_decision')]
            # one focus group, like the game's choice rows: the cursor opens on the item the host marks current
            e = self.element(tmpl, 'PcI%d' % k, a, acts, 'PcIG')
            e.fields[6], e.types[6] = 4, 'u32'     # decide returns to the previous state (as the game's list and CancelArea do)
            elements.append(e)
        groups.append(group('PcIG', []))
        for nm in ('CancelArea',):   # (the game's MenuArea belongs to its list widget and takes the cursor from our items)
            tm = self.nel[nm]
            e = self.l.to_table(tm, L.ELEMENT)
            e.fields[3], e.types[3] = self.l.get(tm, 3, 'str').replace('{LineNaviFrequency}', '%02d' % s), 'off'
            elements.append(e)
        st = self.l.to_table(self.navi, L.STATE)
        st.fields[0], st.types[0] = pull_name(t, s), 'off'
        st.fields[1], st.types[1] = [act(*a) for a in enter], 'off'
        st.fields[2], st.types[2] = elements, 'off'
        st.fields[8], st.types[8] = groups, 'off'
        st.fields[9], st.types[9] = 'PcIG', 'off'
        return st

    # ------------------------------------------------------------------ the game's own tabs set their tab texts again
    def game_tab(self, name):
        s = self.states[name]
        enter = [L.Ref(a) for a in self.l.get(s, 1, L.STATE[1])]
        at = next(i for i, a in enumerate(enter) if self.l.get(a.off, 2, 'str') == 'selector_parent_mc')
        add = [act(10, 7, 'window_mc/window_tab{TabNum}_%02d_mc/dt_tab_mc/dt_tab' % i, '{Option_UI.%s}' % m)
               for i, m in enumerate(GAME_TAB_TEXT, 1)]
        st = self.l.to_table(s, L.STATE)
        st.fields[1], st.types[1] = enter[:at] + add + enter[at:], 'off'
        return st

    def build(self):
        new_states = {n: self.game_tab(n) for n in GAME_TABS}
        states = [new_states.get(self.l.get(s, 0, 'str'), L.Ref(s)) for s in self.l.get(self.l.root, 1, L.ROOT[1])]
        for t in range(len(TABS)):
            states.append(self.tab_state(t))
            for s, r in enumerate(TABS[t][2], 1):
                if r['widget'] == 'p':
                    states.append(self.pull_state(t, s))
        root = self.l.to_table(self.l.root, L.ROOT)
        root.fields[1], root.types[1] = states, 'off'
        return L.Patcher(self.l).build(root)


def rows_txt():
    """The row table for host/mods/nativeopt.cpp: tab lines, then one line per row (tab-separated)."""
    out = ['# native-options rows (scripts/ui/make_native_options.py %s): T <tab> <state> <name>' % VERSION,
           '# R <tab> <slot> <widget c|p|n> <store opt|map|vol> <key> <default> <value=label|value=label...>']
    for t, (name, _, rows) in enumerate(TABS):
        out.append('T\t%d\t%s\t%s' % (t, state_name(t), name))
        for s, r in enumerate(rows, 1):
            ch = '|'.join('%s=%s' % (v, lab) for v, lab in r['choices'])
            out.append('R\t%d\t%d\t%s\t%s\t%s\t%s\t%s' % (t, s, r['widget'], r['store'], r['key'], r['default'], ch))
    return '\n'.join(out) + '\n'


def script_hash():
    h = hashlib.sha256()
    for f in ('make_native_options.py', 'lmb.py'):
        h.update(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), f), 'rb').read())
    return h.hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=SRC)
    ap.add_argument('--out', default=os.path.join(os.environ.get('APPDATA', '.'), 'SnapRecomp', 'mods'))
    ap.add_argument('--if-stale', action='store_true', help='do nothing when the pack was built by this script version')
    a = ap.parse_args()
    pack = os.path.join(a.out, 'native-options')
    stamp = os.path.join(pack, 'build.stamp')
    if a.if_stale and os.path.exists(stamp) and open(stamp).read().strip() == script_hash():
        print('native-options pack is up to date')
        return
    if not os.path.exists(a.src):
        sys.exit('native-options: %s not found (extract the game files first)' % a.src)
    data = Gen(open(a.src, 'rb').read()).build()
    L.Lmb(data).states()   # parses back
    d = os.path.join(pack, 'common', 'UI', 'master', 'Scenes', 'option.drp')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'option.lmb'), 'wb') as f:
        f.write(data)
    with open(os.path.join(pack, 'rows.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(rows_txt())
    with open(os.path.join(pack, 'pack.json'), 'w') as f:
        json.dump({'name': 'Native PC options', 'version': VERSION,
                   'description': 'PC settings as extra tabs of the game\'s own Settings screen (generated locally).'}, f, indent=1)
    with open(stamp, 'w') as f:
        f.write(script_hash())
    print('wrote %s (%d bytes, %d PC tabs)' % (os.path.join(d, 'option.lmb'), len(data), len(TABS)))


if __name__ == '__main__':
    main()
