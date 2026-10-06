let maxSymbols = 24;
let currentPath = '/.crosspoint/stock_watchlist.json';

const contentEl = document.getElementById('content');
const counterEl = document.getElementById('counter');
const saveBtn = document.getElementById('saveBtn');
const metaEl = document.getElementById('watchlistMeta');

function showMessage(text, isError) {
  const el = document.getElementById('message');
  el.textContent = text;
  el.className = 'message ' + (isError ? 'err' : 'ok');
}

// Accepts {"symbols": [...]} or a bare array, matching the device parser.
function parseSymbols(value) {
  const trimmed = value.trim();
  if (!trimmed) return [];
  let parsed;
  try {
    parsed = JSON.parse(trimmed);
  } catch (err) {
    throw new Error('Invalid JSON: ' + err.message);
  }
  const symbols = Array.isArray(parsed) ? parsed : parsed && Array.isArray(parsed.symbols) ? parsed.symbols : null;
  if (!symbols) throw new Error('Expected an array of symbols, or an object with a "symbols" array.');
  if (!symbols.every((s) => typeof s === 'string')) throw new Error('Every symbol must be a string.');
  return symbols;
}

function updateCounter() {
  let symbols;
  try {
    symbols = parseSymbols(contentEl.value);
  } catch (err) {
    counterEl.textContent = err.message;
    counterEl.classList.add('too-large');
    saveBtn.disabled = true;
    return;
  }
  const invalid = symbols.length === 0 || symbols.length > maxSymbols;
  counterEl.textContent = symbols.length + ' / ' + maxSymbols + ' symbols';
  counterEl.classList.toggle('too-large', invalid);
  saveBtn.disabled = invalid;
}

function showMeta(fromFile) {
  metaEl.textContent = 'Editing ' + currentPath + ' (' + (fromFile ? 'file' : 'built-in defaults') + ')';
}

async function loadWatchlist() {
  try {
    const res = await fetch('/api/stocks');
    if (!res.ok) throw new Error('Could not load the stock watchlist');
    const data = await res.json();
    currentPath = data.path || currentPath;
    maxSymbols = data.maxSymbols || maxSymbols;
    contentEl.value = JSON.stringify({ formatVersion: 1, symbols: data.symbols || [] }, null, 2);
    showMeta(data.fromFile);
  } catch (err) {
    showMessage(err.message || 'Could not load the stock watchlist', true);
  }
  updateCounter();
}

async function saveWatchlist() {
  let symbols;
  try {
    symbols = parseSymbols(contentEl.value);
  } catch (err) {
    showMessage(err.message, true);
    updateCounter();
    return;
  }
  saveBtn.disabled = true;
  saveBtn.textContent = 'Saving...';
  try {
    const res = await fetch('/api/stocks', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json; charset=utf-8' },
      body: JSON.stringify({ formatVersion: 1, symbols: symbols }),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || 'Save failed');
    showMeta(true);
    showMessage('Saved ' + (data.count || symbols.length) + ' symbols.', false);
    await loadWatchlist();
  } catch (err) {
    showMessage(err.message || 'Save failed', true);
  } finally {
    saveBtn.textContent = 'Save';
    updateCounter();
  }
}

contentEl.addEventListener('input', updateCounter);
saveBtn.addEventListener('click', saveWatchlist);
loadWatchlist();
