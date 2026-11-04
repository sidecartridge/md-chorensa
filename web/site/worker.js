// The cartridge's install gate (crinstall.wasm) over the user's zip, this
// release's keyed pack and the files it makes, all in memory: for both
// sound paths, then the files back to the page. Nothing leaves the computer.

import createModule from './crinstall.mjs';

const ZIP = 'repository.zip';

onmessage = async (event) => {
  const { zip, pack, firmware } = event.data;
  const reader = new FileReaderSync();
  const files = new Map(); // name -> { bytes: Uint8Array, length }
  for (const [name, buffer] of Object.entries(pack)) {
    const bytes = new Uint8Array(buffer);
    files.set(name, { bytes, length: bytes.length });
  }
  const handles = [];
  const grow = (f, need) => {
    if (need <= f.bytes.length) return;
    const bigger = new Uint8Array(Math.max(need, f.bytes.length * 2, 1 << 16));
    bigger.set(f.bytes.subarray(0, f.length));
    f.bytes = bigger;
  };
  const host = {
    crOpen(name, mode) {
      if (name === ZIP) {
        handles.push({ zip: true });
        return handles.length - 1;
      }
      let f = files.get(name);
      if (mode === 0 && !f) return -1;
      if (mode === 1 || !f) {
        f = { bytes: new Uint8Array(0), length: 0 };
        files.set(name, f);
      }
      handles.push(f);
      return handles.length - 1;
    },
    crRead(h, pos, len) {
      const f = handles[h];
      if (f.zip) return new Uint8Array(reader.readAsArrayBuffer(zip.slice(pos, pos + len)));
      return f.bytes.slice(pos, Math.min(pos + len, f.length));
    },
    crWrite(h, pos, data) {
      const f = handles[h];
      grow(f, pos + data.length);
      f.bytes.set(data, pos);
      f.length = Math.max(f.length, pos + data.length);
      return 0;
    },
    crSize(h) {
      const f = handles[h];
      return f.zip ? zip.size : f.length;
    },
    crClose() {},
    crStat(name) {
      if (name === ZIP) return zip.size;
      const f = files.get(name);
      return f ? f.length : -1;
    },
    crUnlink(name) {
      files.delete(name);
    },
    crList(index) {
      const names = [ZIP, ...files.keys()];
      return index < names.length ? names[index] : '';
    },
    crProgress(bar) {
      postMessage({ type: 'progress', path: currentPath, bar });
    },
  };
  let currentPath = 0;
  const module = await createModule(host);
  const fw = module._malloc(firmware.length + 1);
  module.stringToUTF8(firmware, fw, firmware.length + 1);
  for (const path of [0, 1]) {
    currentPath = path;
    const status = module._cr_install(path, fw);
    if (status !== 0) {
      postMessage({ type: 'done', status });
      return;
    }
  }
  const out = [];
  const transfer = [];
  for (const [name, f] of files) {
    if (/\.(IMK|STK)$/.test(name)) continue; // the pack stays on the site
    const bytes = f.bytes.slice(0, f.length);
    out.push({ name, bytes });
    transfer.push(bytes.buffer);
  }
  postMessage({ type: 'done', status: 0, files: out }, transfer);
};
