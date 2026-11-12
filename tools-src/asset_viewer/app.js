import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { clone as cloneSkeleton } from 'three/addons/utils/SkeletonUtils.js';

const $ = id => document.getElementById(id);
const state = { manifest: { assets: [], variants: [] }, asset: null, models: [], split: .5, texSplit: .5, tab: 'model', texture: null, images: [], zoom: 1, pan: { x: 0, y: 0 }, channel: 'RGB', background: 0, modelEpoch: 0, textureEpoch: 0, modelNotes: [], textureNotes: [] };
const imageCache = new Map();
const backgrounds = [['dark', '#191d24'], ['light', '#e6e8eb'], ['gray', '#777b80']];
function option(value, label) { return new Option(label, value); }
const isAlt = id => typeof id === 'string' && id.startsWith('alt:');
const altModel = id => state.asset?.alt_models?.find(a => 'alt:' + a.id === id);
function modelChoices() {
  return [{ id: 'orig', label: 'Original (game)' }, ...state.manifest.variants.map(v => ({ id: v.id, label: 'HD · ' + v.label })), ...(state.asset?.alt_models || []).map(a => ({ id: 'alt:' + a.id, label: 'Alt · ' + (a.label || a.id) }))];
}
function variantLabel(id) { return modelChoices().find(v => v.id === id)?.label || id; }
function populateChoices() {
  const choices = modelChoices(), prev = [$('variantA').value, $('variantB').value], has = v => choices.some(c => c.id === v);
  for (const id of ['variantA', 'variantB']) $(id).replaceChildren(...choices.map(c => option(c.id, c.label)));
  $('variantA').value = has(prev[0]) ? prev[0] : 'orig';
  $('variantB').value = has(prev[1]) ? prev[1] : (state.manifest.variants[0]?.id || 'orig');
  updateLabels();
}
function notice() { $('notice').textContent = [...new Set([...state.modelNotes, ...state.textureNotes])].join(' · '); }
function cacheURL(path) {
  if (typeof path !== 'string' || !path || path.includes('\\') || path.includes(':') || path.startsWith('/') || path.split('/').includes('..')) throw new Error('Invalid cache path');
  return '/cache/' + path.split('/').map(encodeURIComponent).join('/');
}
async function imageFor(path) {
  const url = cacheURL(path);
  if (!imageCache.has(url)) imageCache.set(url, (async () => {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`Missing texture: ${path}`);
    const blob = await response.blob();
    const image = new Image();
    const objectURL = URL.createObjectURL(blob);
    try { image.src = objectURL; await image.decode(); return image; }
    finally { URL.revokeObjectURL(objectURL); }
  })());
  return imageCache.get(url);
}
async function variantImage(texture, variant, notes) {
  if (isAlt(variant)) variant = 'orig'; // alternate models carry their own textures
  const path = variant === 'orig' ? texture.orig : texture.variants?.[variant];
  if (path) {
    try { return await imageFor(path); }
    catch { notes.push(`${texture.name}: ${variantLabel(variant)} unavailable; using original`); }
  } else if (variant !== 'orig') notes.push(`${texture.name}: ${variantLabel(variant)} unavailable; using original`);
  try { return await imageFor(texture.orig); }
  catch { notes.push(`${texture.name}: original unavailable`); return null; }
}
function pixels(image) {
  const canvas = document.createElement('canvas'); canvas.width = image.width; canvas.height = image.height;
  const context = canvas.getContext('2d', { willReadFrequently: true }); context.drawImage(image, 0, 0);
  return { canvas, context, data: context.getImageData(0, 0, image.width, image.height) };
}
function hasAlpha(image) { const data = pixels(image).data.data; for (let i = 3; i < data.length; i += 4) if (data[i] < 255) return true; return false; }

