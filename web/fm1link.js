// SPDX-License-Identifier: GPL-3.0-only
// A tune to FM-1 SID on a real FM-1, over USB (WebMIDI): fm1-dock's MODULE frames, as its
// tools/dock.py send does (firmware/dock/hb_module.h, "a file to a module"). B [size][subtune],
// D [at][bytes] in order, E: FM-1 SID writes the tune into its C64's memory as it arrives and plays
// it. Nothing goes to flash; the tune is gone at the next one. Every frame is answered; one that
// gets no answer is sent again. The same frames into the DIN jack (no answers, the jack's pace)
// reach the FM-1 on this page (dinFrames).
//   frame    F0 7D 48 42 cmd seq pack7(payload + CRC-32 of the payload, little endian) F7
//   replies  cmd | 0x40, the same seq; the payload starts with a status byte

const MAN = [0x7d, 0x48, 0x42];
const MODULE = 0x23;
const CHUNK = 256;
const NAMES = ["felucca", "fm1-dock", "harbor", "fm-1"];   // the USB-MIDI names fm1-dock goes by
export const isFm1Port = (name) => NAMES.some((n) => name.toLowerCase().includes(n));

// several listeners on one input (an FM-1's port carries the answers to the frames and its keys' notes):
// one onmidimessage handing every message to each (setting it opens the port; a listener alone did not
// get messages in Chrome), and the port opened explicitly as well
const taps = new WeakMap();
export function listen(input, key, fn) {
  let t = taps.get(input);
  if (!t) {
    t = new Map();
    taps.set(input, t);
  }
  if (fn) t.set(key, fn); else t.delete(key);
  input.onmidimessage = t.size ? (ev) => { for (const f of t.values()) f(ev); } : null;
  if (t.size) input.open().catch(() => {});
}
const STATUS = ["done", "bad CRC", "bad header", "too big", "no space", "flash error", "verify failed",
  "data missing", "not started", "not found", "bad message", "refused"];
// FM-1 SID's own refusals (firmware/fm1sid.c usb_file)
const REFUSED = ["", "a bad frame", "the GoatTracker editor is open on the FM-1", "not a tune FM-1 SID plays",
  "a frame out of order"];

const CRC_TAB = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(b) {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = CRC_TAB[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function pack7(b) {                     // 8-to-7 bits, LSB first (fm1-dock's ota_pack7)
  const out = [];
  let acc = 0, nb = 0;
  for (const x of b) {
    acc |= x << nb;
    nb += 8;
    while (nb >= 7) { out.push(acc & 0x7f); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7f);
  return out;
}
function unpack7(b) {
  const out = [];
  let acc = 0, nb = 0;
  for (const x of b) {
    acc |= (x & 0x7f) << nb;
    nb += 7;
    if (nb >= 8) { out.push(acc & 0xff); acc >>>= 8; nb -= 8; }
  }
  return Uint8Array.from(out);
}
const u32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];

function frame(seq, payload) {
  const body = Uint8Array.from([...payload, ...u32(crc32(payload))]);
  return Uint8Array.from([0xf0, ...MAN, MODULE, seq & 0x7f, ...pack7(body), 0xf7]);
}

// the payloads that send a .sid (song 0: the tune's own start song)
export function tuneSteps(bytes, song = 0) {
  const steps = [Uint8Array.from([0x42, ...u32(bytes.length), ...u32(song)])];
  for (let at = 0; at < bytes.length; at += CHUNK)
    steps.push(Uint8Array.from([0x44, ...u32(at), ...bytes.subarray(at, at + CHUNK)]));
  steps.push(Uint8Array.of(0x45));
  return steps;
}

// what fm1-dock can take: a PSID or RSID file (GoatTracker songs: not yet)
export function sendable(bytes) {
  const kind = String.fromCharCode(...bytes.subarray(0, 4));
  if (kind !== "PSID" && kind !== "RSID") return "only .sid tunes can be sent to an FM-1 (GoatTracker songs: not yet)";
  if (bytes.length > 0x7e + 65536) return "too big for a C64";
  return null;
}

// the same frames, one after the other, for the DIN jack of the FM-1 on this page
export function dinFrames(bytes, song = 0) {
  return tuneSteps(bytes, song).map((p, i) => frame(i + 1, p));
}

function why(d) {
  if (!d || !d.length) return "no answer";
  if (d[0] === 11) return REFUSED[d[1]] || `FM-1 SID's error ${d[1]}`;
  if (d[0] === 9) return "nothing on the FM-1 takes tunes: is FM-1 SID playing (an fm1-dock from 0.6)?";
  return STATUS[d[0]] || `status ${d[0]}`;
}

export class FM1Link {
  constructor(input, output) {
    this.input = input;
    this.output = output;
    this.seq = 0;
    this.waiting = null;
    listen(input, "fm1link", (ev) => this.receive(ev.data));
  }

  // the first FM-1 among the MIDI ports (null: none); want: a port whose name has this in it instead
  static async open(access, want = null) {
    const names = want ? [want.toLowerCase()] : NAMES;
    const pick = (ports) => {
      for (const n of names) for (const p of ports.values()) if (p.name.toLowerCase().includes(n)) return p;
      return null;
    };
    const input = pick(access.inputs), output = pick(access.outputs);
    if (!input || !output) return null;
    await input.open();                 // (both opened before the first frame: the answers come back on input)
    await output.open();
    return new FM1Link(input, output);
  }

  get name() { return this.output.name; }
  close() { listen(this.input, "fm1link", null); }

  receive(data) {
    if (data.length < 7 || data[0] !== 0xf0 || data[1] !== MAN[0] || data[2] !== MAN[1] || data[3] !== MAN[2]) return;
    const w = this.waiting;
    if (!w || data[5] !== w.seq || (data[4] !== (MODULE | 0x40) && data[4] !== 0x7f)) return;
    const b = unpack7(data.subarray(6, data.length - 1));
    if (b.length < 4) return;
    const p = b.subarray(0, b.length - 4);
    const c = b[b.length - 4] | b[b.length - 3] << 8 | b[b.length - 2] << 16 | b[b.length - 1] << 24;
    if ((c >>> 0) !== crc32(p)) return;
    this.waiting = null;
    w.done(p);
  }

  ask(payload, timeout = 1000) {
    this.seq = (this.seq + 1) & 0x7f;
    const seq = this.seq;
    return new Promise((done) => {
      const timer = setTimeout(() => { if (this.waiting?.seq === seq) { this.waiting = null; done(null); } }, timeout);
      this.waiting = { seq, done: (p) => { clearTimeout(timer); done(p); } };
      this.output.send(frame(seq, payload));
    });
  }

  // a .sid to FM-1 SID, played as soon as it is all there; throws with the reason it was not taken
  async sendTune(bytes, song = 0, progress = () => {}) {
    const bad = sendable(bytes);
    if (bad) throw new Error(bad);
    const steps = tuneSteps(bytes, song);
    for (let i = 0; i < steps.length; i++) {
      let d = null;
      for (let t = 0; t < 3 && !d; t++) d = await this.ask(steps[i]);
      if (!d || d[0] !== 0) throw new Error(why(d));
      progress((i + 1) / steps.length);
    }
  }
}
