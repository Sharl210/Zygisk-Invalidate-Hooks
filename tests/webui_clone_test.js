'use strict';

const assert = require('assert');
const fs = require('fs');
const { JSDOM } = require('jsdom');

const html = fs.readFileSync('module/template/webroot/index.html', 'utf8');
const commands = [];
const packageInfo = [
  { packageName: 'com.luna.music', appLabel: 'Luna Music' },
  { packageName: 'com.shared.one', appLabel: 'Shared One' },
  { packageName: 'com.shared.two', appLabel: 'Shared Two' },
];

function waitFor(predicate, label) {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    const timer = setInterval(() => {
      if (predicate()) { clearInterval(timer); resolve(); }
      else if (Date.now() - start > 3000) { clearInterval(timer); reject(new Error(`timeout waiting for ${label}`)); }
    }, 10);
  });
}

function savedConfigs() {
  return commands.filter(command => command.includes('base64 -d >')).map(command => {
    const match = command.match(/printf %s '([^']+)' \| base64 -d/);
    return match ? Buffer.from(match[1], 'base64').toString('utf8') : '';
  });
}

function card(document, packageName, uid) {
  return [...document.querySelectorAll('#configured-app-list .card, #running-app-list .card, #app-list .card')].find(node =>
    node.textContent.includes(packageName) && node.textContent.includes(`UID ${uid}`));
}

(async () => {
  const dom = new JSDOM(html, {
    runScripts: 'dangerously', pretendToBeVisual: true,
    beforeParse(window) {
      window.TextEncoder = TextEncoder;
      window.ksu = {
        exec(command, _opts, callback) {
          commands.push(command);
          let stdout = '';
          if (command.startsWith("cat '/data/adb/modules/inline_hook_spoof/config.txt'")) {
            stdout = 'version=3\nenabled=true\napp=com.luna.music|libart.so|true|11010\napp=com.luna.music|libclone.so|false|1011010\n';
          } else if (command === 'pm list users') {
            stdout = 'Users:\n\tUserInfo{0:Owner:13} running\n\tUserInfo{10:Clone:30} running';
          } else if (command === 'pm list packages -3 -U --user 0') {
            stdout = 'package:com.luna.music uid:11010\npackage:com.shared.one uid:12000\npackage:com.shared.two uid:12000';
          } else if (command === 'pm list packages -3 -U --user 10') {
            stdout = 'package:com.luna.music uid:1011010';
          } else if (command === 'ps -A -o NAME') {
            stdout = '';
          }
          setTimeout(() => window[callback](0, stdout, ''), 0);
        },
        getPackagesInfo() { return JSON.stringify(packageInfo); },
      };
    },
  });
  const { window } = dom;
  const { document } = window;
  await window.onload();
  await waitFor(() => document.querySelectorAll('#configured-app-list .card, #app-list .card').length === 4, 'all user instances loaded');

  const primary = card(document, 'com.luna.music', 11010);
  const clone = card(document, 'com.luna.music', 1011010);
  assert.ok(primary && clone, 'same package must render separate UID instances');
  assert.strictEqual(document.querySelectorAll('#configured-app-list .card').length, 2, 'configured instances must be isolated in the top section');
  assert.match(primary.textContent, /libart\.so · log on/);
  assert.match(clone.textContent, /libclone\.so · log off/);

  // Select one member of a shared UID group: both package rules must be saved.
  card(document, 'com.shared.one', 12000).click();
  await waitFor(() => savedConfigs().some(config =>
    config.includes('app=com.shared.one|libart.so|false|12000') &&
    config.includes('app=com.shared.two|libart.so|false|12000')), 'shared UID group save');
  assert.match(card(document, 'com.shared.one', 12000).className, /selected/);
  assert.match(card(document, 'com.shared.two', 12000).className, /selected/);

  // Removing either group member removes both rules, while same-package clones remain independent.
  card(document, 'com.shared.two', 12000).click();
  await waitFor(() => savedConfigs().some(config =>
    !config.includes('app=com.shared.one|') && !config.includes('app=com.shared.two|') &&
    config.includes('app=com.luna.music|libart.so|true|11010') &&
    config.includes('app=com.luna.music|libclone.so|false|1011010')), 'shared UID group removal');

  dom.window.close();
  console.log('PASS: same-package clones use independent UID rules; shared UID packages link together');
})().catch(error => { console.error(error.stack || error); process.exit(1); });
