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
