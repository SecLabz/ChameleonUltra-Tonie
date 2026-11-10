/* Run the embedded page with delayed fetches and a minimal DOM. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const html = fs.readFileSync(new URL('../build/index.html', import.meta.url), 'utf8');
const source = html.match(/<script type="module">([\s\S]*?)<\/script>/)[1];
const tick = () => new Promise((resolve) => setImmediate(resolve));
function browser() {
  const nodes = new Map(),
    requests = [];
  const node = (id) => {
    if (!nodes.has(id))
      nodes.set(id, {
        listeners: new Map(),
        dataset: {},
        open: false,
        value: '',
        textContent: '',
        innerHTML: '',
        focus() {},
        select() {},
        classList: { toggle() {} },
        showModal() {
          this.open = true;
        },
        close(v) {
          this.open = false;
          this.returnValue = v;
        },
        closest() {
          return null;
        },
        addEventListener(type, callback) {
          const listeners = this.listeners.get(type) || [];
          listeners.push(callback);
          this.listeners.set(type, listeners);
        },
      });
    return nodes.get(id);
  };
  const context = vm.createContext({
    AbortController,
    TextEncoder,
    Uint8Array,
    Blob,
    URL,
    console,
    location: { hash: '#library' },
    setTimeout() {
      return 1;
    },
    clearTimeout() {},
    setInterval() {},
    window: { addEventListener() {} },
    document: {
      hidden: false,
      getElementById: node,
      querySelectorAll() {
        return [];
      },
      addEventListener() {},
    },
    fetch(path, options) {
      return new Promise((resolve, reject) => requests.push({ path, options, resolve, reject }));
    },
  });
  vm.runInContext(source, context);
  return { context, requests, nodes, run: (s) => vm.runInContext(s, context) };
}
function click(b, container, button) {
  const listeners = b.nodes.get(container).listeners.get('click');
  assert.equal(listeners.length, 1, 'Rendering must not accumulate event handlers');
  // A nested SVG/path or label resolves to its containing button.
  return listeners[0]({
    target: {
      closest(selector) {
        return selector === 'button' ||
          (selector === 'button[data-slot]' && 'slot' in button.dataset)
          ? button
          : null;
      },
    },
  });
}
async function reply(b, index, data) {
  assert(b.requests[index], `Missing request ${index}`);
  b.requests[index].resolve({
    ok: true,
    json: async () => data,
    arrayBuffer: async () => data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
  });
  await tick();
}
const state = (active) => ({
  active,
  selected: 1,
  name: 'Synthetic',
  uid: 'E004030201000001',
  profile: 2,
  favorites: [1, 0, 0, 0, 0, 0, 0, 0],
  battery: 80,
});
// Import and export must work in the generated page, including aliased image constants.
{
  const b = browser(),
    text = fs.readFileSync('build/test-slix.nfc', 'utf8'),
    image = new Uint8Array(fs.readFileSync('build/test-slix.nfci'));
  await reply(b, 0, state(false));
  await reply(b, 1, { tags: [] });
  b.nodes.get('files').files = [
    { name: 'Synthetic.nfc', size: text.length, text: async () => text },
  ];
  b.nodes.get('files').onchange();
  await tick();
  assert.equal(b.requests[2]?.path, '/api/v1/import?name=Synthetic');
  assert.equal(b.requests[2].options.method, 'POST');
  assert.deepEqual(b.requests[2].options.body, image);
  await reply(b, 2, { ok: true });
  await reply(b, 3, state(false));
  await reply(b, 4, { tags: [{ id: 1, name: 'Synthetic', profile: 2, uid: '00' }] });
  assert.equal(b.nodes.get('toast').textContent, '1 tag imported. Playback is paused.');
  b.run('let saved;download=(blob,name)=>{saved={blob,name}}');
  const exported = b.run('exportTag(tags[0])');
  assert.equal(b.requests[5].path, '/api/v1/tag?id=1&emulation=1');
  const emulated = image.slice();
  emulated[5] = 1;
  emulated[7] &= ~1;
  emulated.fill(0x0f, 76, 84);
  await reply(b, 5, emulated);
  await exported;
  assert.equal(b.run('saved.name'), 'Synthetic.nfc');
  assert.equal(
    await b.run('saved.blob.text()'),
    text
      .replace('Password Privacy: 01 02 03 04', 'Password Privacy: 0F 0F 0F 0F')
      .replace('Password Destroy: 00 00 00 00', 'Password Destroy: 0F 0F 0F 0F')
      .replace('Privacy Mode: true', 'Privacy Mode: false'),
  );
}
console.log('Embedded page: NFC file import and SLIX emulation export passed');
// An old library response arrives after a successful mutation.
{
  const b = browser();
  await reply(b, 0, state(true));
  assert.equal(b.run('state.active'), undefined, 'Partial refresh must not commit state');
  const action = b.run("action(()=>post('stop'),'Stopped')");
  await reply(b, 2, { ok: true });
  await action;
  await reply(b, 1, { tags: [{ id: 1, name: 'Old', profile: 2, uid: '00' }] });
  assert.equal(b.run('state.active'), undefined, 'Pre-mutation refresh must not commit');
  assert.equal(b.requests[3].path, '/api/v1/state');
  await reply(b, 3, state(false));
  await reply(b, 4, { tags: [{ id: 1, name: 'Fresh', profile: 2, uid: '00' }] });
  assert.equal(b.run('state.active'), false);
  assert.equal(b.run('tags[0].name'), 'Fresh');
  assert.equal(b.run('connected'), true);
}
// A refresh finishing during a mutation must leave the final refresh to that action.
{
  const b = browser(),
    action = b.run("action(()=>post('stop'),'Stopped')");
  await reply(b, 0, state(true));
  assert.equal(b.requests.length, 2, 'Busy action must prevent follow-up refresh');
  await reply(b, 1, { ok: true });
  await reply(b, 2, state(false));
  await reply(b, 3, { tags: [] });
  await action;
  assert.equal(b.run('state.active'), false);
  assert.equal(b.run('connected'), true);
}
// A stale network failure must trigger a current refresh, not overwrite it.
{
  const b = browser();
  const action = b.run("action(()=>post('stop'),'Stopped')");
  await reply(b, 1, { ok: true });
  await action;
  b.requests[0].reject(Error('Old request failed'));
  await tick();
  assert.equal(b.requests[2].path, '/api/v1/state');
  await reply(b, 2, state(false));
  await reply(b, 3, { tags: [] });
  assert.equal(b.run('connected'), true);
}
// Entering update mode must suppress late status commits and further refreshes.
{
  const b = browser();
  await reply(b, 0, state(true));
  const action = b.run(
    "action(async()=>{await post('bootloader');restarting=true;connected=false;},'Restarting')",
  );
  await reply(b, 2, { ok: true });
  await action;
  await reply(b, 1, { tags: [] });
  assert.equal(b.requests.length, 3);
  assert.equal(b.run('restarting'), true);
  assert.equal(b.run('connected'), false);
  assert.equal(b.run('state.active'), undefined);
}
console.log(
  'Browser state: coherent refresh, stale response/failure recovery and update-mode suppression passed',
);
// A failed library fetch must never download a partial ZIP as a successful backup.
{
  const b = browser(),
    tags = [
      { id: 1, name: 'First', profile: 2, uid: '00' },
      { id: 2, name: 'Second', profile: 2, uid: '00' },
    ];
  await reply(b, 0, state(false));
  await reply(b, 1, { tags });
  b.run('let downloads=0;download=()=>downloads++');
  const action = b.nodes.get('export-all').onclick();
  await reply(b, 2, new Uint8Array(fs.readFileSync('build/test-slix.nfci')));
  b.requests[3].resolve({ ok: false, status: 404, json: async () => ({ error: 'not-found' }) });
  await tick();
  await reply(b, 4, state(false));
  await reply(b, 5, { tags });
  await action;
  assert.equal(b.run('downloads'), 0);
  assert.equal(b.nodes.get('toast').textContent, 'That tag is no longer in the library.');
}
console.log('Browser backup: failed tag fetch aborts download without a partial-success message');
// Removing a library favorite clears every duplicate slot and keeps other tags.
{
  const b = browser(),
    tags = [
      { id: 1, name: 'First', profile: 2, uid: '00' },
      { id: 2, name: 'Second', profile: 2, uid: '01' },
    ];
  const before = { ...state(false), favorites: [1, 2, 1, 0, 0, 0, 0, 0] };
  await reply(b, 0, before);
  await reply(b, 1, { tags });
  const removal = b.run('toggleFavorite(1)');
  assert.equal(b.requests[2].path, '/api/v1/favorite?slot=1&id=0');
  await reply(b, 2, { ok: true });
  assert.equal(b.requests[3].path, '/api/v1/favorite?slot=3&id=0');
  await reply(b, 3, { ok: true });
  await reply(b, 4, { ...before, favorites: [0, 2, 0, 0, 0, 0, 0, 0] });
  await reply(b, 5, { tags });
  await removal;
  assert.deepEqual(Array.from(b.run('state.favorites')), [0, 2, 0, 0, 0, 0, 0, 0]);
  assert.equal(b.run('tags.length'), 2);
  assert.equal(b.nodes.get('toast').textContent, 'Removed from favorites.');
}
// A failed removal stops further writes and refreshes the remaining assignments.
{
  const b = browser(),
    before = { ...state(false), favorites: [1, 0, 1, 0, 0, 0, 0, 0] };
  await reply(b, 0, before);
  await reply(b, 1, { tags: [] });
  const removal = b.run('toggleFavorite(1)');
  b.requests[2].resolve({ ok: false, status: 409, json: async () => ({ error: 'busy' }) });
  await tick();
  assert.equal(b.requests[3].path, '/api/v1/state');
  await reply(b, 3, before);
  await reply(b, 4, { tags: [] });
  await removal;
  assert.deepEqual(Array.from(b.run('state.favorites')), before.favorites);
  assert.equal(
    b.nodes.get('toast').textContent,
    'Remove the Ultra from the reader, then try again.',
  );
}
console.log(
  'Browser favorites: duplicate-slot removal, preserved library and failure recovery passed',
);

// Container handlers survive rerenders and resolve nested icon clicks to current state.
{
  const b = browser();
  await reply(b, 0, state(false));
  await reply(b, 1, { tags: [{ id: 1, name: 'First', uid: '00' }] });
  b.run('render();render()');
  const playing = click(b, 'favorite-grid', { dataset: { slot: '0' } });
  assert.equal(b.requests[2].path, '/api/v1/emulate?id=1');
  click(b, 'tag-list', { dataset: { emulate: '1' } });
  assert.equal(b.requests.length, 3, 'A busy action must suppress additional clicks');
  await reply(b, 2, { ok: true });
  await reply(b, 3, state(true));
  await reply(b, 4, { tags: [{ id: 1, name: 'First', uid: '00' }] });
  await playing;
  const paused = click(b, 'active-panel', { id: 'stop-active', dataset: {} });
  assert.equal(b.requests[5].path, '/api/v1/stop');
  await reply(b, 5, { ok: true });
  await reply(b, 6, state(false));
  await reply(b, 7, { tags: [{ id: 1, name: 'First', uid: '00' }] });
  await paused;
  const resumed = click(b, 'active-panel', { id: 'resume-last', dataset: {} });
  assert.equal(b.requests[8].path, '/api/v1/emulate?id=1');
  await reply(b, 8, { ok: true });
  await reply(b, 9, state(true));
  await reply(b, 10, { tags: [{ id: 1, name: 'First', uid: '00' }] });
  await resumed;
  click(b, 'live-strip', { id: 'strip-open', dataset: {} });
  assert.equal(b.context.location.hash, 'favorites');
  click(b, 'tag-list', { dataset: { emulate: '1' }, disabled: true });
  assert.equal(b.requests.length, 11, 'Disabled buttons must remain inactive');
}
// Empty/manage favorite clicks open assignment instead of starting emulation.
{
  const b = browser();
  await reply(b, 0, state(false));
  await reply(b, 1, { tags: [] });
  b.run('let assigned=[];assign=(slot)=>assigned.push(slot)');
  click(b, 'favorite-grid', { dataset: { slot: '1' } });
  b.run('manage=true');
  click(b, 'favorite-grid', { dataset: { slot: '0' } });
  assert.deepEqual(Array.from(b.run('assigned')), [1, 0]);
  assert.equal(b.requests.length, 2);
}
// Library buttons route play, favorites and more actions after rerendering.
{
  const b = browser();
  await reply(b, 0, state(false));
  await reply(b, 1, { tags: [] });
  b.run(
    'let events=[];action=(fn)=>fn();post=(path)=>events.push(path);toggleFavorite=(id)=>events.push(id);more=(id)=>events.push(id)',
  );
  b.run('renderLibrary();renderLibrary()');
  click(b, 'tag-list', { dataset: { emulate: '7' } });
  click(b, 'tag-list', { dataset: { favorite: '8' } });
  click(b, 'tag-list', { dataset: { more: '9' } });
  assert.deepEqual(Array.from(b.run('events')), ['emulate?id=7', 8, 9]);
}
// Retry and first import still work when their buttons are inserted dynamically.
{
  const b = browser();
  b.requests[0].reject(Error('Disconnected'));
  await tick();
  click(b, 'tag-list', { id: 'retry-connect', dataset: {} });
  assert.equal(b.requests[1].path, '/api/v1/state');
  await reply(b, 1, { ...state(false), selected: 0 });
  await reply(b, 2, { tags: [] });
  let opened = 0;
  b.nodes.get('files').click = () => opened++;
  click(b, 'active-panel', { id: 'first-import', dataset: {} });
  assert.equal(opened, 1);
}
// Scan drawer detects tag and saves scanned NFCI.
{
  const b = browser(),
    image = new Uint8Array(fs.readFileSync('build/test-slix.nfci'));
  await reply(b, 0, state(false));
  await reply(b, 1, { tags: [] });
  b.nodes.get('scan-button').onclick();
  assert.equal(b.nodes.get('scan-dialog').open, true);
  assert.equal(b.nodes.get('scan-details').hidden, true);
  assert.equal(b.requests[2].path, '/api/v1/scan');
  await reply(b, 2, image);
  assert.equal(b.nodes.get('scan-details').hidden, false);
  assert.equal(b.nodes.get('scan-name').value, 'Tonie 000001');
  b.nodes.get('scan-name').value = 'My Tonie';
  const saved = b.nodes.get('scan-form').onsubmit({ preventDefault() {} });
  await tick();
  assert.equal(b.nodes.get('scan-dialog').open, false);
  assert.equal(b.requests[3].path, '/api/v1/import?name=My%20Tonie');
  assert.deepEqual(b.requests[3].options.body, image);
  await reply(b, 3, { ok: true, id: 2 });
  await reply(b, 4, state(false));
  await reply(b, 5, { tags: [{ id: 2, name: 'My Tonie', profile: 2, uid: 'E004030201000001' }] });
  await saved;
  assert.equal(b.nodes.get('toast').textContent, 'Tag scanned and saved.');
}
// Cancelling after headers arrive must discard the pending response body.
{
  const b = browser();
  await reply(b, 0, state(false));
  await reply(b, 1, { tags: [] });
  const scan = b.nodes.get('scan-button').onclick();
  let finishBody;
  const body = new Promise((resolve) => (finishBody = resolve));
  b.requests[2].resolve({ ok: true, arrayBuffer: () => body });
  await tick();
  b.nodes.get('scan-cancel').onclick();
  assert.equal(b.requests[2].options.signal.aborted, true);
  finishBody(Uint8Array.from(fs.readFileSync('build/test-slix.nfci')).buffer);
  await scan;
  assert.equal(b.nodes.get('scan-details').hidden, true);
  assert.equal(b.run('scannedBytes'), null);
}
console.log(
  'Browser delegation: nested clicks, rerenders, busy/disabled guards and action routing passed',
);
