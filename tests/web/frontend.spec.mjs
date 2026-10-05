import {expect, test} from '@playwright/test';
import {Buffer} from 'node:buffer';
import {mkdir} from 'node:fs/promises';
import path from 'node:path';

function makeLoopingRom() {
  // A valid 32 KiB MBC1 cartridge with battery RAM and a deterministic loop
  // at the entry point. This keeps the browser flow independent of a
  // copyrighted game dump while still exercising the real ROM picker and
  // emulator startup path.
  const rom = new Uint8Array(0x8000);
  // Turn the LCD off, draw a deterministic striped tile into VRAM, fill the
  // background map with it, then turn the LCD back on. This gives the voxel
  // captures real geometry while remaining a tiny, redistribution-safe ROM.
  const program = [
    0xf3, 0xaf, 0xe0, 0x40, // DI; XOR A; LCDC = 0
    0x21, 0x00, 0x80,        // HL = tile 0
    0x3e, 0xff, 0x22, 0x3e, 0x00, 0x22,
    0x3e, 0x00, 0x22, 0x3e, 0xff, 0x22,
    0x3e, 0xff, 0x22, 0x3e, 0x00, 0x22,
    0x3e, 0x00, 0x22, 0x3e, 0xff, 0x22,
    0x3e, 0xff, 0x22, 0x3e, 0x00, 0x22,
    0x3e, 0x00, 0x22, 0x3e, 0xff, 0x22,
    0x3e, 0xff, 0x22, 0x3e, 0x00, 0x22,
    0x3e, 0x00, 0x22, 0x3e, 0xff, 0x22,
    0x3e, 0xe4, 0xe0, 0x47, // BGP = all four shades
    0xaf, 0xe0, 0x42, 0xe0, 0x43, // SCY/SCX = 0
    0x21, 0x00, 0x98, 0x01, 0x00, 0x04,
    0xaf, 0x22, 0x0b, 0x78, 0xb1, 0x20, 0xf9, // fill 0x400 map bytes with 0
    0x3e, 0x91, 0xe0, 0x40, // LCDC = BG on, tile data 0x8000
    0x18, 0xfe, // loop forever
  ];
  rom.set(program, 0x100);
  const title = new TextEncoder().encode('GBB E2E');
  rom.set(title, 0x134);
  rom[0x147] = 0x03; // MBC1 + RAM + battery
  rom[0x148] = 0x00; // 32 KiB
  rom[0x149] = 0x02; // 8 KiB external RAM
  let checksum = 0;
  for (let index = 0x134; index <= 0x14c; ++index) {
    checksum = (checksum - rom[index] - 1) & 0xff;
  }
  rom[0x14d] = checksum;
  return rom;
}

test('loads a ROM and persists the primary display settings', async ({page}) => {
  const pageErrors = [];
  page.on('pageerror', error => pageErrors.push(error));

  await page.goto('/');
  await expect(page.locator('#status')).toHaveText(
    'Ready. Choose a Game Boy ROM to begin.', {timeout: 90_000});
  await expect(page.locator('#open-rom')).toBeEnabled();
  await expect(page.locator('#display-palette')).toBeEnabled();
  await expect(page.locator('#video-mode')).toBeEnabled();
  await expect(page.locator('#hardware-model')).toBeEnabled();
  await expect(page.locator('#startup-mode')).toBeEnabled();
  await expect(page.locator('#startup-mode')).toHaveValue('instant');
  await page.locator('#startup-mode').selectOption('replacement-dmg');
  await expect(await page.evaluate(() => localStorage.getItem('gbb-startup-mode'))).toBe('replacement-dmg');

  await page.locator('#audio-enabled').uncheck();
  await page.locator('#display-palette').selectOption('3');
  await page.locator('#video-mode').selectOption('2');
  await page.locator('#hardware-model').selectOption('cgb-e');
  await expect(await page.evaluate(() => ({
    audio: localStorage.getItem('gbb-audio-enabled'),
    palette: localStorage.getItem('gbb-display-palette'),
    video: localStorage.getItem('gbb-video-mode'),
    hardware: localStorage.getItem('gbb-hardware-model'),
  }))).toEqual({audio: 'false', palette: '3', video: '2', hardware: 'cgb-e'});

  await page.reload();
  await expect(page.locator('#status')).toHaveText(
    'Ready. Choose a Game Boy ROM to begin.', {timeout: 90_000});
  await expect(page.locator('#audio-enabled')).not.toBeChecked();
  await expect(page.locator('#startup-mode')).toHaveValue('replacement-dmg');
  await expect(page.locator('#display-palette')).toHaveValue('3');
  await expect(page.locator('#video-mode')).toHaveValue('2');
  await expect(page.locator('#hardware-model')).toHaveValue('cgb-e');

  await page.locator('#rom-file').setInputFiles({
    name: 'gbb-browser-e2e.gb',
    mimeType: 'application/octet-stream',
    buffer: Buffer.from(makeLoopingRom()),
  });
  await expect(page.locator('#status')).toContainText('ROM loaded', {
    timeout: 30_000,
  });
  await expect(page.locator('#save-actions')).toBeVisible();
  await expect(page).toHaveTitle(/gbb-browser-e2e\.gb/);

  const captureDirectory = process.env.GBB_WEB_CAPTURE_DIR ||
    'web-visual-captures';
  await mkdir(captureDirectory, {recursive: true});
  for (const [mode, name] of [[5, 'voxel'], [6, 'voxel_shape'],
                              [7, 'voxel_popup']]) {
    await page.locator('#video-mode').selectOption(String(mode));
    await page.waitForTimeout(350);
    const capture = await page.locator('#canvas').screenshot({
      path: path.join(captureDirectory, `${name}.png`),
      animations: 'disabled',
    });
    expect(capture.length).toBeGreaterThan(1000);
  }

  expect(pageErrors, pageErrors.map(error => error.stack).join('\n'))
    .toEqual([]);
});

