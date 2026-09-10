import {expect, test} from '@playwright/test';

function makeLoopingRom() {
  // A valid 32 KiB MBC1 cartridge with battery RAM and a deterministic loop
  // at the entry point. This keeps the browser flow independent of a
  // copyrighted game dump while still exercising the real ROM picker and
  // emulator startup path.
  const rom = new Uint8Array(0x8000);
  rom.set([0x00, 0xc3, 0x00, 0x01], 0x100); // NOP; JP $0100
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
    buffer: makeLoopingRom(),
  });
  await expect(page.locator('#status')).toContainText('ROM loaded', {
    timeout: 30_000,
  });
  await expect(page.locator('#save-actions')).toBeVisible();
  await expect(page).toHaveTitle(/gbb-browser-e2e\.gb/);

  expect(pageErrors, pageErrors.map(error => error.stack).join('\n'))
    .toEqual([]);
});
