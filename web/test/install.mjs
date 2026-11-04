// The install gate in Node (web/dist/crinstall.mjs), as the page runs it:
// the repository's zip (CHORENSHA_ZIP) and a keyed pack (PACK) made into
// /CHORENSHA's files for both sound paths, each compared with the host's:
// the sound with CHORENSHA_REFERENCE's files, the images with
// CHORENSHA_IMAGES' plain ones, each when set (the gate itself checks every
// unlocked image against the CRC-32 its keyed copy carries). Run by
// web/build.sh test, in Emscripten's Docker image.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import createModule from '../dist/crinstall.mjs';

const zipPath = process.env.CHORENSHA_ZIP;
const pack = process.env.PACK;
const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'chorensha-'));
for (const name of fs.readdirSync(pack)) {
  if (/\.(IMK|STK)$/.test(name)) fs.copyFileSync(path.join(pack, name), path.join(folder, name));
}
const zipName = 'repository.zip';
const where = (name) => (name === zipName ? zipPath : path.join(folder, name));

const handles = [];
let bar = 0, backwards = 0;
const host = {
  crOpen(name, mode) {
    const file = where(name);
    if (mode === 0 && !fs.existsSync(file)) return -1;
    const fd = fs.openSync(file, mode === 0 ? 'r' : mode === 1 ? 'w+' : 'a+');
    handles.push(fd);
    return handles.length - 1;
  },
  crRead(h, pos, len) {
    const buffer = Buffer.alloc(len);
    const got = fs.readSync(handles[h], buffer, 0, len, pos);
    return buffer.subarray(0, got);
  },
  crWrite(h, pos, bytes) {
    fs.writeSync(handles[h], bytes, 0, bytes.length, pos);
    return 0;
  },
  crSize: (h) => fs.fstatSync(handles[h]).size,
  crClose(h) {
    fs.closeSync(handles[h]);
  },
  crStat(name) {
    const file = where(name);
    return fs.existsSync(file) ? fs.statSync(file).size : -1;
  },
  crUnlink(name) {
    fs.rmSync(where(name), { force: true });
  },
  crList(index) {
    const names = [zipName, ...fs.readdirSync(folder)];
    return index < names.length ? names[index] : '';
  },
  crProgress(b) {
    if (b < bar) backwards++;
    bar = b;
  },
};

const firmware = fs.readFileSync(path.join(pack, 'COMMON.IMK')).subarray(32, 64)
  .toString('latin1').replace(/\0.*$/s, '');
const module = await createModule(host);
const stringOf = (s) => {
  const ptr = module._malloc(s.length + 1);
  module.stringToUTF8(s, ptr, s.length + 1);
  return ptr;
};
let failures = 0;
const check = (ok, what) => {
  if (!ok) {
    failures++;
    console.log(`  FAILED: ${what}`);
  }
};

const fw = stringOf(firmware);
for (const [p, name] of [[0, 'the DMA chip'], [1, 'the YM']]) {
  bar = 0, backwards = 0;
  const t = Date.now();
  const status = module._cr_install(p, fw);
  console.log(`  ${name}'s: status ${status}, ${((Date.now() - t) / 1000).toFixed(1)} s, bar ${bar}`);
  check(status === 0, `${name}'s status`);
  check(backwards === 0, `${name}'s bar went back`);
}
const same = (a, b) => fs.existsSync(a) && fs.existsSync(b) &&
  Buffer.compare(fs.readFileSync(a), fs.readFileSync(b)) === 0;
const images = ['COMMON.IMG', 'MENUS.IMG', 'STAGE1.IMG', 'STAGE2.IMG', 'STAGE3.IMG', 'STAGE4.IMG',
  'STAGE5.IMG', 'STAGE6.IMG', 'STAGE7.IMG', 'START.STA'];
const plain = process.env.CHORENSHA_IMAGES, reference = process.env.CHORENSHA_REFERENCE;
for (const name of plain ? images : []) check(same(path.join(folder, name), path.join(plain, name)), name);
for (const name of reference ? fs.readdirSync(reference) : []) {
  check(same(path.join(folder, name), path.join(reference, name)), name);
}
check(fs.existsSync(path.join(folder, 'INSTALL.TXT')), 'INSTALL.TXT');
const made = fs.readdirSync(folder).filter((n) => !/\.(IMK|STK)$/.test(n));
console.log(`  ${made.length} files made; ${failures} failed`);
process.exit(failures ? 1 : 0);