test('bundled DMG boot reaches a logo-free cartridge and unknown preferences fall back safely', async ({page}) => {
  await page.goto('/');
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await page.evaluate(() => localStorage.setItem('gbb-startup-mode', 'unknown'));
  await page.reload();
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await expect(page.locator('#startup-mode')).toHaveValue('instant');
  await page.locator('#audio-enabled').uncheck();
  await page.locator('#hardware-model').selectOption('dmg');
  await page.locator('#startup-mode').selectOption('replacement-dmg');

  const rom = new Uint8Array(0x8000);
  // JP 0150; cartridge writes a unique scroll marker after boot handoff.
  rom.set([0xc3, 0x50, 0x01], 0x100);
  rom.set([0x3e, 0x2a, 0xe0, 0x43, 0x18, 0xfe], 0x150);
  let checksum = 0;
  for (let index = 0x134; index <= 0x14c; ++index) checksum = (checksum - rom[index] - 1) & 0xff;
  rom[0x14d] = checksum;
  await page.locator('#rom-file').setInputFiles({
    name: 'gbb-logo-free-boot.gb', mimeType: 'application/octet-stream', buffer: Buffer.from(rom),
  });
  await expect(page.locator('#status')).toContainText('ROM loaded');
  await expect.poll(async () => page.evaluate(() => {
    const pointer = Module._gbb_export_scene_snapshot();
    return JSON.parse(UTF8ToString(pointer)).scx;
  }), {timeout: 30_000}).toBe(42);
  // Preference changes do not restart or replace the running cartridge.
  await page.locator('#startup-mode').selectOption('instant');
  await expect(await page.evaluate(() => {
    return JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).scx;
  })).toBe(42);
  await expect(await page.evaluate(() => localStorage.getItem('gbb-startup-mode'))).toBe('instant');
});

test('animated DMG splash stays flat with voxel selected and hands off', async ({page}) => {
  await page.goto('/');
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await page.locator('#audio-enabled').uncheck();
  await page.locator('#hardware-model').selectOption('dmg');
  await page.locator('#startup-mode').selectOption('animated-dmg');
  await page.locator('#video-mode').selectOption('5');
  const rom = new Uint8Array(0x8000);
  rom.set([0xc3, 0x50, 0x01], 0x100);
  rom.set([0x3e, 0x2a, 0xe0, 0x43, 0x18, 0xfe], 0x150);
  let sum = 0;
  for (let i = 0x134; i <= 0x14c; ++i) sum = (sum - rom[i] - 1) & 0xff;
  rom[0x14d] = sum;
  await page.locator('#rom-file').setInputFiles({
    name: 'gbb-animated-boot.gb', mimeType: 'application/octet-stream', buffer: Buffer.from(rom),
  });
  // Pause at a known emulated time rather than relying on screenshot wall time.
  await expect.poll(async () => page.evaluate(() => {
    const state = JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot()));
    if (state.emulation_cycles >= 18000000 && state.emulation_cycles < 23000000) {
      Module._gbb_pause_rom();
      return true;
    }
    return false;
  }), {intervals: [10], timeout: 30000}).toBe(true);
  const directory = process.env.GBB_WEB_CAPTURE_DIR || 'test-results/web-captures';
  await mkdir(directory, {recursive: true});
  await page.locator('#canvas').screenshot({path: path.join(directory, 'animated-boot.png')});
  await page.evaluate(() => Module._gbb_start_rom());
  await expect.poll(async () => page.evaluate(() =>
    JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).scx
  ), {timeout: 30000}).toBe(42);
  await expect(page.locator('#startup-mode')).toHaveValue('animated-dmg');
  await expect(page.locator('#video-mode')).toHaveValue('5');
  expect(await page.evaluate(() => localStorage.getItem('gbb-startup-mode'))).toBe('animated-dmg');
});

