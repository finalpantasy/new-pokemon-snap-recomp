import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { cp, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
const root = fileURLToPath(new URL('../', import.meta.url));
const port = Number(process.env.VIEWER_TEST_PORT || 18765);
const server = spawn('python', ['-B','-E','-P', 'serve.py','--cache','testdata/cache','--port',String(port)], {cwd:root,windowsHide:true,stdio:['ignore','pipe','pipe']});
let browser;
const errors=[], checks=[];
const base=`http://127.0.0.1:${port}`;
async function ready(page) { await page.waitForFunction(()=>document.querySelector('#viewport').dataset.ready==='true'); }
try {
  await Promise.race([once(server.stdout,'data'), once(server,'exit').then(()=>{throw new Error('Server exited');}),new Promise((_,reject)=>setTimeout(()=>reject(new Error('Server startup timeout')),10000))]);
  browser=await chromium.launch({channel:'msedge',headless:true});
  const context=await browser.newContext({viewport:{width:1440,height:1000},deviceScaleFactor:1});
  const page=await context.newPage();
  page.on('response',response=>{if(response.status()>=400)console.error('HTTP',response.status(),response.url());});
  page.on('pageerror',e=>errors.push(e.message));
  page.on('console',msg=>{if(msg.type()==='error')errors.push(msg.text());});
  // Runtime requests must stay on localhost; this also demonstrates offline operation.
  await context.route('**/*',route=>new URL(route.request().url()).origin===base?route.continue():route.abort());
  await page.goto(base); await ready(page);
  assert.equal(await page.locator('.asset-row').count(),2); checks.push('manifest, list, and local module loading');
  let d=await page.evaluate(()=>viewerDiagnostics());
  assert.equal(d.modelCount,2); assert(d.materials.every(ms=>ms.every(m=>m.map)));
  assert(d.materials.every(ms=>ms.some(m=>m.normal)));
  assert(d.materials.every(ms=>ms.some(m=>m.alphaTest===.5)));
  assert(d.materials[0].some(m=>m.width===64)); assert(d.materials[1].some(m=>m.width===256)); checks.push('both model variants, normal maps, and alpha test');
  await page.locator('#viewport').screenshot({path:root+'docs/model-canvas.png'});
  await page.screenshot({path:root+'docs/3d-ab.png'});
  await page.locator('#split').uncheck(); assert(await page.locator('#modelDivider').isHidden());
  await page.locator('#split').check();
  await page.locator('#modelDivider').focus(); await page.keyboard.press('ArrowRight');
  assert((await page.evaluate(()=>viewerDiagnostics().split))>.5);
  const viewport=await page.locator('#viewport').boundingBox();
  const camera=d.camera;
  await page.mouse.move(viewport.x+viewport.width*.2,viewport.y+viewport.height*.4);
  await page.mouse.wheel(0,200);
  await page.waitForFunction(old=>JSON.stringify(viewerDiagnostics().camera)!==JSON.stringify(old),camera);
  const beforePan=await page.evaluate(()=>viewerDiagnostics().target);
  await page.mouse.down({button:'right'}); await page.mouse.move(viewport.x+viewport.width*.25,viewport.y+viewport.height*.4,{steps:6}); await page.mouse.up({button:'right'});
  await page.waitForFunction(old=>JSON.stringify(viewerDiagnostics().target)!==JSON.stringify(old),beforePan);
  await page.mouse.move(viewport.x+viewport.width*.3,viewport.y+viewport.height*.4); await page.mouse.down(); await page.mouse.move(viewport.x+viewport.width*.4,viewport.y+viewport.height*.5,{steps:10}); await page.mouse.up();
  await page.waitForFunction(old=>JSON.stringify(viewerDiagnostics().camera)!==JSON.stringify(old),camera);
  checks.push('shared orbit camera and keyboard-accessible split divider');
  await page.locator('#wireframe').check(); assert((await page.evaluate(()=>viewerDiagnostics().materials)).flat().every(m=>m.wireframe));
  await page.locator('#wireframe').uncheck();
  await page.locator('#lod').selectOption('1'); await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('LOD 1')); assert((await page.evaluate(()=>viewerDiagnostics().materials[0][0].vertices))<d.materials[0][0].vertices); checks.push('LOD geometry changes and wireframe');
  await page.locator('#background').click(); assert.match(await page.locator('#background').textContent(),/light/);
  await page.locator('#variantB').selectOption('x4_warm'); await page.waitForFunction(()=>document.querySelector('#labelB').textContent.includes('Warm'));
  await page.locator('#modelTab').focus(); await page.keyboard.press('a'); assert.equal(await page.locator('#variantA').inputValue(),'x4_warm');
  await page.keyboard.press('b'); assert.equal(await page.locator('#variantA').inputValue(),'orig');
  await page.keyboard.press('2'); assert.equal(await page.locator('#variantB').inputValue(),'x4_detail');
  // Wait for all asynchronous variant swaps before opening the texture inspector.
  await page.waitForFunction(()=>viewerDiagnostics().materials[1]?.some(m=>m.width===256));
  // Alternate model: original (game) on A, alt on B, counts shown per side.
  await page.locator('#variantA').selectOption('orig');
  await page.locator('#variantB').selectOption('alt:ext_demo');
  await page.waitForFunction(()=>viewerDiagnostics().materials[1]?.some(m=>m.name==='alt_body')&&document.querySelector('#labelB').textContent.includes('tris'));
  const la=await page.locator('#labelA').textContent(), lb=await page.locator('#labelB').textContent();
  assert.match(la,/Original \(game\).*tris.*verts/); assert.match(lb,/Alt .*Synthetic.*tris.*verts/); assert.notEqual(la.split('·').slice(1).join(),lb.split('·').slice(1).join());
  const am=await page.evaluate(()=>viewerDiagnostics().materials); assert(am[1].some(m=>m.map&&m.name==='alt_body'));
  await page.locator('#viewport').screenshot({path:root+'docs/model-canvas-alt.png'});
  await page.screenshot({path:root+'docs/3d-ab-alt.png'});
  await page.locator('#variantB').selectOption('x4_detail'); await page.waitForFunction(()=>document.querySelector('#labelB').textContent.includes('HD'));
  checks.push('alternate model A/B against the original with triangle and vertex counts');
  await page.locator('#textureTab').click(); assert.equal(await page.locator('.texture-card').count(),3);
  await page.locator('.texture-card').first().click(); await page.waitForFunction(()=>document.querySelector('#textureViewport').dataset.ready==='true');
  assert.match(await page.locator('#textureSizes').textContent(),/64 × 64.*256 × 256/);
  await page.locator('#zoom').selectOption('8');
  assert.equal(await page.evaluate(()=>viewerDiagnostics().zoom),8);
  await page.screenshot({path:root+'docs/texture-slider.png'});
  for(const channel of ['R','G','B','A','RGB']) {await page.locator(`[data-channel="${channel}"]`).click(); assert.equal(await page.evaluate(()=>viewerDiagnostics().channel),channel);}
  await page.locator('#textureDivider').focus(); await page.keyboard.press('ArrowLeft');
  const tc=await page.locator('#textureCanvas').boundingBox(); await page.mouse.move(tc.x+80,tc.y+100);await page.mouse.down();await page.mouse.move(tc.x+120,tc.y+120);await page.mouse.up();
  assert.equal(await page.evaluate(()=>viewerDiagnostics().pan.x),40);
  await page.locator('#zoom').selectOption('16'); assert.equal(await page.evaluate(()=>viewerDiagnostics().zoom),16);
  // Mismatched sizes (A 64², B 256²): both sides must cover the same UV region.
  await page.locator('#zoom').selectOption('1');
  await page.waitForFunction(()=>viewerDiagnostics().draw?.rects?.length===2);
  let dr=await page.evaluate(()=>viewerDiagnostics().draw);
  assert.deepEqual(dr.rects[0].srcW,64); assert.equal(dr.rects[1].srcW,256);
  for(const k of ['x','y','w','h']) assert.equal(dr.rects[0][k],dr.rects[1][k]);
  assert.equal(dr.nearest,false);
  await page.locator('#zoom').selectOption('4'); dr=await page.evaluate(()=>viewerDiagnostics().draw);
  assert.equal(dr.nearest,true); assert.equal(dr.rects[1].w,dr.rects[0].w);
  await page.locator('#nativeB').check(); dr=await page.evaluate(()=>viewerDiagnostics().draw);
  assert.equal(dr.rects[1].w,dr.rects[0].w*4);
  assert.equal(dr.rects[1].x+dr.rects[1].w/2,dr.rects[0].x+dr.rects[0].w/2);
  await page.locator('#nativeB').uncheck();
  checks.push('texture A/B with mismatched sizes shares one UV region; native 1:1 toggle; nearest at 4x');
  checks.push('texture sizes, 1–16x zoom, RGB/R/G/B/A, synced pan, and divider');
  await page.locator('#search').fill('no match'); assert.equal(await page.locator('.asset-row').count(),0);
  await page.locator('#search').fill(''); await page.locator('#category').selectOption('ui'); assert.equal(await page.locator('.asset-row').count(),1);
  await page.locator('.asset-row').click(); await page.waitForFunction(()=>document.querySelector('#modelMessage').textContent.includes('No model'));
  checks.push('search, category filter, and texture-only assets');
  await page.setViewportSize({width:390,height:844});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)); checks.push('390px layout without horizontal overflow');
  assert.equal((await fetch(base+'/cache/%2e%2e%2fserve.py')).status,403);
  assert.equal((await fetch(base+'/cache/C%3A/test')).status,403);
  assert.equal((await fetch(base+'/serve.py')).status,404);
  checks.push('traversal and private app file rejection');
  const manifest=await (await fetch(base+'/api/manifest')).json();
  const vessel=manifest.assets.find(a=>a.id==='contour_vessel');
  assert.equal(vessel.alt_models.length,1); assert.equal(vessel.alt_models[0].url,'/external/contour_vessel/alt/model.gltf');
  assert.equal(manifest.assets.filter(a=>a.alt_models).length,1);
  assert.equal((await fetch(base+'/external/contour_vessel/alt/model.gltf')).status,200);
  assert.equal((await fetch(base+'/external/%2e%2e%2fcache%2fmanifest.json')).status,403);
  assert.equal((await fetch(base+'/external/x',{method:'POST'})).status,501);
  checks.push('server alt-model merge (id, then case-insensitive name), read-only /external/');
  // Missing alt_models.json and an empty cache must both degrade gracefully.
  const tmp=await mkdtemp(join(tmpdir(),'viewer-'));
  const extra=[];
  try {
    await cp(root+'testdata/cache',join(tmp,'cache'),{recursive:true}); await rm(join(tmp,'cache','alt_models.json'));
    for (const [dir,portOffset] of [[join(tmp,'cache'),1],[tmp,2]]) {
      const srv=spawn('python',['-B','-E','-P','serve.py','--cache',dir,'--port',String(port+portOffset)],{cwd:root,windowsHide:true,stdio:['ignore','pipe','pipe']});
      extra.push(srv); await once(srv.stdout,'data');
      const m=await (await fetch(`http://127.0.0.1:${port+portOffset}/api/manifest`)).json();
      if (portOffset===1) { assert.equal(m.assets.length,2); assert(m.assets.every(a=>!a.alt_models)); }
      else { assert.equal(m.assets.length,0); assert.match(m.build_command,/build_cache\.py/); }
    }
    const empty=await browser.newPage(); await empty.goto(`http://127.0.0.1:${port+2}`);
    await empty.waitForSelector('#empty:not([hidden])'); assert.match(await empty.locator('#empty code').textContent(),/python -E -P scripts\\hd\\build_cache\.py/);
    await empty.close();
  } finally { extra.forEach(x=>x.kill()); await rm(tmp,{recursive:true,force:true}); }
  checks.push('missing alt_models.json and empty cache handled with the build command message');
  // A sparse manifest must visibly fall back, rather than pretending the upscale loaded.
  const originalManifest=JSON.parse(await readFile(root+'testdata/cache/manifest.json','utf8'));
  const partial=structuredClone(originalManifest);
  delete partial.assets[0].textures[0].variants.x4_detail;
  partial.assets=[partial.assets[0]];
  await page.route('**/api/manifest',route=>route.fulfill({json:partial}));
  await page.setViewportSize({width:1440,height:1000});
  await page.locator('#reload').click();
  await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('using original'));
  assert((await page.evaluate(()=>viewerDiagnostics().materials[1])).some(m=>m.name==='ceramic'&&m.width===64));
  checks.push('partial variant fallback with a visible notice');
  await page.unroute('**/api/manifest');
  await page.route('**/api/manifest',route=>route.fulfill({json:{version:1,assets:[],variants:[],message:'Cache unavailable: synthetic missing cache'}}));
  await page.locator('#reload').click(); await page.locator('#empty').waitFor({state:'visible'});
  assert.match(await page.locator('#empty code').textContent(),/python -E -P scripts\\hd\\build_cache.py/);
  assert.match(await page.locator('#emptyMessage').textContent(),/Cache unavailable/);
  checks.push('rendered missing-cache message and pipeline command');
  assert.deepEqual(errors,[]);
  checks.push('zero browser console errors and page exceptions');
  await writeFile(root+'docs/verification.json',JSON.stringify({browser:'Headless installed Edge via Playwright',viewport:'1440 × 1000; 390 × 844',checks,errors},null,2));
  console.log(JSON.stringify({checks,errors},null,2));
} finally {
  await browser?.close();
  server.kill();
  if(server.exitCode===null) await once(server,'exit');
}

