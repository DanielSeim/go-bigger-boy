import {defineConfig} from '@playwright/test';

export default defineConfig({
  testDir: '.',
  timeout: 180_000,
  expect: {timeout: 20_000},
  reporter: 'line',
  use: {
    baseURL: process.env.GBB_WEB_BASE_URL || 'http://127.0.0.1:8765',
    browserName: 'chromium',
    headless: true,
  },
});