test('Pocket fast and animated boot expose FF to a dual-mode cartridge', async ({page}) => {
  await page.goto('/');
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await page.locator('#audio-enabled').uncheck();
  await page.locator('#hardware-model').selectOption('mgb');
  const rom = new Uint8Array(0x8000);
  rom.set([0xc3, 0x50, 0x01], 0x100);
  // Write the untouched handoff A into SCX, then loop forever.
  rom.set([0xe0, 0x43, 0x18, 0xfe], 0x150);
  rom[0x143] = 0x80;
  let sum = 0;
  for (let i = 0x134; i <= 0x14c; ++i) sum = (sum - rom[i] - 1) & 0xff;
  rom[0x14d] = sum;
  for (const mode of ['replacement-dmg', 'animated-dmg']) {
    await page.locator('#startup-mode').selectOption(mode);
    await page.locator('#rom-file').setInputFiles({
      name: `gbb-pocket-${mode}.gb`, mimeType: 'application/octet-stream', buffer: Buffer.from(rom),
    });
    await expect.poll(async () => page.evaluate(() =>
      JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).scx
    ), {timeout: 30000}).toBe(255);
    await expect(page.locator('#hardware-model')).toHaveValue('mgb');
  }
});

test('DMG0 fast and animated boot expose the early B register', async ({page}) => {
  await page.goto('/');
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await page.locator('#audio-enabled').uncheck();
  await page.locator('#hardware-model').selectOption('dmg0');
  const rom = new Uint8Array(0x8000);
  rom.set([0xc3, 0x50, 0x01], 0x100);
  rom.set([0x78, 0xe0, 0x43, 0x18, 0xfe], 0x150); // LD A,B; publish B in SCX
  let sum = 0;
  for (let i = 0x134; i <= 0x14c; ++i) sum = (sum - rom[i] - 1) & 0xff;
  rom[0x14d] = sum;
  for (const mode of ['replacement-dmg', 'animated-dmg']) {
    await page.locator('#startup-mode').selectOption(mode);
    await page.locator('#rom-file').setInputFiles({
      name: `gbb-dmg0-${mode}.gb`, mimeType: 'application/octet-stream', buffer: Buffer.from(rom),
    });
    await expect.poll(async () => page.evaluate(() =>
      JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).scx
    ), {timeout: 30000}).toBe(255);
    await expect(page.locator('#hardware-model')).toHaveValue('dmg0');
  }
});

