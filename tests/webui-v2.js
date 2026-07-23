(() => {
  'use strict';
  const MODULE_ID = 'inline_hook_spoof';
  const CONFIG_PATH = `/data/adb/modules/${MODULE_ID}/config.v2.conf`;
  const PACKAGE_RE = /^[A-Za-z][A-Za-z0-9_$]*(?:\.[A-Za-z][A-Za-z0-9_$]*)+$/;
  const DEFAULT_CONFIG = Object.freeze({ enabled: false, mode: 'audit', library: 'libart.so', includeSubprocesses: false, maxRestoreBytes: 4096, packages: [] });
  let config = { ...DEFAULT_CONFIG, packages: [] };
  let installedPackages = [];
  let selectedPackages = new Set();

  const $ = id => document.getElementById(id);
  const status = (id, message) => { $(id).textContent = message; };
  const isKsu = () => Boolean(window.ksu && typeof window.ksu.exec === 'function');

  function exec(command) {
    return new Promise(resolve => {
      if (!isKsu()) return resolve({ code: -1, stdout: '', stderr: 'KernelSU WebUI API unavailable' });
      const callback = `inline_hook_spoof_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
      window[callback] = (code, stdout, stderr) => {
        delete window[callback];
        resolve({ code: Number(code) || 0, stdout: String(stdout || ''), stderr: String(stderr || '') });
      };
      try { window.ksu.exec(command, JSON.stringify({}), callback); }
      catch (error) { delete window[callback]; resolve({ code: -1, stdout: '', stderr: String(error) }); }
    });
  }

  function parseConfig(text) {
    const next = { ...DEFAULT_CONFIG, packages: [] };
    for (const rawLine of text.split(/\r?\n/)) {
      const line = rawLine.trim();
      if (!line || line.startsWith('#')) continue;
      const separator = line.indexOf('=');
      if (separator <= 0) continue;
      const key = line.slice(0, separator).trim();
      const value = line.slice(separator + 1).trim();
      if (key === 'enabled') next.enabled = value === 'true';
      else if (key === 'mode' && (value === 'audit' || value === 'restore')) next.mode = value;
      else if (key === 'library' && value === 'libart.so') next.library = value;
      else if (key === 'include_subprocesses') next.includeSubprocesses = value === 'true';
      else if (key === 'max_restore_bytes' && ['1024', '4096', '16384', '65536'].includes(value)) next.maxRestoreBytes = Number(value);
      else if (key === 'package' && PACKAGE_RE.test(value)) next.packages.push(value);
    }
    return next;
  }

  function renderConfig() {
    $('enabled').checked = config.enabled;
    $('budget').value = String(config.maxRestoreBytes);
    $('subprocesses').checked = config.includeSubprocesses;
    document.querySelector(`input[name="mode"][value="${config.mode}"]`).checked = true;
    selectedPackages = new Set(config.packages);
    syncRestoreGate();
    renderPackages();
  }

  function selectedMode() { return document.querySelector('input[name="mode"]:checked').value; }

  function syncRestoreGate() {
    const restore = selectedMode() === 'restore';
    $('restore-gate').hidden = !restore;
    $('runtime-notice').className = restore ? 'notice warn' : 'notice';
    $('runtime-notice').innerHTML = restore
      ? '<strong>Restore mode selected.</strong> V2 still rejects unknown libraries, unselected processes, excessive differences, invalid mappings, and write verification failures.'
      : '<strong>Safe default active.</strong> The generated V2 configuration starts disabled. Audit mode never changes memory. Restore mode requires explicit enablement, at least one selected package, and a typed confirmation.';
  }

  function visiblePackagesForSearch() {
    const filter = $('package-search').value.trim().toLowerCase();
    const known = new Map(installedPackages.map(entry => [entry.packageName, entry]));
    for (const packageName of selectedPackages) {
      if (!known.has(packageName)) known.set(packageName, { packageName, appLabel: packageName });
    }
    const rows = [...known.values()]
      .filter(entry => !filter || entry.packageName.toLowerCase().includes(filter) || String(entry.appLabel || '').toLowerCase().includes(filter))
      .sort((a, b) => a.packageName.localeCompare(b.packageName));
    return { filter, total: known.size, rows };
  }

  function renderPackages() {
    const { filter, total, rows } = visiblePackagesForSearch();
    $('package-list').replaceChildren();
    if (!rows.length) {
      const empty = document.createElement('div');
      empty.className = 'empty';
      empty.textContent = isKsu() ? 'No packages match this search.' : 'KernelSU API is unavailable. Add a package manually.';
      $('package-list').append(empty);
    }
    for (const entry of rows) {
      const item = document.createElement('label'); item.className = `package${selectedPackages.has(entry.packageName) ? ' selected' : ''}`;
      const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.checked = selectedPackages.has(entry.packageName);
      checkbox.addEventListener('change', () => { if (checkbox.checked) selectedPackages.add(entry.packageName); else selectedPackages.delete(entry.packageName); renderPackages(); });
      const copy = document.createElement('span'); copy.className = 'package-name';
      const name = document.createElement('strong'); name.textContent = entry.appLabel || entry.packageName;
      const code = document.createElement('code'); code.textContent = entry.packageName;
      copy.append(name, code); item.append(checkbox, copy); $('package-list').append(item);
    }
    status('package-count', `${selectedPackages.size} selected`);
    status('search-feedback', filter
      ? (rows.length ? `Showing ${rows.length} of ${total} packages for “${$('package-search').value.trim()}”.` : `No package matches “${$('package-search').value.trim()}”.`)
      : (total ? `${total} packages available. Type a name, then press Search or Enter.` : 'Type a package name or app label to filter the list.'));
  }

  function applyPackageSearch() {
    renderPackages();
  }

  async function refreshPackages() {
    if (!isKsu()) { status('load-status', 'KernelSU API unavailable; manual package entry remains available'); renderPackages(); return; }
    status('load-status', 'Loading installed packages…');
    const response = await exec('pm list packages -3');
    if (response.code !== 0) { status('load-status', 'Could not load installed packages'); renderPackages(); return; }
    const names = [...new Set(response.stdout.split(/\r?\n/).map(line => line.trim().replace(/^package:/, '')).filter(name => PACKAGE_RE.test(name)))];
    let info = [];
    try {
      if (typeof window.ksu.getPackagesInfo === 'function') info = JSON.parse(window.ksu.getPackagesInfo(JSON.stringify(names)) || '[]');
    } catch (_) { info = []; }
    const labels = new Map(info.map(entry => [entry.packageName, entry.appLabel || entry.packageName]));
    installedPackages = names.map(packageName => ({ packageName, appLabel: labels.get(packageName) || packageName }));
    status('load-status', `Loaded ${installedPackages.length} user packages`);
    renderPackages();
  }

  function buildConfig(forceDisabled = false) {
    const enabled = forceDisabled ? false : $('enabled').checked;
    const mode = selectedMode();
    const packages = [...selectedPackages].sort();
    if (enabled && packages.length === 0) throw new Error('An enabled configuration requires at least one selected package.');
    if (mode === 'restore' && enabled && (!$('restore-ack').checked || $('restore-token').value.trim() !== 'RESTORE')) throw new Error('Restore mode requires the acknowledgement and exact RESTORE confirmation.');
    return ['# Inline Hook Spoof V2 configuration', '# Default posture: disabled + audit. Only libart.so is accepted by the native module.', 'version=2', `enabled=${enabled}`, `mode=${mode}`, 'library=libart.so', `include_subprocesses=${$('subprocesses').checked}`, `max_restore_bytes=${$('budget').value}`, ...packages.map(packageName => `package=${packageName}`), ''].join('\n');
  }

  async function save(forceDisabled = false) {
    try {
      const content = buildConfig(forceDisabled);
      const encoded = btoa(content);
      const temporary = `${CONFIG_PATH}.ui-${Date.now()}`;
      status('save-status', 'Writing configuration…');
      const command = `umask 022; printf %s '${encoded}' | base64 -d > '${temporary}' && chmod 0644 '${temporary}' && mv '${temporary}' '${CONFIG_PATH}'`;
      const response = await exec(command);
      if (response.code !== 0) throw new Error(response.stderr || 'The root WebUI command failed.');
      config = parseConfig(content);
      renderConfig();
      status('save-status', forceDisabled ? 'Disabled V2 configuration saved' : 'V2 configuration saved');
    } catch (error) { status('save-status', `Not saved: ${error.message}`); }
  }

  function addManualPackage() {
    const packageName = $('manual-package').value.trim();
    if (!PACKAGE_RE.test(packageName)) { status('save-status', 'Not added: invalid package identifier'); return; }
    selectedPackages.add(packageName); $('manual-package').value = ''; renderPackages();
  }

  async function initialize() {
    if (!isKsu()) status('load-status', 'KernelSU API unavailable; the UI can prepare but not save configuration');
    else {
      const response = await exec(`cat '${CONFIG_PATH}'`);
      if (response.code === 0 && response.stdout.trim()) { config = parseConfig(response.stdout); status('load-status', 'Loaded V2 configuration'); }
      else status('load-status', 'No V2 configuration found; using safe defaults');
    }
    renderConfig();
    await refreshPackages();
  }

  document.querySelectorAll('input[name="mode"]').forEach(input => input.addEventListener('change', syncRestoreGate));
  $('package-search').addEventListener('input', renderPackages);
  $('package-search').addEventListener('search', applyPackageSearch);
  $('package-search').addEventListener('keydown', event => {
    if (event.key === 'Enter') {
      event.preventDefault();
      applyPackageSearch();
    }
  });
  $('search-packages').addEventListener('click', applyPackageSearch);
  $('refresh-packages').addEventListener('click', refreshPackages);
  $('add-package').addEventListener('click', addManualPackage);
  $('manual-package').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); addManualPackage(); } });
  $('save').addEventListener('click', () => save(false));
  $('disable-save').addEventListener('click', () => save(true));
  initialize();
})();