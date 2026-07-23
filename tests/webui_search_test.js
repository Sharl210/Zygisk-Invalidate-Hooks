'use strict';

const assert = require('assert');
const fs = require('fs');
const { JSDOM } = require('jsdom');

const html = fs.readFileSync('module/template/webroot/index.html', 'utf8');
const packages = [
  { packageName: 'com.alpha.notes', appLabel: 'Alpha Notes', uid: 11010 },
  { packageName: 'com.beta.camera', appLabel: 'Beta Camera', uid: 11011 },
  { packageName: 'org.gamma.reader', appLabel: 'Gamma Reader', uid: 11012 },
];
const commands = [];

function waitFor(predicate, label) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() - started > 3000) {
        clearInterval(timer);
        reject(new Error(`timeout waiting for ${label}`));
      }
    }, 10);
  });
}

function savedConfigs() {
  return commands
    .filter(command => command.includes("base64 -d >") && command.includes('config.txt'))
    .map(command => {
      const match = command.match(/printf %s '([^']+)' \| base64 -d/);
      assert.ok(match, `could not extract saved config from ${command}`);
      return Buffer.from(match[1], 'base64').toString('utf8');
    });
}

function findAppCard(document, label) {
  return [...document.querySelectorAll('#configured-app-list .card, #running-app-list .card, #app-list .card')]
    .find(card => card.textContent.includes(label));
}

(async () => {
  const dom = new JSDOM(html, {
    runScripts: 'dangerously',
    pretendToBeVisual: true,
    beforeParse(window) {
      window.TextEncoder = TextEncoder;
      window.ksu = {
        exec(command, _options, callbackName) {
          commands.push(command);
          let stdout = '';
          if (command.startsWith("cat '/data/adb/modules/inline_hook_spoof/config.txt'")) {
            stdout = [
              '# Inline Hook Spoof per-application configuration',
              'version=3',
              'enabled=true',
              'app=com.alpha.notes|libart.so,libalpha_extra.so|true|11010',
              '',
            ].join('\n');
          } else if (command === 'pm list users') {
            stdout = 'Users:\n\tUserInfo{0:Owner:13} running';
          } else if (command === 'pm list packages -3 -U --user 0') {
            stdout = packages.map(item => `package:${item.packageName} uid:${item.uid}`).join('\n');
          } else if (command === 'pm list packages -3') {
            stdout = packages.map(item => `package:${item.packageName}`).join('\n');
          } else if (command.startsWith('pm list packages -U ')) {
            const packageName = command.slice('pm list packages -U '.length).replace(/'/g, '');
            const item = packages.find(candidate => candidate.packageName === packageName);
            stdout = item ? `package:${item.packageName} uid:${item.uid}` : '';
          } else if (command === 'ps -A -o NAME') {
            stdout = '';
          }
          setTimeout(() => window[callbackName](0, stdout, ''), 0);
        },
        getPackagesInfo() {
          return JSON.stringify(packages);
        },
      };
    },
  });

  const { window } = dom;
  const { document } = window;
  if (typeof window.onload === 'function') await window.onload();
  await waitFor(() => document.getElementById('app-list').textContent.includes('Beta Camera'), 'per-app app list load');

  // Existing per-app rule must show its library count/status in the original app list.
  assert.match(findAppCard(document, 'Alpha Notes').textContent, /libart\.so, libalpha_extra\.so · log on/);

  // Input and Enter both continue to search the original top-bar list.
  const search = document.getElementById('search-input');
  search.value = 'beta';
  search.dispatchEvent(new window.Event('input', { bubbles: true }));
  assert.match(document.getElementById('app-list').textContent, /Beta Camera/);
  assert.doesNotMatch(document.getElementById('app-list').textContent, /Alpha Notes/);
  search.value = 'gamma';
  search.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
  assert.match(document.getElementById('app-list').textContent, /Gamma Reader/);

  // Clear the filter. Clicking an unconfigured app creates the default libart.so rule.
  search.value = '';
  search.dispatchEvent(new window.Event('input', { bubbles: true }));
  const betaCard = findAppCard(document, 'Beta Camera');
  betaCard.click();
  await waitFor(() => savedConfigs().some(config => config.includes('app=com.beta.camera|libart.so|false|11011')), 'default beta rule save');

  // Configure the selected app with Chinese-comma-separated libraries and logs enabled.
  const betaConfigure = [...findAppCard(document, 'Beta Camera').querySelectorAll('button')]
    .find(button => button.textContent === 'Configure');
  betaConfigure.click();
  assert.ok(document.getElementById('per-app-dialog').classList.contains('active'));
  document.getElementById('per-app-libraries').value = 'libart.so， libbeta_extra.so';
  document.getElementById('per-app-log-enabled').checked = true;
  document.getElementById('per-app-save').click();
  await waitFor(() => savedConfigs().some(config => config.includes('app=com.beta.camera|libart.so,libbeta_extra.so|true|11011')), 'custom beta rule save');

  // Removing the rule writes a config without that package; old logs are not touched by this UI action.
  const configuredBeta = findAppCard(document, 'Beta Camera');
  [...configuredBeta.querySelectorAll('button')].find(button => button.textContent === 'Configure').click();
  document.getElementById('per-app-remove').click();
  await waitFor(() => savedConfigs().some(config => !config.includes('app=com.beta.camera|')), 'beta rule removal save');

  dom.window.close();
  console.log('PASS: per-app libraries, Chinese/English commas, per-app logs, selection, removal, and search work');
})().catch(error => {
  console.error(error.stack || error);
  process.exit(1);
});
