// The page: this release's keyed pack fetched from pack/, the user's zip
// chosen, the cartridge's install gate run in a worker (worker.js) for both
// sound paths, and the files written to the card's CHORENSHA folder (a
// browser that can) or into CHORENSHA.zip. Nothing leaves the computer.

const $ = (id) => document.getElementById(id);
const PACK_FILES = ['COMMON.IMK', 'MENUS.IMK', 'STAGE1.IMK', 'STAGE2.IMK', 'STAGE3.IMK',
  'STAGE4.IMK', 'STAGE5.IMK', 'STAGE6.IMK', 'STAGE7.IMK', 'START.STK'];
// The gate's install errors (rp/src/include/install.h).
const ERRORS = {
  '-1': 'this zip is not the Amiga port\'s repository',
  '-2': 'a file in the zip is missing or differs from the tested one: download the zip again',
  '-3': 'a file could not be made',
  '-4': 'the zip could not be read',
  '-5': 'this release\'s image pack is missing from the page',
  '-6': 'the image pack is another release\'s',
  '-7': 'the image pack is damaged',
};

let pack = null;  // { firmware, version, files: {name: ArrayBuffer} }
let zip = null;   // the File
let made = null;  // [{name, bytes}]

function status(id, text, kind) {
  $(id).textContent = text;
  $(id).className = `status ${kind || ''}`;
}

// The step to do now, for a page that marks it (the site's title-menu cursor).
function step(n) {
  const convert = $('convert');
  if (convert) convert.dataset.step = String(n);
}

function updateGo() {
  $('go').disabled = !pack || !zip;
}

async function loadPack() {
  try {
    const info = await (await fetch('pack/pack.json')).json();
    const files = {};
    for (const name of PACK_FILES) {
      const r = await fetch(`pack/${name}`);
      if (!r.ok) throw new Error(`${name}: ${r.status}`);
      files[name] = await r.arrayBuffer();
    }
    pack = { ...info, files };
    $('version').textContent = `Version ${info.version}, firmware ${info.firmware}.`;
    status('pack-status', `Image pack for version ${info.version} ready: it works with the `
      + `firmware ${info.firmware} (the app's build ID).`, 'good');
  } catch (e) {
    status('pack-status', `This release's image pack could not be fetched (${e.message}).`, 'bad');
  }
  updateGo();
}

function chooseZip(file) {
  made = null;
  $('save').hidden = true;
  if (!/\.zip$/i.test(file.name)) {
    status('zip-status', `${file.name} is not a zip.`, 'bad');
    zip = null;
    step(1);
  } else {
    zip = file;
    status('zip-status', `${file.name}, ${(file.size / 1e6).toFixed(0)} MB. Ready.`, 'good');
    step(2);
  }
  updateGo();
}

async function makeFiles() {
  $('go').disabled = true;
  $('bar-fill').style.width = '0';
  status('go-status', 'Making the files…');
  const t = performance.now();
  const worker = new Worker('worker.js', { type: 'module' });
  const result = await new Promise((resolve) => {
    worker.onmessage = (event) => {
      const m = event.data;
      if (m.type === 'progress') {
        // Two passes (the DMA chip's, the YM's), each its bar of 224.
        $('bar-fill').style.width = `${(50 * m.path + (50 * m.bar) / 224).toFixed(1)}%`;
      } else if (m.type === 'done') {
        resolve(m);
      }
    };
    worker.postMessage({ zip, pack: pack.files, firmware: pack.firmware });
  });
  worker.terminate();
  if (result.status !== 0) {
    status('go-status', `The files could not be made: ${ERRORS[result.status] || result.status}.`, 'bad');
    updateGo();
    return;
  }
  made = result.files;
  $('bar-fill').style.width = '100%';
  const mb = made.reduce((a, f) => a + f.bytes.length, 0) / 1e6;
  status('go-status', `${made.length} files, ${mb.toFixed(0)} MB, made in ${((performance.now() - t) / 1000).toFixed(0)} s.`, 'good');
  $('save').hidden = false;
  step(3);
  if (!('showDirectoryPicker' in window)) {
    $('to-folder').hidden = true;
    $('folder-hint').textContent = 'Unzip CHORENSHA.zip onto the card: it holds the CHORENSHA folder.';
  }
  updateGo();
}

async function toFolder() {
  try {
    const root = await window.showDirectoryPicker({ mode: 'readwrite' });
    const dir = root.name.toUpperCase() === 'CHORENSHA' ? root
      : await root.getDirectoryHandle('CHORENSHA', { create: true });
    let k = 0;
    for (const f of made) {
      status('save-status', `Writing ${f.name} (${++k} of ${made.length})…`);
      const w = await (await dir.getFileHandle(f.name, { create: true })).createWritable();
      await w.write(f.bytes);
      await w.close();
    }
    status('save-status', `${made.length} files written to the card's CHORENSHA folder.`, 'good');
  } catch (e) {
    if (e.name !== 'AbortError') status('save-status', `Could not write to the card (${e.message}).`, 'bad');
  }
}

// A zip of the files, stored (they hardly compress), under CHORENSHA/.
function crc32(bytes) {
  let table = crc32.table;
  if (!table) {
    table = crc32.table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = table[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function zipOf(files) {
  const parts = [];
  const central = [];
  let offset = 0;
  const enc = new TextEncoder();
  for (const f of files) {
    const name = enc.encode(`CHORENSHA/${f.name}`);
    const crc = crc32(f.bytes);
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034B50, true);
    local.setUint16(4, 20, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, f.bytes.length, true);
    local.setUint32(22, f.bytes.length, true);
    local.setUint16(26, name.length, true);
    parts.push(local, name, f.bytes);
    const entry = new DataView(new ArrayBuffer(46));
    entry.setUint32(0, 0x02014B50, true);
    entry.setUint16(4, 20, true);
    entry.setUint16(6, 20, true);
    entry.setUint32(16, crc, true);
    entry.setUint32(20, f.bytes.length, true);
    entry.setUint32(24, f.bytes.length, true);
    entry.setUint16(28, name.length, true);
    entry.setUint32(42, offset, true);
    central.push(entry, name);
    offset += 30 + name.length + f.bytes.length;
  }
  const size = central.reduce((a, p) => a + p.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054B50, true);
  end.setUint16(8, files.length, true);
  end.setUint16(10, files.length, true);
  end.setUint32(12, size, true);
  end.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end], { type: 'application/zip' });
}

function toZip() {
  const url = URL.createObjectURL(zipOf(made));
  const a = document.createElement('a');
  a.href = url;
  a.download = 'CHORENSHA.zip';
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
  status('save-status', 'CHORENSHA.zip downloaded: unzip it onto the card.', 'good');
}

$('zip').addEventListener('change', (e) => e.target.files[0] && chooseZip(e.target.files[0]));
const drop = $('drop');
drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  if (e.dataTransfer.files[0]) chooseZip(e.dataTransfer.files[0]);
});
$('go').addEventListener('click', makeFiles);
$('to-folder').addEventListener('click', toFolder);
$('to-zip').addEventListener('click', toZip);
loadPack();
