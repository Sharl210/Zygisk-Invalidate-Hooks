'use strict';

const assert = require('assert');
const fs = require('fs');
const { JSDOM } = require('jsdom');
const html = fs.readFileSync('module/template/webroot/index.html', 'utf8');

function makeDom(language) {
  return new JSDOM(html, {
    runScripts: 'dangerously',
    pretendToBeVisual: true,
    beforeParse(window) {
      Object.defineProperty(window.navigator, 'language', { value: language, configurable: true });
      window.TextEncoder = TextEncoder;
    },
  });
}

const zh = makeDom('zh-CN');
assert.strictEqual(zh.window.document.documentElement.lang, 'zh-CN');
assert.strictEqual(zh.window.document.getElementById('search-input').placeholder, '搜索应用或包名…');
assert.strictEqual(zh.window.document.getElementById('per-app-dialog-title').textContent, '应用实例配置');
assert.strictEqual(zh.window.document.getElementById('per-app-diagnostics').textContent, '高级：内存映射');
assert.match(zh.window.document.getElementById('settings').textContent, /跟随系统：中文/);
zh.window.close();

const en = makeDom('fr-FR');
assert.strictEqual(en.window.document.documentElement.lang, 'en');
assert.strictEqual(en.window.document.getElementById('search-input').placeholder, 'Search apps or packages...');
assert.strictEqual(en.window.document.getElementById('per-app-dialog-title').textContent, 'Application configuration');
assert.strictEqual(en.window.document.getElementById('per-app-diagnostics').textContent, 'Advanced memory maps');
assert.match(en.window.document.getElementById('settings').textContent, /Follows system: English/);
en.window.close();

console.log('PASS: WebUI follows Chinese system language and English fallback');
