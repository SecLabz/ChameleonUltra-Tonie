import { importFlipper, exportFlipper, exportZip, safeName } from './formats.js';
const $ = (id) => document.getElementById(id),
  escape = (s) =>
    String(s).replace(
      /[&<>"']/g,
      (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c],
    );
let state = { favorites: Array(8).fill(0) },
  tags = [],
  busy = false,
  refreshing = false,
  connected = false,
  manage = false,
  restarting = false,
  modalTagId = 0,
  revision = 0;
const slotColors = [
  ['Orange', '#ff982e'],
  ['Blue', '#668cff'],
  ['Green', '#42df63'],
  ['Yellow', '#ffe04b'],
  ['Magenta', '#ff65eb'],
  ['Cyan', '#40e3ed'],
  ['White', '#ffffff'],
  ['Red', '#ff4545'],
];
const icon = `<svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor"
  stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
  <path d="M10 6H5v12h5l5 3V3zM18 8a7 7 0 0 1 0 8M20 5a11 11 0 0 1 0 14"/>
</svg>`;
const message = (e) =>
  ({
    busy: 'Remove the Ultra from the reader, then try again.',
    'storage-full': 'The library is full. Export and remove some tags.',
    'invalid-data': 'This file or operation is not supported.',
    'storage-error': 'Storage could not be read or written.',
    'not-found': 'That tag is no longer in the library.',
  })[e] || e;
let toastTimer;
function toast(text, error = false) {
  $('toast').textContent = text;
  $('toast').classList.toggle('error', error);
  $('toast').hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => ($('toast').hidden = true), error ? 8000 : 4000);
}
async function api(path, options = {}) {
  const controller = new AbortController(),
    timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch('/api/v1/' + path, {
      cache: 'no-store',
      ...options,
      signal: controller.signal,
    });
    if (!response.ok) {
      let data;
      try {
        data = await response.json();
      } catch {}
      throw Error(message(data?.error || `Device returned HTTP ${response.status}`));
    }
    return options.binary ? new Uint8Array(await response.arrayBuffer()) : await response.json();
  } catch (e) {
    if (e.name === 'AbortError')
      throw Error('The Ultra is not responding. Check the USB connection and try again.');
    throw e;
  } finally {
    clearTimeout(timeout);
  }
}
function post(path, body) {
  return api(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body,
  });
}
async function refresh() {
  if (busy || refreshing || restarting) return;
  refreshing = true;
  const started = revision;
  try {
    const next = await api('state');
    if (started !== revision || busy || restarting) return;
    const library = await api('tags');
    if (started !== revision || busy || restarting) return;
    state = next;
    tags = library.tags;
    connected = true;
    render();
  } catch (e) {
    if (started !== revision || busy || restarting) return;
    connected = false;
    if (!tags.length) {
      $('tag-list').innerHTML = `<div class="empty-state">
        <span>⌁</span><h2>Connect your Ultra</h2>
        <p>Use a USB data cable, then open 192.168.7.1.</p>
        <button id="retry-connect">Try again</button>
      </div>`;
    }
  } finally {
    refreshing = false;
    if (started !== revision && !busy && !restarting) refresh();
  }
}
async function action(fn, success) {
  if (busy) return;
  busy = true;
  revision++;
  document.querySelectorAll('button').forEach((b) => {
    if (!b.closest('dialog')) b.disabled = true;
  });
  try {
    await fn();
    if (success) toast(success);
  } catch (e) {
    toast(e.message, true);
  } finally {
    busy = false;
    document.querySelectorAll('button').forEach((b) => (b.disabled = false));
    await refresh();
  }
}
function page() {
  const name = ['favorites', 'library', 'device'].includes(location.hash.slice(1))
    ? location.hash.slice(1)
    : 'favorites';
  document.querySelectorAll('.page').forEach((p) => (p.hidden = p.id !== name));
  document.querySelectorAll('nav a').forEach((a) => {
    const selected = a.dataset.page === name;
    a.classList.toggle('selected', selected);
    if (selected) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
  $('live-strip').hidden = !state.active || name === 'favorites';
}
function render() {
  renderDevice();
  renderActive();
  renderFavorites();
  renderLibrary();
  page();
}

function renderDevice() {
  const battery = Number.isFinite(state.battery) ? Math.max(0, Math.min(100, state.battery)) : null;
  $('battery').innerHTML = `
    <span class="battery-icon" aria-hidden="true"><span style="width:${battery ?? 0}%"></span></span>
    <span>${battery === null ? '—' : battery + '%'}</span>`;
  $('battery').title = battery === null ? 'Battery level unavailable' : `Battery ${battery}%`;
  $('battery').classList.toggle('low', battery !== null && battery <= 20);
  $('device-battery').textContent = Number.isFinite(state.battery)
    ? `${state.battery}%`
    : 'Unknown';
  $('device-count').textContent = `${tags.length} tags`;
  $('library-count').textContent = `${tags.length} / ${state.limit || 64} tags`;
  $('firmware-version').textContent = `ChameleonUltra-Tonie · ${state.firmware || 'development'}`;
}

function renderActive() {
  const title =
    state.active || state.selected
      ? state.name
      : tags.length
        ? 'Choose a favorite'
        : 'Start your collection';
  const status = state.active
    ? '● Now playing'
    : connected
      ? state.selected
        ? 'Paused'
        : 'Ready to play'
      : 'Waiting for your Ultra';
  const controls = state.active
    ? '<button class="primary" id="stop-active">Ⅱ Pause</button>'
    : state.selected
      ? '<button class="primary" id="resume-last">▶ Play</button>'
      : !tags.length
        ? '<button class="primary" id="first-import">＋ Import tags</button>'
        : '';
  $('active-panel').innerHTML = `
    <article class="active-card">
      <p class="status">${status}</p>
      <div class="active-name"><h2>${escape(title)}</h2></div>
      ${controls ? `<div class="actions">${controls}</div>` : ''}
    </article>`;
  $('live-strip').innerHTML = `
    ● ${escape(state.name)} · now playing
    <button id="strip-open" aria-label="Open active tag">↑</button>`;
}

function renderFavorites() {
  $('favorite-grid').classList.toggle('managing', manage);
  $('favorite-grid').innerHTML = state.favorites
    .map((id, i) => {
      const tag = tags.find((t) => t.id === id),
        selected = !!id && id === state.selected;
      return `
      <button class="favorite ${tag ? '' : 'empty'} ${selected ? 'active' : ''}"
        data-slot="${i}" style="--slot-color:${slotColors[i][1]}" aria-pressed="${selected}"
        aria-label="${manage ? 'Edit favorite' : 'Favorite'} ${i + 1}, ${slotColors[i][0]}: ${escape(tag?.name || 'empty')}">
        <span class="edit-marker" aria-hidden="true">✎</span>
        <span class="slot"><span class="slot-dot" aria-hidden="true"></span>${i + 1} · ${slotColors[i][0]}</span>
        <span class="glyph">${tag ? icon : '＋'}</span>
        <span class="name">${escape(tag?.name || 'Add a tag')}</span>
      </button>`;
    })
    .join('');
}

function renderLibrary() {
  const query = $('search').value.toLocaleLowerCase();
  const list = tags.filter((t) => t.name.toLocaleLowerCase().includes(query));
  $('tag-list').innerHTML = list.length
    ? list
        .map((t) => {
          const slots = state.favorites.map((id, i) => (id === t.id ? i + 1 : 0)).filter(Boolean);
          const active = state.active && t.id === state.selected;
          const labels = slots
            .map(
              (slot) => `
      <span class="favorite-label" style="--slot-color:${slotColors[slot - 1][1]}">
        <span class="slot-dot" aria-hidden="true"></span>${slot} · ${slotColors[slot - 1][0]}
      </span>`,
            )
            .join('');
          const favorites = slots.length
            ? `<div class="tag-favorites" aria-label="Assigned favorites">${labels}</div>`
            : active
              ? ''
              : '<p class="tag-meta">Not assigned</p>';
          return `
      <article class="tag-card ${active ? 'active' : ''}">
        <div class="tag-icon" aria-hidden="true">${icon}</div>
        <div class="tag-info">
          <div class="tag-title"><strong>${escape(t.name)}</strong></div>
          <p class="tag-meta uid">${escape(t.uid)}</p>
          ${active ? '<p class="tag-meta online-text">● Now playing</p>' : ''}
          ${favorites}
        </div>
        <div class="tag-actions">
          <button class="primary" data-emulate="${t.id}">▶ Play</button>
          <button class="star" data-favorite="${t.id}" aria-pressed="${!!slots.length}"
            aria-label="${slots.length ? 'Remove' : 'Add'} ${escape(t.name)} ${slots.length ? 'from' : 'to'} favorites">
            ${slots.length ? '★' : '☆'}
          </button>
          <button class="more" data-more="${t.id}" aria-label="More actions for ${escape(t.name)}">···</button>
        </div>
      </article>`;
        })
        .join('')
    : `
    <div class="empty-state">
      <span>▤</span><h2>${tags.length ? 'No matching tags' : 'Your collection starts here'}</h2>
      <p>${tags.length ? 'Try another name.' : 'Import SLIX and SLIX-L .nfc files from your phone.'}</p>
    </div>`;
}

/* Stable containers keep their handlers when rendered children are replaced. */
$('favorite-grid').addEventListener('click', (e) => {
  const button = e.target.closest('button[data-slot]');
  if (!button || button.disabled || busy) return;
  const slot = +button.dataset.slot,
    id = state.favorites[slot];
  if (manage || !id) return assign(slot);
  return action(() => post(`emulate?id=${id}`), 'Playing.');
});
$('tag-list').addEventListener('click', (e) => {
  const button = e.target.closest('button');
  if (!button || button.disabled || busy) return;
  if (button.id === 'retry-connect') return refresh();
  if (button.dataset.emulate)
    return action(() => post(`emulate?id=${button.dataset.emulate}`), 'Playing.');
  if (button.dataset.favorite) return toggleFavorite(+button.dataset.favorite);
  if (button.dataset.more) return more(+button.dataset.more);
});
$('active-panel').addEventListener('click', (e) => {
  const button = e.target.closest('button');
  if (!button || button.disabled || busy) return;
  if (button.id === 'stop-active') return action(() => post('stop'), 'Paused.');
  if (button.id === 'resume-last')
    return action(() => post(`emulate?id=${state.selected}`), 'Playing.');
  if (button.id === 'first-import') $('files').click();
});
$('live-strip').addEventListener('click', (e) => {
  const button = e.target.closest('button');
  if (button?.id === 'strip-open' && !button.disabled && !busy) location.hash = 'favorites';
});

$('modal-close').onclick = $('modal-cancel').onclick = () => $('modal').close('cancel');
function modal(title, html, confirm = 'Save') {
  return new Promise((resolve) => {
    const d = $('modal');
    $('modal-title').textContent = title;
    $('modal-body').innerHTML = html;
    $('modal-confirm').textContent = confirm;
    $('modal-confirm').hidden = !confirm;
    $('modal-cancel').textContent = confirm ? 'Cancel' : 'Done';
    d.returnValue = '';
    d.onclose = () => resolve(d.returnValue === 'confirm');
    d.showModal();
  });
}
async function assign(slot) {
  const choices = tags
    .map(
      (t) => `
    <option value="${t.id}" ${state.favorites[slot] === t.id ? 'selected' : ''}>${escape(t.name)}</option>`,
    )
    .join('');
  const remove = state.favorites[slot]
    ? '<button type="button" class="modal-option" id="favorite-remove">Remove from favorites</button>'
    : '';
  const result = modal(
    `Favorite ${slot + 1} · ${slotColors[slot][0]}`,
    `<label>Tag<select id="tag-choice"><option value="0">Empty</option>${choices}</select></label>${remove}`,
  );
  if (state.favorites[slot])
    $('favorite-remove').onclick = async () => {
      await closeModal();
      action(() => post(`favorite?slot=${slot + 1}&id=0`), 'Favorite removed.');
    };
  if (await result)
    action(() => post(`favorite?slot=${slot + 1}&id=${$('tag-choice').value}`), 'Favorite saved.');
}
function toggleFavorite(id) {
  const slots = state.favorites.map((tid, i) => (tid === id ? i + 1 : 0)).filter(Boolean);
  if (!slots.length) return chooseSlot(id);
  return action(async () => {
    for (const slot of slots) await post(`favorite?slot=${slot}&id=0`);
  }, 'Removed from favorites.');
}
async function chooseSlot(id) {
  const choices = state.favorites
    .map(
      (tid, i) => `
    <option value="${i + 1}">${i + 1} · ${slotColors[i][0]} · ${escape(tags.find((t) => t.id === tid)?.name || 'Empty')}</option>`,
    )
    .join('');
  if (
    await modal(
      'Assign a favorite',
      `<label>Favorite position<select id="slot-choice">${choices}</select></label>`,
    )
  )
    action(() => post(`favorite?slot=${$('slot-choice').value}&id=${id}`), 'Favorite saved.');
}
function closeModal() {
  return new Promise((resolve) => {
    const d = $('modal');
    d.addEventListener('close', resolve, { once: true });
    d.close('cancel');
  });
}
function download(blob, name) {
  const url = URL.createObjectURL(blob),
    a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}
async function exportTag(t) {
  const b = await api(`tag?id=${t.id}`, { binary: true });
  download(new Blob([exportFlipper(b)], { type: 'text/plain' }), safeName(t.name) + '.nfc');
}
async function more(id) {
  modalTagId = id;
  const t = tags.find((t) => t.id === id);
  await modal(
    t.name,
    `<button type="button" class="modal-option" id="tag-export">↓ Export original .nfc</button>
    <button type="button" class="modal-option" id="tag-rename">Rename</button>
    <button type="button" class="modal-option danger" id="tag-delete">Delete from library</button>`,
    null,
  );
}
/* Delegated modal actions also work after the dialog content is replaced. */
document.addEventListener('click', async (e) => {
  const id = e.target.id;
  if (!['tag-export', 'tag-rename', 'tag-delete'].includes(id)) return;
  const t = tags.find((t) => t.id === modalTagId);
  if (!t) return;
  await closeModal();
  if (id === 'tag-export') action(() => exportTag(t), 'Original dump exported.');
  if (id === 'tag-rename') {
    if (
      await modal(
        'Rename tag',
        `<label>Name<input id="new-name" value="${escape(t.name)}" autocomplete="off"></label>`,
      )
    )
      action(
        () => post(`rename?id=${t.id}&name=${encodeURIComponent($('new-name').value.trim())}`),
        'Tag renamed.',
      );
  }
  if (
    id === 'tag-delete' &&
    (await modal(
      'Delete tag?',
      `<p>Delete <strong>${escape(t.name)}</strong> and clear its favorite shortcuts? Export it first if you want a backup.</p>`,
      'Delete',
    ))
  )
    action(() => post(`delete?id=${t.id}`), 'Tag deleted.');
});
$('import-button').onclick = () => $('files').click();
$('files').onchange = () => {
  const files = Array.from($('files').files);
  $('files').value = '';
  action(async () => {
    let passed = 0;
    const failures = [];
    for (const file of files) {
      try {
        if (file.size > 64 * 1024) throw Error('File is too large');
        const b = importFlipper(await file.text());
        await post(`import?name=${encodeURIComponent(safeName(file.name))}`, b);
        passed++;
      } catch (e) {
        failures.push(`${file.name}: ${e.message}`);
      }
    }
    if (failures.length) {
      await modal(
        'Import results',
        `<p>${passed} imported · ${failures.length} failed</p><ul>${failures.map((s) => `<li>${escape(s)}</li>`).join('')}</ul>`,
        null,
      );
    } else toast(`${passed} tag${passed === 1 ? '' : 's'} imported. Playback is paused.`);
  });
};
$('export-all').onclick = () =>
  action(async () => {
    if (!tags.length) throw Error('The library is empty.');
    const files = [];
    for (const t of tags) {
      const b = await api(`tag?id=${t.id}`, { binary: true });
      files.push({ name: `${safeName(t.name)}-${t.id}.nfc`, text: exportFlipper(b) });
    }
    download(exportZip(files), 'ChameleonUltra-Tonie-library.zip');
  }, 'Library exported.');
$('library-reset').onclick = async () => {
  if (
    await modal(
      'Erase the entire library?',
      '<p>All stored tags and favorites will be erased. Export any originals you want to keep before continuing.</p>',
      'Erase library',
    )
  )
    action(() => post('library-reset'), 'Library erased.');
};
$('bootloader').onclick = async () => {
  if (
    await modal(
      'Restart in DFU mode?',
      '<p>The web interface will disconnect. Use a Tonie application-only DFU package to update over USB.</p>',
      'Restart',
    )
  )
    action(async () => {
      await post('bootloader');
      restarting = true;
      connected = false;
    }, 'Restarting in DFU update mode.');
};
$('manage-favorites').onclick = () => {
  manage = !manage;
  $('manage-favorites').textContent = manage ? 'Done' : 'Manage';
  $('manage-favorites').setAttribute('aria-pressed', String(manage));
  render();
};
$('search').oninput = renderLibrary;
window.addEventListener('hashchange', page);
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) refresh();
});
page();
refresh();
setInterval(() => {
  if (!document.hidden) refresh();
}, 5000);
