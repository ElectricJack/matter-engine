import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const browser = process.env.MATTER_AUTHORING_BROWSER ?? 'google-chrome';
const profile = mkdtempSync(join(tmpdir(), 'matter-authoring-browser-'));
try {
  const result = spawnSync(browser, ['--headless', '--no-sandbox', '--disable-gpu', '--disable-background-networking',
    '--no-first-run', '--no-default-browser-check', '--host-resolver-rules=MAP * ~NOTFOUND',
    '--allow-file-access-from-files', `--user-data-dir=${profile}`, '--dump-dom', '--virtual-time-budget=5000',
    new URL('./browser.html', import.meta.url).href], { encoding: 'utf8', timeout: 30000, maxBuffer: 4 * 1024 * 1024 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /<body data-status="pass">/, result.stdout + result.stderr);
  const report = JSON.parse(result.stdout.match(/<pre id="result">([^<]+)<\/pre>/)[1]);
  assert.deepEqual(report, { fixtures: 3, rngWords: 32, rngUnits: 32, diagnostics: 1 });
  console.log(`PASS SDK-BROWSER-v1: ${JSON.stringify(report)}`);
} finally { rmSync(profile, { recursive: true, force: true }); }
