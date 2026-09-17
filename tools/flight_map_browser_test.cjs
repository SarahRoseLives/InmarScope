// Manual browser/Windows bridge check: npm install playwright in an isolated
// directory and set NODE_PATH to its node_modules. Tiles are blocked in tests.
const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs'),http=require('node:http'),path=require('node:path');
(async()=>{
  let browser,server;
  try {
    let page;
    if(process.argv[2]==='--native') {
      for(let i=0;i<30;i++) {
        try {browser=await chromium.connectOverCDP('http://127.0.0.1:19322');break;}
        catch {await new Promise(r=>setTimeout(r,250));}
      }
      assert.ok(browser,'native test harness CDP endpoint');
      for(let i=0;i<30;i++) {
        page=browser.contexts().flatMap(c=>c.pages()).find(p=>p.url().startsWith('https://inmarscope.local/'));
        if(page)break;await new Promise(r=>setTimeout(r,250));
      }
      assert.ok(page,'packaged local map');
      // WebView2 enforces CSP in Playwright's injected wait helper; use direct
      // debugger evaluation without weakening the production CSP.
      const session=await page.context().newCDPSession(page);
      let state;
      for(let i=0;i<30;i++) {
        const result=await session.send('Runtime.evaluate',{expression:'JSON.stringify({counts:document.getElementById("counts")?.textContent,markers:document.querySelectorAll(".leaflet-interactive").length,missing:document.querySelectorAll("#missing li").length})',returnByValue:true});
        state=JSON.parse(result.result.value);
        if(state.counts?.includes('2 received'))break;
        await new Promise(r=>setTimeout(r,250));
      }
      assert.match(state.counts,/2 received/);assert.equal(state.markers,1);assert.equal(state.missing,1);
      console.log('PASS: actual native AircraftTable -> WebView2 -> marker; identity without position remains listed');
    } else {
      const root=path.resolve('assets/flight-map');
      server=http.createServer((req,res)=>{
        const file=path.resolve(root,'.'+(req.url==='/'?'/index.html':req.url));
        if(!file.startsWith(root+path.sep))return res.writeHead(403).end();
        try {res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html');res.end(fs.readFileSync(file));}
        catch {res.writeHead(404).end();}
      });
      await new Promise(r=>server.listen(0,'127.0.0.1',r));
      browser=await chromium.launch({headless:true});page=await browser.newPage();
      const errors=[];page.on('pageerror',e=>errors.push(e.message));
      const external=[];await page.route('https://**/*',r=>{external.push(r.request().url());return r.abort();});
      await page.goto(`http://127.0.0.1:${server.address().port}/`);
      await page.waitForFunction(()=>typeof window.updateAircraft==='function');
      const view=()=>page.evaluate(()=>[map.getCenter().lat,map.getCenter().lng,map.getZoom()]);
      const before=await view();
      assert.equal(await page.locator('.leaflet-interactive').count(),0);
      const data=JSON.parse(fs.readFileSync(process.argv[2]||'build/flight-map-fixture.json','utf8'));
      await page.evaluate(d=>window.updateAircraft(d),data);
      assert.equal(await page.locator('.leaflet-interactive').count(),2);
      const colors=await page.locator('.leaflet-interactive').evaluateAll(nodes=>nodes.map(n=>n.getAttribute('fill')));
      assert.deepEqual(colors.sort(),['#27c5ff','#ffb347'].sort());
      assert.deepEqual(await view(),before);
      await page.locator('#fit').click();await page.waitForTimeout(500);
      const chosen=await view();data[0].lat-=1;
      await page.evaluate(d=>window.updateAircraft(d),data);assert.deepEqual(await view(),chosen);
      await page.evaluate(()=>window.updateAircraft([]));assert.equal(await page.locator('.leaflet-interactive').count(),0);
      assert.deepEqual(errors,[]);assert.ok(external.every(u=>u.startsWith('https://tile.openstreetmap.org/')));
      console.log('PASS: native fixture rendered in real browser; decoded/online source colors; unchanged viewport; clear; no browser traffic queries');
    }
  } finally {if(browser)await browser.close();if(server)server.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
