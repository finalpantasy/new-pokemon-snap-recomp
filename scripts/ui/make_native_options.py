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
import argparse, hashlib, json, os, re, sys
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
    assert len(label) <= 24, label
    return dict(label=label, widget=widget, store=store, key=key, default=default, choices=choices, help=help)


def nums(*vals, fmt='{}', suffix=''):
    return tuple((v, fmt.format(v) + suffix) for v in vals)


def visual_defaults():
    """The visual-extras defaults from runtime/hle/visual_defaults.h (the runtime's one table), so the menu's defaults
    (and Restore Default Settings) cannot disagree with the runtime: {env name: default value}."""
    src = open(os.path.join(ROOT, 'runtime', 'hle', 'visual_defaults.h'), encoding='utf-8').read()
    enhanced = re.search(r'kDefaultEnhanced\s*=\s*(true|false)', src).group(1) == 'true'
    rows = re.findall(r'\{"(SNAP_\w+)",\s*"([^"]*)",\s*"([^"]*)"', src)
    assert rows, 'visual_defaults.h: no rows'
    return {k: (e if enhanced else o) for k, o, e in rows}


# Anti-aliasing row choices in the F1 table's values, so a hand pick of the default look stores "default" (the key erased) and the
# quality preset keeps its name (kPresets compare these strings). Original look: default = the game's FXAA; enhanced: default = ssaa2+smaa.
AA_CHOICES = ((('default', 'Original'), ('smaa', 'SMAA'), ('ssaa2,smaa', 'SMAA + supersampling'))
              if visual_defaults()['SNAP_AA'] in ('original', 'off', 'fxaa') else
              (('off', 'Original'), ('smaa', 'SMAA'), ('default', 'SMAA + supersampling')))