test('Color boot fades GBB lettering and hands off native and compatibility registers', async ({page}) => {
  await page.goto('/');
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await page.locator('#audio-enabled').uncheck();
  await page.locator('#hardware-model').selectOption('cgb-e');
  await page.locator('#video-mode').selectOption('5');
  for (const native of [true, false]) {
    const rom = new Uint8Array(0x8000);
    rom.set([0xc3, 0x50, 0x01], 0x100);
    rom.set([0x7b, 0xe0, 0x43, 0x18, 0xfe], 0x150); // Publish handoff E in SCX
    rom[0x143] = native ? 0x80 : 0;
    let sum = 0;
    for (let i = 0x134; i <= 0x14c; ++i) sum = (sum - rom[i] - 1) & 0xff;
    rom[0x14d] = sum;
    for (const mode of ['replacement-dmg', 'animated-dmg']) {
      await page.locator('#startup-mode').selectOption(mode);
      await page.locator('#rom-file').setInputFiles({
        name: `gbb-color-${native}-${mode}.gbc`, mimeType: 'application/octet-stream', buffer: Buffer.from(rom),
      });
      // File selection starts an asynchronous load. Do not mistake the previous
      // cartridge's clock for the newly requested intro's capture window.
      await expect.poll(async () => page.evaluate(() =>
        JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).emulation_cycles
      ), {intervals: [10], timeout: 30000}).toBeLessThan(2000000);
      if (native && mode === 'animated-dmg') {
        await expect.poll(async () => page.evaluate(() => {
          const state = JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot()));
          if (state.emulation_cycles >= 8000000 && state.emulation_cycles < 9500000) {
            Module._gbb_pause_rom();
            return true;
          }
          return false;
        }), {intervals: [10], timeout: 30000}).toBe(true);
        // Let the paused frame reach the canvas before capturing its colors.
        await page.evaluate(() => new Promise(resolve =>
          requestAnimationFrame(() => requestAnimationFrame(resolve))));
        const pausedCycles = await page.evaluate(() =>
          JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).emulation_cycles);
        expect(pausedCycles).toBeGreaterThanOrEqual(8000000);
        expect(pausedCycles).toBeLessThan(9500000);
        const bluePixels = await page.evaluate(() => new Promise(resolve => {
          requestAnimationFrame(() => {
            const source = document.querySelector('#canvas');
            const copy = document.createElement('canvas');
            copy.width = source.width; copy.height = source.height;
            const context = copy.getContext('2d');
            context.drawImage(source, 0, 0);
            const rgba = context.getImageData(0, 0, copy.width, copy.height).data;
            let count = 0;
            for (let i = 0; i < rgba.length; i += 4)
              if (rgba[i] === 0 && rgba[i + 1] === 0 && rgba[i + 2] === 255) ++count;
            resolve(count);
          });
        }));
        expect(bluePixels).toBeGreaterThan(1000);
        const directory = process.env.GBB_WEB_CAPTURE_DIR || path.join(process.cwd(), 'web-captures');
        await mkdir(directory, {recursive: true});
        await page.locator('#canvas').screenshot({path: path.join(directory, 'cgb-animated-boot.png')});
        expect(await page.evaluate(() =>
          JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).emulation_cycles)).toBe(pausedCycles);
        await page.evaluate(() => Module._gbb_start_rom());
      }
      await expect.poll(async () => page.evaluate(() =>
        JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).scx
      ), {timeout: 30000}).toBe(native ? 0x56 : 8);
      await expect(page.locator('#hardware-model')).toHaveValue('cgb-e');
    }
  }
});

test('AGB profiles expose GBA detection registers in all startup modes', async ({page}) => {
  await page.goto('/');
  await expect(page.locator('#startup-mode')).toBeEnabled({timeout: 90_000});
  await page.locator('#audio-enabled').uncheck();
  for (const profile of ['agb0', 'agb']) {
    await page.locator('#hardware-model').selectOption(profile);
    for (const color of [false, true]) {
      const rom = new Uint8Array(0x8000);
      rom.set([0xc3, 0x50, 0x01], 0x100);
      rom.set([0x78, 0xe0, 0x43, 0x18, 0xfe], 0x150); // Publish handoff B in SCX
      rom[0x134] = 0x0f;
      rom[0x143] = color ? 0x80 : 0;
      rom[0x14b] = 1;
      let sum = 0;
      for (let i = 0x134; i <= 0x14c; ++i) sum = (sum - rom[i] - 1) & 0xff;
      rom[0x14d] = sum;
      for (const mode of ['instant', 'replacement-dmg', 'animated-dmg']) {
        await page.locator('#startup-mode').selectOption(mode);
        const name = `gbb-${profile}-${color}-${mode}.gbc`;
        await page.locator('#rom-file').setInputFiles({
          name,
          mimeType: 'application/octet-stream', buffer: Buffer.from(rom),
        });
        await expect(page).toHaveTitle(`${name} — Go Bigger Boy`);
        if (mode !== 'instant') {
          await expect.poll(async () => page.evaluate(() =>
            JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).emulation_cycles
          ), {intervals: [10], timeout: 30000}).toBeLessThan(2000000);
          await expect.poll(async () => page.evaluate(() =>
            JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).emulation_cycles
          ), {timeout: 30000}).toBeGreaterThan(mode === 'animated-dmg' ? 13000000 : 4300000);
        }
        await expect.poll(async () => page.evaluate(() =>
          JSON.parse(UTF8ToString(Module._gbb_export_scene_snapshot())).scx
        ), {timeout: 30000}).toBe(color ? 1 : 0x10);
        await expect(page.locator('#hardware-model')).toHaveValue(profile);
      }
    }
  }
  await page.reload();
  await expect(page.locator('#hardware-model')).toHaveValue('agb', {timeout: 90_000});
});