let renderer, scene, camera, controls;
try {
  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); renderer.setClearColor(backgrounds[0][1]);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  $('viewport').prepend(renderer.domElement);
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(38, 1, .001, 10000);
  camera.position.set(3, 2, 4);
  controls = new OrbitControls(camera, renderer.domElement); controls.enableDamping = true;
  scene.add(new THREE.HemisphereLight(0xffffff, 0x636974, 2));
  const key = new THREE.DirectionalLight(0xfff9ef, 3); key.position.set(4, 6, 5); scene.add(key);
  const rim = new THREE.DirectionalLight(0xdceaff, 2.5); rim.position.set(-4, 3, -4); scene.add(rim);
} catch { $('modelMessage').textContent = 'WebGL is unavailable. Texture comparison is still available.'; }
function resize() {
  if (renderer && !$('modelPanel').hidden) {
    const w = $('viewport').clientWidth, h = $('viewport').clientHeight;
    if (w && h) { renderer.setSize(w, h, false); camera.aspect = w / h; camera.updateProjectionMatrix(); }
  }
  drawTexture();
}
new ResizeObserver(resize).observe($('viewport'));
new ResizeObserver(drawTexture).observe($('textureViewport'));
function render() {
  requestAnimationFrame(render);
  if (!renderer || $('editor').hidden || state.tab !== 'model') return;
  controls.update();
  const w = $('viewport').clientWidth, h = $('viewport').clientHeight;
  renderer.setViewport(0, 0, w, h); renderer.setScissorTest(false); renderer.clear();
  const split = $('split').checked;
  renderer.setScissorTest(true);
  state.models.forEach((root, index) => {
    if (index === 1 && !split) return;
    state.models.forEach((model, i) => model.visible = i === index);
    const x = Math.round(w * state.split);
    renderer.setScissor(index ? x : 0, 0, split ? (index ? w - x : x) : w, h);
    renderer.render(scene, camera);
  });
  renderer.setScissorTest(false);
}
render();
function disposeModels(roots) {
  const resources = new Set();
  roots.forEach(root => { scene?.remove(root); root.traverse(object => {
    if (object.geometry) resources.add(object.geometry);
    for (const material of Array.isArray(object.material) ? object.material : [object.material]) {
      if (!material) continue;
      resources.add(material); for (const value of Object.values(material)) if (value?.isTexture) resources.add(value);
    }
  }); });
  resources.forEach(resource => resource.dispose());
}
function frameModel() {
  if (!state.models.length || !camera) return;
  const box = new THREE.Box3(); state.models.forEach(m => box.union(new THREE.Box3().setFromObject(m)));
  if (box.isEmpty()) return;
  const center = box.getCenter(new THREE.Vector3());
  const radius = Math.max(box.getSize(new THREE.Vector3()).length() / 2, .01);
  controls.target.copy(center);
  camera.position.copy(center).add(new THREE.Vector3(1, .55, 1.5).normalize().multiplyScalar(radius / Math.sin(THREE.MathUtils.degToRad(camera.fov / 2)) * 1.15));
  camera.near = radius / 1000; camera.far = radius * 1000; camera.updateProjectionMatrix();
  controls.minDistance = radius * .05; controls.maxDistance = radius * 100; controls.update();
}
// The cache glTF owns geometry/skin/UVs; the manifest owns all texture bindings.
function stripTextures(value) {
  if (!value || typeof value !== 'object') return;
  for (const key of Object.keys(value)) {
    if (key.endsWith('Texture')) delete value[key]; else stripTextures(value[key]);
  }
}
async function parseOriginal(asset) {
  const path = (asset.lods?.length ? asset.lods : [asset.model])[Number($('lod').value) || 0];
  if (!path) throw new Error('No model in this cache entry. Textures can still be previewed.');
  const url = cacheURL(path), response = await fetch(url);
  if (!response.ok) throw new Error('Model is not available yet. Refresh the cache when the build completes.');
  const json = await response.json();
  json.images = []; json.textures = []; json.samplers = [];
  (json.materials || []).forEach(stripTextures);
  return (await new GLTFLoader(resourceManager()).parseAsync(JSON.stringify(json), url.slice(0, url.lastIndexOf('/') + 1))).scene;
}
function resourceManager() {
  const manager = new THREE.LoadingManager();
  manager.setURLModifier(resource => {
    const resolved = new URL(resource, location.origin);
    if (resolved.origin !== location.origin || !(resolved.pathname.startsWith('/cache/') || resolved.pathname.startsWith('/external/'))) throw new Error('Model resource must be inside the cache');
    return resolved.href;
  });
  return manager;
}
function countGeometry(root) {
  let verts = 0, tris = 0;
  root.traverse(o => { if (!o.isMesh) return; const p = o.geometry.attributes.position; if (!p) return; verts += p.count; tris += (o.geometry.index ? o.geometry.index.count : p.count) / 3; });
  return { verts, tris: Math.round(tris) };
}
async function loadModel(reframe = false) {
  const epoch = ++state.modelEpoch, asset = state.asset;
  state.modelNotes = []; notice();
  if (!renderer || !asset) return;
  $('modelMessage').textContent = 'Loading model…';
  const roots = [], notes = [], errors = [];
  let basePromise;
  const base = () => basePromise ||= parseOriginal(asset);
  try {
    for (let i = 0; i < 2; i++) {
      const choice = $(i ? 'variantB' : 'variantA').value;
      try {
        if (isAlt(choice)) {
          const alt = altModel(choice);
          if (!alt) throw new Error('Alternate model is not listed for this asset');
          const gltf = await new GLTFLoader(resourceManager()).loadAsync(alt.url);
          gltf.scene.traverse(o => { if (o.isMesh) for (const m of Array.isArray(o.material) ? o.material : [o.material]) m.wireframe = $('wireframe').checked; });
          roots.push(gltf.scene);
          continue;
        }
        const root = cloneSkeleton(await base());
        const materialCache = new Map();
        const materialFor = source => {
          if (!materialCache.has(source.name)) materialCache.set(source.name, (async () => {
            const binding = asset.materials?.find(m => m.name === source.name);
            const material = new THREE.MeshStandardMaterial({ name: source.name, color: 0xffffff, roughness: .72, metalness: 0, wireframe: $('wireframe').checked, side: source.side });
            if (!binding) { notes.push(`No manifest texture binding for material ${source.name || '(unnamed)'}`); return material; }
            for (const [role, slot] of [['albedo', 'map'], ['normal', 'normalMap'], ['emissive', 'emissiveMap']]) {
              const texture = asset.textures?.find(t => t.name === binding[role]);
              if (!texture) continue;
              const image = await variantImage(texture, choice, notes);
              if (!image) continue;
              const map = new THREE.Texture(image); map.flipY = false; map.colorSpace = role === 'normal' ? THREE.NoColorSpace : THREE.SRGBColorSpace;
              map.wrapS = map.wrapT = THREE.RepeatWrapping; map.needsUpdate = true;
              material[slot] = map;
              if (role === 'albedo' && hasAlpha(image)) { material.alphaTest = .5; material.side = THREE.DoubleSide; }
              if (role === 'emissive') material.emissive.set(0xffffff);
            }
            return material;
          })());
          return materialCache.get(source.name);
        };
        const pending = [];
        root.traverse(object => {
          if (!object.isMesh) return;
          const materials = Array.isArray(object.material) ? object.material : [object.material];
          pending.push(Promise.all(materials.map(materialFor)).then(m => { object.material = Array.isArray(object.material) ? m : m[0]; }));
        });
        await Promise.all(pending);
        roots.push(root);
      } catch (error) {
        errors.push(error.message); notes.push(`Side ${i ? 'B' : 'A'}: ${error.message}`);
        roots.push(new THREE.Group());
      }
    }
    if (epoch !== state.modelEpoch) { disposeModels(roots); return; }
    disposeModels(state.models); state.models = roots; roots.forEach(root => scene.add(root));
    state.counts = roots.map(countGeometry);
    updateLabels();
    state.modelNotes = notes; notice();
    if (errors.length === 2) {
      $('modelMessage').textContent = errors[0]; $('viewport').dataset.ready = 'false';
      $('status').textContent = 'Model unavailable · texture preview available';
      return;
    }
    $('modelMessage').textContent = '';
    if (reframe) frameModel();
    $('status').textContent = `${asset.id} · LOD ${$('lod').value} · ${asset.textures?.length || 0} textures`;
    $('viewport').dataset.ready = 'true'; resize();
  } catch (error) {
    disposeModels(roots);
    if (epoch !== state.modelEpoch) return;
    $('modelMessage').textContent = error.message; $('viewport').dataset.ready = 'false';
  }
}
function fmt(n) { return n.toLocaleString('en-US'); }
function updateLabels() {
  for (const [id, variant, prefix] of [['labelA','variantA','A'],['labelB','variantB','B']]) {
    const c = state.counts?.[prefix === 'A' ? 0 : 1];
    $(id).textContent = `${prefix} / ${variantLabel($(variant).value)}` + (c ? ` · ${fmt(c.tris)} tris · ${fmt(c.verts)} verts` : '');
  }
  for (const [id, variant, prefix] of [['texLabelA','variantA','A'],['texLabelB','variantB','B']]) $(id).textContent = `${prefix} / ${variantLabel($(variant).value)}`;
  $('labelB').hidden = $('modelDivider').hidden = !$('split').checked;
}
function listAssets() {
  const search = $('search').value.toLowerCase(), category = $('category').value;
  $('assetList').replaceChildren();
  const assets = state.manifest.assets.filter(asset => `${asset.name} ${asset.id}`.toLowerCase().includes(search) && (!category || (asset.category || 'other') === category));
  assets.forEach(asset => {
    const row = document.createElement('button'); row.className = 'asset-row' + (state.asset?.id === asset.id ? ' active' : '');
    const title = document.createElement('strong'); title.textContent = asset.name || asset.id;
    const detail = document.createElement('small'); detail.textContent = `${asset.category || 'other'} · ${asset.textures?.length || 0} textures`;
    row.append(title, detail); row.onclick = () => selectAsset(asset); $('assetList').append(row);
  });
  if (!assets.length) $('assetList').textContent = 'No matching assets.';
  $('assetCount').textContent = `${state.manifest.assets.length} assets`;
}
function setTab(tab) {
  state.tab = tab;
  $('modelPanel').hidden = tab !== 'model'; $('texturePanel').hidden = tab !== 'texture';
  for (const [id, active] of [['modelTab', tab === 'model'], ['textureTab', tab === 'texture']]) { $(id).classList.toggle('active', active); $(id).setAttribute('aria-selected', active); }
  for (const id of ['lod', 'wireframe', 'split', 'background', 'frame']) $(id).disabled = tab !== 'model';
  resize();
}
function textureGrid() {
  $('textureGrid').replaceChildren();
  for (const texture of state.asset.textures || []) {
    const button = document.createElement('button'); button.className = 'texture-card';
    const image = document.createElement('img'); image.alt = texture.name; image.loading = 'lazy';
    try { image.src = cacheURL(texture.orig); } catch { /* Invalid paths remain blank. */ }
    image.onerror = () => { image.removeAttribute('src'); image.alt = `${texture.name} · original not available yet`; };
    const info = document.createElement('div'), title = document.createElement('strong'), detail = document.createElement('small');
    title.textContent = texture.name; detail.textContent = `${texture.role || 'other'} · ${texture.w} × ${texture.h} · ${texture.format || 'PNG'}`;
    info.append(title, detail); button.append(image, info); button.onclick = () => openTexture(texture); $('textureGrid').append(button);
  }
  if (!state.asset.textures?.length) $('textureGrid').textContent = 'No textures have been added to this asset yet.';
}
async function selectAsset(asset) {
  ++state.textureEpoch; state.asset = asset; state.texture = null; state.images = []; state.textureNotes = []; imageCache.clear();
  $('assetName').textContent = asset.name || asset.id; $('breadcrumb').textContent = `${asset.category || 'other'} / ${asset.id}`;
  $('textureCount').textContent = asset.textures?.length || 0;
  $('lod').replaceChildren(...(asset.lods?.length ? asset.lods : [asset.model]).map((_, i) => option(i, `LOD ${i}`)));
  $('textureCompare').hidden = true; $('textureGrid').hidden = false;
  populateChoices(); listAssets(); textureGrid(); setTab('model'); await loadModel(true);
}
async function refresh() {
  $('reload').disabled = true;
  try {
    const response = await fetch('/api/manifest'); if (!response.ok) throw new Error('Could not read the manifest');
    const manifest = await response.json();
    if (manifest.version !== 1 || !Array.isArray(manifest.assets) || !Array.isArray(manifest.variants)) throw new Error('Unsupported cache contract. Expected version 1.');
    state.manifest = manifest;
    $('category').replaceChildren(option('', 'All categories'), ...[...new Set(manifest.assets.map(a => a.category || 'other'))].sort().map(c => option(c, c)));
    $('empty').hidden = !!manifest.assets.length; $('editor').hidden = !manifest.assets.length;
    if (!manifest.assets.length) {
      ++state.modelEpoch; ++state.textureEpoch; disposeModels(state.models); state.models = []; state.asset = null;
      $('emptyMessage').textContent = manifest.message || 'No assets are available yet. Build the cache, then refresh.'; listAssets();
    } else await selectAsset(manifest.assets.find(a => a.id === state.asset?.id) || manifest.assets[0]);
  } catch (error) { $('empty').hidden = false; $('editor').hidden = true; $('emptyMessage').textContent = error.message; }
  finally { $('reload').disabled = false; }
}
async function openTexture(texture) {
  state.texture = texture; state.pan = { x: 0, y: 0 }; state.zoom = 1; $('zoom').value = '1';
  $('textureGrid').hidden = true; $('textureCompare').hidden = false; $('textureName').textContent = `${texture.name} / ${texture.role}`;
  await loadTextureCompare();
}
async function loadTextureCompare() {
  const epoch = ++state.textureEpoch, texture = state.texture;
  if (!texture) return;
  state.images = []; state.textureNotes = []; drawTexture();
  const notes = [], ids = [$('variantA').value, $('variantB').value];
  const images = await Promise.all(ids.map(id => variantImage(texture, id, notes)));
  if (epoch !== state.textureEpoch) return;
  state.images = images; state.textureNotes = notes; notice();
  const sizes = images.map((image, i) => `${i ? 'B' : 'A'} / ${variantLabel(ids[i])}${isAlt(ids[i]) ? ' (own textures; showing original)' : ''}: ${image ? `${image.width} × ${image.height}` : 'unavailable'}`);
  $('textureSizes').replaceChildren(...[`Original: ${texture.w} × ${texture.h} · ${texture.format || 'PNG'}`, ...sizes].map(text => { const span = document.createElement('span'); span.textContent = text; return span; }));
  channelImages(); $('textureViewport').dataset.ready = 'true';
}
function channelImages() {
  state.channelCanvases = state.images.map(image => {
    if (!image) return null;
    const { canvas, context, data } = pixels(image);
    if (state.channel !== 'RGB') {
      const channel = { R: 0, G: 1, B: 2, A: 3 }[state.channel];
      for (let i = 0; i < data.data.length; i += 4) { const value = data.data[i + channel]; data.data[i] = data.data[i+1] = data.data[i+2] = value; data.data[i+3] = 255; }
      context.putImageData(data, 0, 0);
    }
    return canvas;
  }); drawTexture();
}
function drawTexture() {
  const canvas = $('textureCanvas'); if (!canvas || $('texturePanel').hidden || $('textureCompare').hidden) return;
  const w = $('textureViewport').clientWidth, h = $('textureViewport').clientHeight, dpr = devicePixelRatio;
  if (!w || !h) return;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  const context = canvas.getContext('2d'); context.scale(dpr, dpr); context.imageSmoothingEnabled = false;
  for (let y = 0; y < h; y += 16) for (let x = 0; x < w; x += 16) { context.fillStyle = ((x+y)/16)%2 ? '#242a33' : '#303742'; context.fillRect(x,y,16,16); }
  if (!state.texture) return;
  // Both sides cover the same UV region: A defines the reference size, and B pixels map as
  // B_px = A_px * (B.w / A.w), so B draws at A's on-screen size (extra detail shows when zoomed).
  // "B at native 1:1" draws B at its own pixel size around the same UV center instead.
  const ref = state.channelCanvases?.[0] || state.channelCanvases?.[1];
  const refW = ref ? (state.channelCanvases[0] ? ref.width : state.texture.w) : state.texture.w;
  const refH = ref ? (state.channelCanvases[0] ? ref.height : state.texture.h) : state.texture.h;
  const nearest = state.zoom >= 4, native = $('nativeB')?.checked;
  context.imageSmoothingEnabled = !nearest; context.imageSmoothingQuality = 'high';
  const rects = [];
  (state.channelCanvases || []).forEach((image, i) => {
    if (!image || !state.images[i]) return;
    const sx = i && native ? image.width / refW : 1, sy = i && native ? image.height / refH : 1;
    const dw = refW * state.zoom * sx, dh = refH * state.zoom * sy;
    const x = (w-dw)/2 + state.pan.x, y = (h-dh)/2 + state.pan.y;
    context.save(); context.beginPath(); context.rect(i ? w*state.texSplit : 0, 0, i ? w*(1-state.texSplit) : w*state.texSplit, h); context.clip();
    context.drawImage(image, x, y, dw, dh); context.restore(); rects[i] = { x, y, w: dw, h: dh, srcW: image.width };
  });
  state.lastDraw = { rects, refW, refH, nearest, native: !!native };
}
function divider(id, key, redraw) {
  const element = $(id);
  function set(value) { state[key] = Math.max(.02, Math.min(.98, value)); element.style.left = `${state[key]*100}%`; element.setAttribute('aria-valuenow', Math.round(state[key]*100)); redraw?.(); }
  element.onpointerdown = event => { event.stopPropagation(); element.setPointerCapture(event.pointerId); };
  element.onpointermove = event => { if (!element.hasPointerCapture(event.pointerId)) return; const rect = element.parentElement.getBoundingClientRect(); set((event.clientX - rect.left)/rect.width); };
  element.onkeydown = event => { if (['ArrowLeft','ArrowRight','Home','End'].includes(event.key)) { event.preventDefault(); set(event.key === 'Home' ? .02 : event.key === 'End' ? .98 : state[key] + (event.key === 'ArrowLeft' ? -.02 : .02)); } };
}
divider('modelDivider','split'); divider('textureDivider','texSplit',drawTexture);
let lastPointer;
$('textureCanvas').onpointerdown = event => { lastPointer = { x: event.clientX, y: event.clientY }; event.target.setPointerCapture(event.pointerId); };
$('textureCanvas').onpointermove = event => {
  if (!event.target.hasPointerCapture(event.pointerId)) return;
  state.pan.x += event.clientX-lastPointer.x; state.pan.y += event.clientY-lastPointer.y; lastPointer = { x:event.clientX,y:event.clientY }; drawTexture();
};
async function variantsChanged() { updateLabels(); await Promise.all([loadModel(), loadTextureCompare()]); }
$('variantA').onchange = $('variantB').onchange = variantsChanged;
$('swap').onclick = () => { const a=$('variantA').value; $('variantA').value=$('variantB').value; $('variantB').value=a; variantsChanged(); };
$('wireframe').onchange = () => state.models.forEach(root => root.traverse(object => { for (const material of Array.isArray(object.material) ? object.material : [object.material]) if (material) material.wireframe = $('wireframe').checked; }));
$('split').onchange = updateLabels;
$('lod').onchange = () => loadModel();
$('background').onclick = () => { state.background=(state.background+1)%backgrounds.length; const [name,color]=backgrounds[state.background]; renderer?.setClearColor(color); $('background').textContent=`Background: ${name}`; };
$('frame').onclick = frameModel;
$('modelTab').onclick = () => setTab('model'); $('textureTab').onclick = () => setTab('texture');
$('search').oninput = $('category').onchange = listAssets;
$('reload').onclick = refresh;
$('backToGrid').onclick = () => { ++state.textureEpoch; state.texture=null; $('textureCompare').hidden=true; $('textureGrid').hidden=false; };
$('nativeB').onchange = drawTexture;
$('zoom').onchange = () => { state.zoom=Number($('zoom').value); drawTexture(); };
$('resetPan').onclick = () => { state.pan={x:0,y:0}; drawTexture(); };
$('channels').onclick = event => { const channel=event.target.dataset.channel; if (!channel) return; state.channel=channel; for (const button of $('channels').children) button.classList.toggle('active',button.dataset.channel===channel); channelImages(); };
document.addEventListener('keydown', event => {
  if (event.repeat || event.ctrlKey || event.metaKey || event.altKey || /INPUT|SELECT|TEXTAREA/.test(event.target.tagName) || !state.asset) return;
  const key=event.key.toLowerCase();
  if (key==='a'||key==='b') $('swap').click();
  if (key==='w') { $('wireframe').checked=!$('wireframe').checked; $('wireframe').onchange(); }
  if (/^[1-4]$/.test(key)) { const variant=modelChoices()[Number(key)-1]; if (variant) { $('variantB').value=variant.id; variantsChanged(); } }
});
// Read-only diagnostics for reproducible browser checks.
window.viewerDiagnostics = () => ({ modelCount:state.models.length, camera:camera?.position.toArray(), target:controls?.target.toArray(), split:state.split, pan:{...state.pan}, zoom:state.zoom, draw:state.lastDraw, channel:state.channel, materials:state.models.map(root => { const out=[];root.traverse(o=>{if(o.isMesh)for(const m of Array.isArray(o.material)?o.material:[o.material])out.push({vertices:o.geometry.attributes.position.count,name:m.name,map:!!m.map,normal:!!m.normalMap,alphaTest:m.alphaTest,wireframe:m.wireframe,width:m.map?.image.width});});return out;}) });
refresh();
