'use strict';

const assert = require('assert');
const fs = require('fs');
const { JSDOM } = require('jsdom');
const html = fs.readFileSync('module/template/webroot/index.html', 'utf8');
const commands = [];

function waitFor(predicate, label) {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    const timer = setInterval(() => {
      if (predicate()) { clearInterval(timer); resolve(); }
      else if (Date.now() - start > 3000) { clearInterval(timer); reject(new Error(`timeout waiting for ${label}`)); }
    }, 10);
  });
}

(async () => {
  const dom = new JSDOM(html, {
    runScripts: 'dangerously', pretendToBeVisual: true,
    beforeParse(window) {
      window.TextEncoder = TextEncoder;
      window.confirm = () => true;
      window.ksu = {
        exec(command, _options, callback) {
          commands.push(command);
          let stdout = '';
          if (command.startsWith("cat '/data/adb/modules/inline_hook_spoof/config.txt'")) {
            stdout = 'version=3\nenabled=true\napp=com.alpha.notes|libart.so|true|11010\n';
          } else if (command === 'pm list users') {
            stdout = 'Users:\n\tUserInfo{0:Owner:13} running';
          } else if (command === 'pm list packages -3 -U --user 0') {
            stdout = 'package:com.alpha.notes uid:11010';
          } else if (command === 'ps -A -o NAME') {
            stdout = '1234 11010 com.alpha.notes';
          } else if (command.includes('/proc/') && command.includes('iflag=skip_bytes,count_bytes')) {
            stdout = 'RESULT|/data/adb/modules/inline_hook_spoof/maps/com.alpha.notes_11010/20260723123456_libart.so_0000000000400000.bin|4096|4012|0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef|equal|\n';
          }
          setTimeout(() => window[callback](0, stdout, ''), 0);
        },
        getPackagesInfo() { return JSON.stringify([{ packageName: 'com.alpha.notes', appLabel: 'Alpha Notes' }]); },
      };
    },
  });
  const { window } = dom;
  const { document } = window;
  await window.onload();
  await waitFor(() => document.querySelector('#configured-app-list .card'), 'configured instance');
  const configure = [...document.querySelector('#configured-app-list .card').querySelectorAll('button')]
    .find(button => button.textContent === 'Configure');
  configure.click();
  document.getElementById('per-app-diagnostics').click();
  await waitFor(() => typeof window.performDump === 'function', 'maps export override');
  await window.performDump({
    range: '0000000000400000-0000000000401000', perms: 'r-xp', offset: '00000000', path: '/apex/com.android.art/lib64/libart.so',
  });
  const exportCommand = commands.find(command => command.includes('iflag=skip_bytes,count_bytes'));
  assert.ok(exportCommand, 'maps export must use an exact byte-addressed root command');
  assert.match(exportCommand, /maps\/com\.alpha\.notes_11010/);
  assert.match(exportCommand, /NONZERO=/);
  assert.match(exportCommand, /sha256sum/);
  assert.match(exportCommand, /DISK_COMPARE=/);
  assert.match(exportCommand, /cmp -s/);
  assert.match(exportCommand, /FIRST_MEMORY_DIFFERENCE_ADDRESS=/);
  assert.match(exportCommand, /\.meta/);
  assert.match(document.getElementById('per-app-status').textContent, /Map export saved/);
  dom.window.close();
  console.log('PASS: maps export targets package UID module directory and validates content');
})().catch(error => { console.error(error.stack || error); process.exit(1); });
