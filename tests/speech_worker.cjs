/** Synthetic audio + injected deterministic decoder; no model downloads or real recordings. */
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { run, wave } = require('../cpp/speech/worker.cjs');

/** Write a real RIFF file including an extra chunk to catch fixed-header assumptions. */
function wav(file, seconds, rate = 8000, odd = false) {
  const data = Buffer.alloc(seconds * rate * 2);
  for (let i = 0; i < data.length / 2; i++) data.writeInt16LE(Math.round(Math.sin(i / 30) * 1000), i * 2);
  const h = Buffer.alloc(56); h.write('RIFF'); h.writeUInt32LE(h.length + data.length - 8, 4); h.write('WAVEfmt ', 8);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22); h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34);
  h.write('JUNK', 36); h.writeUInt32LE(odd ? 3 : 4, 40); h.write('data', 48); h.writeUInt32LE(data.length, 52);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}
/** A speech detector with continuous 30s segments records the actual resampled sample count. */
function backend(options = {}) {
  const stats = { samples: 0, decoded: 0, maxBuffer: 0 };
  let queued = [], offset = 0, buffer = [];
  const factory = () => ({
    recognizer: {
      createStream() { return { acceptWaveform(audio) { assert.equal(audio.sampleRate, 16000); stats.maxBuffer = Math.max(stats.maxBuffer, audio.samples.length); } }; },
      decode() { stats.decoded++; if (stats.decoded === options.failAt) throw Error('fixture crash'); },
      getResult() { return { text: '測試結果 ' + stats.decoded }; },
    },
    detector: {
      acceptWaveform(samples) {
        stats.samples += samples.length; buffer.push(...samples);
        if (buffer.length >= (options.seconds || 30) * 16000) this.segment();
      },
      segment() { if (!buffer.length) return; queued.push({ start: offset, samples: Float32Array.from(buffer) }); offset += buffer.length; buffer = []; },
      isEmpty() { return !queued.length; }, front() { return queued[0]; }, pop() { queued.shift(); }, flush() { this.segment(); },
    },
  });

  return { stats, factory };
}
/** Isolate all source and result paths and remove only this owned temporary directory. */
async function fixture(fn) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'PhoneRecord-speech-worker-'));
  try { await fn(root); } finally { fs.rmSync(root, { recursive: true, force: true }); }
}
test('8 kHz long WAV crosses DHS request limits without losing samples or rewriting audio', async () => fixture(async root => {
  const file = path.join(root, '合成長通話.wav'); wav(file, 150);
  const before = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  const { stats, factory } = backend(); const progress = path.join(root, 'progress.json');
  await run({ root, wav: file, language: 'auto', progress }, factory);
  const result = JSON.parse(fs.readFileSync(file + '.transcript.json'));
  assert.equal(stats.samples, 150 * 16000); assert.equal(stats.decoded, 5); assert.ok(stats.maxBuffer <= 30.3 * 16000 + 512);
  assert.equal(result.status, 'complete'); assert.equal(result.segments.at(-1).end, 150);
  assert.equal(result.sourceSha256, before); assert.equal(await require('../cpp/speech/worker.cjs').digest(file), before);
  assert.equal(JSON.parse(fs.readFileSync(progress)).step, 'complete');
  assert.match(fs.readFileSync(file + '.transcript.txt', 'utf8'), /測試結果/);
  const plain = fs.readFileSync(file.replace(/\.wav$/i, '.live.txt'), 'utf8');
  assert.equal(plain.charCodeAt(0), 0xFEFF);
  assert.match(plain, /測試結果/);
}));
test('failed inference checkpoints complete segments and explicit retry skips them', async () => fixture(async root => {
  const file = path.join(root, 'partial.wav'); wav(file, 75);
  const job = { root, wav: file, language: 'zh', progress: path.join(root, 'progress.json') };
  await assert.rejects(run(job, backend({ failAt: 2 }).factory));
  let result = JSON.parse(fs.readFileSync(file + '.transcript.json'));
  assert.equal(result.status, 'running'); assert.equal(result.segments.length, 1);
  const retry = backend(); await run(job, retry.factory);
  result = JSON.parse(fs.readFileSync(file + '.transcript.json'));
  assert.equal(retry.stats.decoded, 2); assert.equal(result.segments.length, 3); assert.equal(result.status, 'complete');
}));
test('16 kHz input passes unchanged and language changes invalidate old checkpoints', async () => fixture(async root => {
  const file = path.join(root, 'native.wav'); wav(file, 2, 16000, true);
  const job = { root, wav: file, language: 'en', progress: path.join(root, 'progress.json') };
  const fixture = backend(); await run(job, fixture.factory); assert.equal(fixture.stats.samples, 32000);
  await assert.rejects(run({ ...job, language: 'es' }, fixture.factory), /invalid_language/);
}));
test('a wetype sidecar is not resumed as a SenseVoice checkpoint', async () => fixture(async root => {
  const file = path.join(root, 'ime.wav'); wav(file, 30);
  fs.writeFileSync(file + '.transcript.json', JSON.stringify({ schema: 1, model: 'wetype', engine: 'wetype-ime', language: 'zh', mode: 'live', sourceSha256: '', duration: 30, status: 'running', segments: [{ start: 0, end: 30, text: '輸入法舊稿' }], text: '輸入法舊稿' }));
  const { stats, factory } = backend({ seconds: 30 });
  await run({ root, wav: file, language: 'zh', progress: path.join(root, 'progress.json') }, factory);
  const result = JSON.parse(fs.readFileSync(file + '.transcript.json', 'utf8'));
  assert.equal(result.model, 'sensevoice-int8-2024-07-17');
  assert.equal(stats.decoded, 1);
  assert.doesNotMatch(result.text, /輸入法舊稿/);
}));
test('truncated RIFF and unsupported sample rates fail before reading PCM', async () => fixture(async root => {
  for (const rate of [8000, 44100]) {
    const file = path.join(root, rate + '.wav'); wav(file, 1, rate);
    if (rate === 8000) fs.truncateSync(file, 100);
    const fd = fs.openSync(file, 'r'); try { assert.throws(() => wave(fd), /wav/); } finally { fs.closeSync(fd); }
  }
}));

