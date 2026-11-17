// Drive the emulator page in headless Chromium: start the sound, use the controls, read what
// the page shows, and take screenshots in both themes and at phone width.
// usage: node emu_test.js PAGE.html OUTDIR
const { chromium } = require('playwright');

(async () => {
  const page = process.argv[2], out = process.argv[3] || '.';
  const b = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
  let failed = 0;
  const expect = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failed++; };
  for (const scheme of ['light', 'dark']) {
    const p = await b.newPage({ viewport: { width: 1100, height: 1000 }, colorScheme: scheme });
    const errs = [];
    p.on('console', (m) => { if (m.type() === 'error') errs.push(m.text()); });
    p.on('pageerror', (e) => errs.push(e.message));
    await p.goto('file://' + page);
    await p.click('#start');
    await p.waitForTimeout(1500);
    const title = () => p.textContent('#pagename');
    expect((await title()) === 'Home', `${scheme}: boots on Home (${await title()})`);
    if (scheme === 'light') {
      await p.keyboard.press('2'); await p.waitForTimeout(300);
      expect((await title()).startsWith('Channel 2: Bells'), 'T2 opens channel 2 (' + await title() + ')');
      await p.keyboard.down('Shift'); await p.keyboard.press('@'); await p.keyboard.up('Shift'); await p.waitForTimeout(300);
      expect((await title()).endsWith('extended'), '•• + T2: extended (' + await title() + ')');
      await p.keyboard.press('Shift'); await p.waitForTimeout(80); await p.keyboard.press('Shift'); await p.waitForTimeout(300);
      expect((await title()) === 'Home', '•• twice: Home');
      await p.keyboard.down('Shift'); await p.keyboard.press('$'); await p.keyboard.up('Shift'); await p.waitForTimeout(300);
      const chans = (await p.textContent('#chans')).replace(/\s+/g, ' ');
      expect(chans.includes('T4Arp'), '•• + T4 on Home: Flute -> Arp (' + chans + ')');
      console.log('     cpu estimate', await p.textContent('#cpu'), await p.textContent('#cpupill'));
      await p.keyboard.press(']'); await p.waitForTimeout(400);
      expect((await p.textContent('#piece')).includes('frozen'), 'FFWD tap freezes');
      await p.keyboard.press(']'); await p.waitForTimeout(400);
      expect(!(await p.textContent('#piece')).includes('frozen'), 'FFWD tap again lets go');
      // drag F1 (tempo) up from the bottom: it should pick the tempo up and change it
      const tr = await p.$$('.track'); const bb = await tr[0].boundingBox();
      await p.mouse.move(bb.x + 20, bb.y + bb.height * 0.98); await p.mouse.down();
      for (let k = 0; k <= 12; k++) { await p.mouse.move(bb.x + 20, bb.y + bb.height * (0.98 - k * 0.07)); await p.waitForTimeout(40); }
      await p.mouse.up(); await p.waitForTimeout(300);
      const fmap = (await p.textContent('#fmap')).replace(/\s+/g, ' ');
      expect(/^F1Tempo\d+ BPMF2/.test(fmap), 'F1 drag picks up and sets the tempo (' + fmap + ')');
    }
    expect(errs.length === 0, `${scheme}: no console errors ${errs.join(' / ')}`);
    await p.screenshot({ path: `${out}/emu-${scheme}.png`, fullPage: true });
    await p.close();
  }
  const m = await b.newPage({ viewport: { width: 390, height: 900 } });
  await m.goto('file://' + page);
  const [sw, iw] = await m.evaluate(() => [document.documentElement.scrollWidth, window.innerWidth]);
  expect(sw <= iw, `phone: no sideways scroll (${sw} <= ${iw})`);
  await m.screenshot({ path: `${out}/emu-phone.png`, fullPage: true });
  await b.close();
  process.exit(failed ? 1 : 0);
})();