TABS = [
    ('Quality', 'Picture quality for this PC version.', [
        row('Quality preset', 'p', 'opt', 'gfx_preset', 'normal',
            (('verylow', 'Very Low'), ('low', 'Low'), ('normal', 'Normal'), ('high', 'High'), ('ultra', 'Ultra'),
             ('maximum', 'Maximum'), ('custom', 'Custom')),
            'Sets this tab and the next together. Very Low suits weak PCs, Maximum a very fast GPU.' + R),
        row('Render scale', 'p', 'opt', 'gfx_render_scale', '1.00',
            (('0.50', '0.5x'), ('0.75', '0.75x'), ('1.00', '1x (1080p)'), ('1.25', '1.25x'), ('1.50', '1.5x'), ('1.75', '1.75x'),
             ('2.00', '2x (4K)'), ('2.50', '2.5x'), ('3.00', '3x'), ('4.00', '4x (8K)')),
            'Scene size as a multiple of 1080p. 4x needs a lot of video memory.' + R),
        # Visual extras (owner 2026-11-13): original look by default, every extra off. The AA values are the F1 table's
        # (ui_options.cpp gfx_aa, kPresets): "default" = the visual_defaults.h AA, "off" = the game's FXAA; choices set below.
        row('Anti-aliasing', 'p', 'opt', 'gfx_aa', 'default', AA_CHOICES,
            'Smooths jagged edges. Supersampling draws at 2x size (1.5x at 4K) and costs the most.' + R),
        # (a pulldown, not the game's 0..10 slider: its knob is moved by the game's handler, which a PC page bypasses)
        row('Sharpening', 'p', 'opt', 'gfx_sharpen', '0.00',
            (('0.00', 'Off'),) + nums('0.10', '0.20', '0.30', '0.40', '0.50', '0.60', '0.70', '0.85', '1.00'),
            'Contrast-adaptive sharpening after anti-aliasing. An upscaler turns it off unless set.' + R),
        row('Upscaler', 'p', 'opt', 'gfx_upscaler', 'off',
            (('off', 'Off'), ('dlaa', 'DLAA'), ('dlss', 'DLSS'), ('fsr', 'FSR'), ('xess', 'XeSS')),
            'Rebuilds a sharp picture from fewer pixels. DLSS and DLAA need an NVIDIA RTX card.' + R),
        row('Upscaler quality', 'p', 'opt', 'gfx_upscale_quality', 'native',
            (('native', 'Native'), ('quality', 'Quality'), ('balanced', 'Balanced'), ('performance', 'Performance')),
            'How many pixels are drawn before the upscaler enlarges them. Lower modes run faster.' + R),
    ]),
    ('Textures', 'Texture, shadow and shading detail.', [
        row('Texture filtering', 'p', 'opt', 'gfx_aniso', '0',
            (('0', 'Original'), ('2', '2x anisotropic'), ('4', '4x anisotropic'), ('8', '8x anisotropic'), ('16', '16x anisotropic')),
            'Keeps ground and walls sharp at shallow angles. 16x costs almost nothing on a PC.' + R),
        row('Texture sharpness', 'p', 'opt', 'gfx_lod_bias', 'auto',
            (('auto', 'Automatic'), ('-2', '-2 (sharpest)'), ('-1.5', '-1.5'), ('-1', '-1 (sharper)'), ('-0.5', '-0.5'),
             ('0', '0 (original)'), ('0.25', '+0.25'), ('0.5', '+0.5'), ('1', '+1 (softer)'), ('2', '+2 (softest)')),
            'Sharper (negative) or softer (positive) texture detail. Sharper settings can shimmer.' + R),
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
        # HDR mode: a saved "1" (the old on/off row) is HDR10; nvn_hdr.cpp reads 0/off, 1/hdr10, scrgb
        row('HDR output', 'p', 'opt', 'gfx_hdr', '0', (('0', 'Off'), ('1', 'HDR10'), ('scrgb', 'scRGB')),
            'For an HDR monitor with Windows HDR on. scRGB has far less banding.' + R),
        # Auto = no key: the monitor's reported peak, else 1000
        row('HDR peak brightness', 'p', 'opt', 'gfx_hdr_peak', 'default',
            (('default', 'Auto'),) + nums('400', '600', '800', '1000', '1500', '2000', '3000', '4000', suffix=' nits'),
            'The brightest highlight to send. Auto reads the monitor.' + R),
        row('HDR paper white', 'p', 'opt', 'gfx_hdr_paper', '200',
            nums('80', '100', '150', '200', '250', '300', '400', suffix=' nits'),
            'How bright ordinary white is. Lower it if the picture feels too bright in a dark room.' + R),
        row('HDR highlights', 'p', 'opt', 'gfx_hdr_expand', '0.30',
            nums('0.00', '0.10', '0.20', '0.30', '0.40', '0.50', '0.60', '0.80', '1.00'),
            'How far near-white scene pixels lift into HDR. Needs HDR on.' + R),
        row('Frame rate limit', 'p', 'opt', 'frame_cap', '0',
            (('0', 'Off'),) + nums('30', '45', '60', '75', '90', '120', '144', '165', '240', suffix=' FPS'),
            'Stops the game from drawing faster than this. Off keeps the original 60 FPS pacing.'),
        # live (runtime/nvn/nvn_vk.cpp snap_vsync_set: the next present rebuilds the swapchain); settings key vsync -> SNAP_VSYNC
        row('Vsync', 'p', 'opt', 'vsync', 'on', (('on', 'On'), ('adaptive', 'Adaptive'), ('fast', 'Fast'), ('off', 'Off')),
            'On: no tearing. Adaptive: tears only below the refresh rate. Fast: lower latency. Off: lowest, may tear.'),
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
        row('Sharper menu text', 'c', 'opt', 'gfx_ui_font', 'off', (('auto', 'On'), ('off', 'Off')),
            'At 1440p and above, draws menu letters twice as sharp, at the same size.' + R),
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
        row('Master volume', 'p', 'vol', 'master', '10',
            (('0', 'Mute'),) + tuple((str(v), '%d%%' % (v * 10)) for v in range(2, 11)),
            'Overall loudness of the game.'),
        row('Output mode', 'p', 'opt', 'audio_output', 'stereo',
            (('stereo', 'Stereo'), ('speakers', 'Speakers (5.1/7.1)'), ('spatial', 'Spatial (headphones)')),
            'Stereo is the original mix. Speakers sends surround, Spatial uses Windows Spatial Sound.' + R),
        row('Mute in background', 'c', 'opt', 'win_mute', '1', TOGGLE,
            'Silences the game while its window is not in front.'),
    ]),
    ('Photos', 'Photo files and expeditions.', [
        row('Save PNG copies', 'c', 'opt', 'photo_export', '0', TOGGLE,
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
        # opt-in mod row (runtime/nvn/nvn_shiny.cpp, needs the shiny pack): key shiny -> SNAP_SHINY=1 + SNAP_SHINY_ODDS (settings.cpp)
        row('Shiny Pokémon', 'p', 'opt', 'shiny', 'default',
            (('default', 'Off'), ('4096', 'Rare (1/4096)'), ('512', 'Common (1/512)')),
            'Very rarely, a Pokémon appears in its shiny colors. Cosmetic only; photo scores are unchanged.'
            ' Needs a restart.'),   # (the usual ' Applies after restart.' passes the 112-character limit)
    ]),
    # Tab 11, the first of a third tab set (docs/advanced-graphics-options.md). Reserved slots, in order: AO radius and AO strength
    # (SNAP_SSAO_RADIUS / SNAP_SSAO_STRENGTH are parsed in host/mods/gamemods.cpp but not applied yet), then the lighting stream's
    # Lighting preset, shadow softness and ray-traced effects. Not in the quality presets (open question for the owner).
    ('Shadows & AO', 'Shadow and ambient occlusion detail.', [
        row('Shadow distance', 'p', 'opt', 'gfx_shadow_dist', 'default',
            (('default', '1x (original)'),) + nums('1.5', '2', '3', '4', '6', '8', '12', '16', suffix='x'),
            'How far moving shadows reach. Pair with shadow resolution: farther is blurrier.' + R),
    ]),
]
VISUAL_KEYS = {'gfx_sharpen': 'SNAP_SHARPEN', 'gfx_aniso': 'SNAP_ANISO', 'gfx_ui_font': 'SNAP_UI_FONT_SCALE',
               'photo_export': 'SNAP_PHOTO_EXPORT'}
_VD = visual_defaults()
for _t in TABS:
    assert len(_t[2]) <= 6, (_t[0], 'a PC page has 6 row slots and no scrolling')
    for _r in _t[2]:
        if _r['key'] in VISUAL_KEYS:
            _r['default'] = _VD[VISUAL_KEYS[_r['key']]]
            assert _r['default'] in [c[0] for c in _r['choices']], (_r['label'], _r['default'])

RESET_NOTE = ' Restore Default Settings resets only this tab.'
GAME_TABS = ('Game', 'Camera', 'Button', 'Sound', 'Others')    # state = tab element names, in tab-bar order
# message ids of the game's tab names (Init sets them once through {Tab1Text}..; only Init has those variables)
GAME_TAB_TEXT = ('Option_TabGame_Tx', 'Option_TabCamera_Tx', 'Option_TabButton_Tx', 'Option_TabSound_Tx', 'Option_Extra_Tx')
SEL = 'selector_parent_mc/selector_{s:02d}_mc'
# The tab bar layout for TabNum tabs (lb_tab_02..05). The game plays it only in Init; every game and PC tab replays it, so a short
# last PC set (TabNum 2..4, nativeopt.cpp) and the 5-tab sets around it each show their own bar.
TAB_BAR = (10, 1, 'window_mc', 'lb_tab_0{TabNum}')


def pull_slot(s, n):
    """Row slot whose popup (pulldown_NN_mc) a pulldown on slot s with n items uses. A popup opens downward from its row
    (rows 60 px apart, items 37 px, at 960 x 540), so a long list on a low row would run off the screen: it opens from a
    higher row instead (row 1 holds 10 items). host/mods/nativeopt.cpp reads it from rows.txt (prepare_pull)."""
    p = s
    while p > 1 and 60 * (p - 1) + 37 * (n - 1) > 339:
        p -= 1
    return p


def pull_texts(s, r):
    """Enter actions writing a pulldown's popup texts (row label, item labels) into its popup slot."""
    pd = 'pulldown_%02d_mc' % pull_slot(s, len(r['choices']))
    out = [(10, 7, pd + '/dt_selector_mc/dt_selector', r['label'])]
    for k, (_, lab) in enumerate(r['choices'], 1):
        out.append((10, 7, pd + '/selector_item_%02d_mc/dt_select_pulldown_mc/dt_select_pulldown' % k, lab))
    return out


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
        enter = [TAB_BAR]
        oacts = [self.l.action(a) for a in self.l.get(self.others, 1, L.STATE[1])]
        for trig, op, path, arg, _ in oacts:          # the tab bar part of the game's enter actions (wait, tab highlights)
            if trig == 10 and (path == '_root' or path.startswith('window_mc/')):
                enter.append((trig, op, path, arg))
        # a short last set: nativeopt.cpp sets TabNum to max(2, its tab count) (the screen has 2- to 5-tab bars); a spare slot is blank
        for i in range(max(2, min(5, len(TABS) - base))):
            enter.append((10, 7, 'window_mc/window_tab{TabNum}_%02d_mc/dt_tab_mc/dt_tab' % (i + 1),
                          TABS[base + i][0] if base + i < len(TABS) else ''))
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
                enter += pull_texts(s, r)   # (the popup state writes them again: two rows can share a popup slot)
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
        pd = 'pulldown_%02d_mc' % pull_slot(s, n)
        rep = {'pulldown_{LineNaviFrequency}': pd[:-3], '{LineNaviFrequency}': '%02d' % s}   # popup slot, then the row's own
        enter = []
        for trig, op, path, arg, _ in (self.l.action(a) for a in self.l.get(self.navi, 1, L.STATE[1])):
            for k, v in rep.items():
                path = path.replace(k, v)
            enter.append((trig, op, path, arg))
        enter += pull_texts(s, r)
        # (no lb_start_list enter action, as in the game's popups: the host starts the list on decide, see prepare_pull)
        enter.append((10, 7, pd + '/selector_main_mc/dt_select_pulldown_mc/dt_select_pulldown', var(t, s, 'V')))
        for k in range(1, n + 1):
            enter.append((10, 1, pd + '/selector_item_%02d_mc' % k, var(t, s, 'I%d' % k)))
        enter.append((1, 1, pd, 'lb_end_list%02d' % (n + 1)))
        enter.append((2, 1, pd, 'lb_end_list%02d' % (n + 1)))
        # The game's own list element (type 6, f6 = 4): it moves the cursor over selector_item_01..f4 itself, reports
        # focus and decide as PulldownMenuItemNN and returns to the previous state on decide. (Separate item elements
        # linked by focus groups never moved: the player resolved Down back to the same item.)
        acts = [(3, 1, pd + '/selector_main_mc', 'lb_select'), (4, 1, pd + '/selector_main_mc', 'lb_unselect'),
                (1, 1, pd + '/selector_main_mc', 'lb_decide'), (3, 7, 'help_mc/dt_help', r['help']), (3, 10, '', 'Play_out_cursor')]
        e = self.element(self.nel['PulldownMenu'], 'PulldownMenu', pd + '/selector_main_mc', acts, None)
        e.fields[4], e.types[4] = pd + '/selector_item_%02d_mc' % n, 'off'
        elements = [e]
        # The game's MenuArea (type 2 on the whole list, as in its NaviFrequency popup): part of the list widget; the game's
        # popup descriptor lists it next to PulldownMenu.
        ma = self.l.to_table(self.nel['MenuArea'], L.ELEMENT)
        ma.fields[3], ma.types[3] = pd, 'off'
        elements.append(ma)
        for nm in ('CancelArea',):
            tm = self.nel[nm]
            e = self.l.to_table(tm, L.ELEMENT)
            # B and a click outside close the popup through this element (f6 = 4); on the full-screen dimmer it took the
            # cursor whenever the mouse rested on the screen, so it sits on the (covered) L arrow instead
            e.fields[3], e.types[3] = 'arrow_l_mc', 'off'
            elements.append(e)
        st = self.l.to_table(self.navi, L.STATE)
        st.fields[0], st.types[0] = pull_name(t, s), 'off'
        st.fields[1], st.types[1] = [act(*a) for a in enter], 'off'
        st.fields[2], st.types[2] = elements, 'off'
        return st   # (no focus groups, as in the game's NaviFrequency popup)

    # ------------------------------------------------------------------ the game's own tabs set their tab texts again
    def game_tab(self, name):
        s = self.states[name]
        enter = [L.Ref(a) for a in self.l.get(s, 1, L.STATE[1])]
        at = next(i for i, a in enumerate(enter) if self.l.get(a.off, 2, 'str') == 'selector_parent_mc')
        add = [act(10, 7, 'window_mc/window_tab{TabNum}_%02d_mc/dt_tab_mc/dt_tab' % i, '{Option_UI.%s}' % m)
               for i, m in enumerate(GAME_TAB_TEXT, 1)]
        st = self.l.to_table(s, L.STATE)
        st.fields[1], st.types[1] = [act(*TAB_BAR)] + enter[:at] + add + enter[at:], 'off'
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
           '# R <tab> <slot> <widget c|p|n> <store opt|map|vol> <key> <default> <value=label|value=label...> <popup slot> <label>']
    for t, (name, _, rows) in enumerate(TABS):
        out.append('T\t%d\t%s\t%s' % (t, state_name(t), name))
        for s, r in enumerate(rows, 1):
            ch = '|'.join('%s=%s' % (v, lab) for v, lab in r['choices'])
            out.append('R\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%d\t%s' % (t, s, r['widget'], r['store'], r['key'], r['default'], ch,
                                                               pull_slot(s, len(r['choices'])), r['label']))
    return '\n'.join(out) + '\n'


def script_hash():
    h = hashlib.sha256()
    for f in ('make_native_options.py', 'lmb.py'):
        h.update(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), f), 'rb').read())
    h.update(open(os.path.join(ROOT, 'runtime', 'hle', 'visual_defaults.h'), 'rb').read())   # the row defaults come from it
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
