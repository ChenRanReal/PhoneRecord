/** Local-only SenseVoice job runner. No network access; all audio stays on disk/in memory. */
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

/** Plain sibling of a recording, opened directly from the recordings folder. */
function readableTranscript(wav) {
  return wav.replace(/\.wav$/i, '') + '.live.txt';
}
/** Atomically publish UTF-8 metadata; never modify the source WAV. */
function atomic(file, value) {
  const temp = file + '.' + crypto.randomBytes(8).toString('hex') + '.pending';
  try {
    fs.writeFileSync(temp, value, { encoding: 'utf8', mode: 0o600, flag: 'wx' });
    fs.renameSync(temp, file);
  } finally { if (fs.existsSync(temp)) fs.unlinkSync(temp); }
}
/** Publish stable codes instead of raw library exceptions or personal transcript text. */
function progress(file, step, percent = 0) { atomic(file, JSON.stringify({ step, percent: String(percent) })); }
/** Read an exact byte range from an already opened file. */
function read(fd, count, offset) {
  const b = Buffer.alloc(count);
  if (fs.readSync(fd, b, 0, count, offset) !== count) throw Error('wav');
  return b;
}
/** Walk RIFF chunks instead of assuming a 44-byte WAV header. */
function wave(fd) {
  const size = fs.fstatSync(fd).size, h = read(fd, 12, 0);
  if (h.toString('ascii', 0, 4) !== 'RIFF' || h.toString('ascii', 8, 12) !== 'WAVE' || h.readUInt32LE(4) + 8 !== size) throw Error('wav');
  let format, data;
  for (let p = 12; p + 8 <= size;) {
    const c = read(fd, 8, p), n = c.readUInt32LE(4), id = c.toString('ascii', 0, 4);
    if (p + 8 + n > size) throw Error('wav');
    if (id === 'fmt ') {
      if (n < 16) throw Error('wav');
      const f = read(fd, 16, p + 8);
      format = { type: f.readUInt16LE(0), channels: f.readUInt16LE(2), rate: f.readUInt32LE(4), bits: f.readUInt16LE(14) };
    }
    if (id === 'data') data = { offset: p + 8, bytes: n };
    p += 8 + n + (n % 2);
  }
  if (!format || !data || format.type !== 1 || format.channels !== 1 || format.bits !== 16 || ![8000, 16000].includes(format.rate) || data.bytes % 2) throw Error('wav');
  return { ...format, ...data };
}
/** Hash by streaming so long calls do not require whole-file buffers. */
async function digest(file) {
  const hash = crypto.createHash('sha256');
  const stream = typeof file === 'number' ? fs.createReadStream(null, { fd: file, autoClose: false, start: 0 }) : fs.createReadStream(file);
  for await (const b of stream) hash.update(b);
  return hash.digest('hex');
}
/** Construct the same CPU SenseVoice / Silero pair used by DHS 0.2.2-rc.1. */
function models(root, language, live = false) {
  const sherpa = require(path.join(root, 'runtime', 'node_modules', 'sherpa-onnx-node'));
  const recognizer = new sherpa.OfflineRecognizer({
    featConfig: { sampleRate: 16000, featureDim: 80 },
    modelConfig: { senseVoice: { model: path.join(root, 'models', 'model.int8.onnx'), language, useInverseTextNormalization: 1 },
      tokens: path.join(root, 'models', 'tokens.txt'), numThreads: 2, provider: 'cpu', debug: 0 },
  });
  const detector = new sherpa.Vad({ sileroVad: { model: path.join(root, 'models', 'silero_vad.onnx'), threshold: 0.5,
    minSilenceDuration: 0.5, minSpeechDuration: 0.25, maxSpeechDuration: live ? 3 : 30, windowSize: 512 }, sampleRate: 16000, numThreads: 2, provider: 'cpu', debug: 0 }, 32);
  return { recognizer, detector };
}
/** Execute one bounded-memory job, checkpointing complete VAD segments for explicit retries. */
async function run(job, backend = models) {
  if (!['auto', 'zh', 'en', 'yue', 'ja', 'ko'].includes(job.language)) throw Error('invalid_language');
  progress(job.progress, 'load');
  const { recognizer, detector } = await backend(job.root, job.language, !!job.live);
  if (job.prepare) { progress(job.progress, 'ready', 100); return; }
  const fd = fs.openSync(job.wav, 'r');
  try {
    // The recorder writes a canonical PCM header, patched only when the call closes.
    const header = job.live ? read(fd, 44, 0) : null;
    if (header && (header.toString('ascii', 0, 4) !== 'RIFF' || header.toString('ascii', 8, 16) !== 'WAVEfmt ' || header.readUInt16LE(20) !== 1 || header.readUInt16LE(22) !== 1 || header.readUInt32LE(24) !== 8000 || header.readUInt16LE(34) !== 16 || header.toString('ascii',36,40) !== 'data')) throw Error('wav');
    const info = job.live ? {offset:44, bytes:0, rate:8000} : wave(fd), source = job.live ? '' : await digest(fd);
    if (info.bytes / 2 / info.rate > 12 * 3600) throw Error('wav');
    const output = job.wav + '.transcript.json', textFile = job.wav + '.transcript.txt';
    const result = { schema: 1, model: 'sensevoice-int8-2024-07-17', engine: 'sherpa-onnx-1.13.8', language: job.language,
      mode: job.live ? 'live' : 'file', sourceSha256: source, duration: info.bytes / 2 / info.rate, status: 'running', segments: [], text: '' };
    let checkpoint = 0;
    try {
      if (fs.statSync(output).size > 16 * 1024 * 1024) throw Error('metadata');
      const old = JSON.parse(fs.readFileSync(output, 'utf8'));
      if (!job.live && old.sourceSha256 === source && old.model === result.model && old.engine === result.engine && old.language === result.language && old.status !== 'complete' && Array.isArray(old.segments)) {
        let end = 0;
        if (!old.segments.every(s => {
          if (typeof s.text !== 'string' || !Number.isFinite(s.start) || !Number.isFinite(s.end) || s.start < 0 || s.end <= end || s.end < s.start || s.end > result.duration + 0.01) return false;
          end = s.end; return true;
        })) throw Error('metadata');
        result.segments = old.segments;
        checkpoint = result.segments.at(-1)?.end || 0;
      }
    } catch {}
    /** Commit only finished segments, retaining a resumable state after a killed worker. */
    const save = () => {
      result.text = result.segments.map(s => s.text).filter(Boolean).join('\n');
      atomic(textFile, result.text);
      atomic(readableTranscript(job.wav), '\uFEFF' + result.text.replace(/\n/g, '\r\n'));
      atomic(output, JSON.stringify(result));
    };
    save();
    let fed = 0, first = true;
    const ring = new Float32Array(34 * 16000);
    /** Drain VAD continuously across I/O blocks so file chunk edges do not cut words. */
    const drain = () => {
      while (!detector.isEmpty()) {
        const segment = detector.front(false);
        const begin = Math.max(0, segment.start - (first ? 4800 : 0)); first = false;
        const end = segment.start + segment.samples.length;
        if (end / 16000 > checkpoint + 0.001) {
          const restored = new Float32Array(end - begin);
          for (let i = begin; i < segment.start; i++) restored[i - begin] = ring[i % ring.length];
          restored.set(segment.samples, segment.start - begin);
          const stream = recognizer.createStream(); stream.acceptWaveform({ sampleRate: 16000, samples: restored });
          recognizer.decode(stream);
          result.segments.push({ start: begin / 16000, end: end / 16000, text: recognizer.getResult(stream).text.trim() });
          save();
        }
        detector.pop();
      }
    };
    // Linear 2x interpolation is a deterministic PCM adapter, not an audio enhancement.
    // Keep the next source sample across I/O blocks, preventing a discontinuity at each read.
    let total = info.bytes / 2; const block = 256;
    for (let offset = 0;;) {
      let closed = true;
      if (job.live) {
        closed = fs.existsSync(job.stopFile);
        total = Math.floor((fs.fstatSync(fd).size - info.offset) / 2);
        if (total > 12 * 3600 * info.rate) throw Error('wav');
        result.duration = total / info.rate;
        if (!closed && total - offset < block + 1) { await new Promise(resolve => setTimeout(resolve, 100)); continue; }
      }
      if (offset >= total) break;
      const n = Math.min(block, total - offset), extra = offset + n < total ? 1 : 0;
      const pcm = read(fd, (n + extra) * 2, info.offset + offset * 2);
      const samples = new Float32Array(n * (info.rate === 8000 ? 2 : 1));
      for (let i = 0; i < n; i++) {
        const value = pcm.readInt16LE(i * 2) / 32768;
        if (info.rate === 8000) { samples[i * 2] = value; samples[i * 2 + 1] = (value + pcm.readInt16LE(Math.min(i + 1, n + extra - 1) * 2) / 32768) / 2; }
        else samples[i] = value;
      }
      for (const value of samples) ring[fed++ % ring.length] = value;
      detector.acceptWaveform(samples); drain();
      if (offset % (block * 64) === 0) progress(job.progress, job.live ? 'live' : 'transcribing', job.live ? 0 : Math.floor((offset + n) * 100 / total));
      offset += n;
    }
    detector.flush(); drain();
    // Recheck source identity before publishing, including changes made during recognition.
    if (job.live) { wave(fd); result.sourceSha256 = await digest(fd); if (await digest(job.wav) !== result.sourceSha256) throw Error('source_changed'); }
    else if (await digest(job.wav) !== source) throw Error('source_changed');
    result.status = 'complete'; result.completedAt = new Date().toISOString();
    atomic(textFile, result.segments.map(s => s.text).filter(Boolean).join('\n'));
    save(); progress(job.progress, 'complete', 100);
  } finally { fs.closeSync(fd); }
}

if (require.main === module) {
  let job;
  Promise.resolve().then(() => { job = JSON.parse(fs.readFileSync(process.argv[2], 'utf8')); return run(job); }).catch(error => {
    if (job?.progress) progress(job.progress, ['wav', 'invalid_language', 'source_changed'].includes(error.message) ? error.message : 'inference_failed');
    process.exitCode = 1;
  });
}
module.exports = { wave, run, digest };