test('live growing WAV publishes before hangup and drains the final tail without changing audio', async () => fixture(async root => {
  const file = path.join(root,'live.wav'), source = path.join(root,'source.wav'); wav(source,5);
  const original = fs.readFileSync(source);
  // The native writer uses a canonical 44-byte header and patches lengths at close.
  const header = Buffer.concat([original.subarray(0,36), original.subarray(48,56)]);
  const pcm = original.subarray(56); header.writeUInt32LE(36,4); header.writeUInt32LE(0,40);
  fs.writeFileSync(file,header);
  const job = {root,wav:file,language:'auto',progress:path.join(root,'progress.json'),live:true,stopFile:path.join(root,'closed')};
  const fixture = backend({seconds:1});
  const running = run(job,fixture.factory);
  fs.appendFileSync(file,pcm.subarray(0,32000));
  await new Promise(resolve => setTimeout(resolve,250));
  const partial = JSON.parse(fs.readFileSync(file + '.transcript.json'));
  assert.equal(partial.status,'running'); assert.ok(partial.segments.length >= 1);
  assert.match(fs.readFileSync(file.replace(/\.wav$/i, '.live.txt'), 'utf8'), /測試結果/);
  assert.equal(partial.sourceSha256,'');
  fs.appendFileSync(file,pcm.subarray(32000));
  header.writeUInt32LE(pcm.length+36,4); header.writeUInt32LE(pcm.length,40);
  const fd=fs.openSync(file,'r+'); fs.writeSync(fd,header,0,44,0); fs.closeSync(fd);
  const before=fs.readFileSync(file); fs.writeFileSync(job.stopFile,'closed'); await running;
  const result=JSON.parse(fs.readFileSync(file + '.transcript.json'));
  assert.equal(result.status,'complete'); assert.equal(result.mode,'live');
  assert.equal(result.duration,5); assert.equal(fixture.stats.samples,80000);
  assert.equal(result.segments.at(-1).end,5); assert.deepEqual(fs.readFileSync(file),before);
  assert.equal(result.sourceSha256,crypto.createHash('sha256').update(before).digest('hex'));
}));
