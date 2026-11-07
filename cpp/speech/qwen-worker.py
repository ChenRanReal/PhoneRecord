"""Local CPU after-call transcription; preserve independent SenseVoice results."""
import os
for key in ('HF_HUB_OFFLINE', 'TRANSFORMERS_OFFLINE', 'HF_HUB_DISABLE_TELEMETRY'):
    os.environ[key] = '1'
import json
import hashlib
import sys
import wave
import uuid
from array import array
from pathlib import Path
from datetime import datetime, timezone


def readable_transcript(wav, variant):
    """Plain sibling of the recording so the finished text opens in an editor."""
    stem = wav[:-4] if str(wav).lower().endswith('.wav') else str(wav)
    return stem + ('.qwen17.txt' if variant == '1.7B' else '.qwen.txt')


def atomic(file, value):
    """Publish one complete result without modifying the source recording."""
    file = Path(file)
    temp = file.with_name(file.name + '.' + uuid.uuid4().hex + '.pending')
    try:
        temp.write_text(value, encoding='utf-8')
        os.replace(temp, file)
    finally:
        temp.unlink(missing_ok=True)


def progress(job, step, percent=0, detail=''):
    """Publish stable status and bounded, sanitized technical failures."""
    for private in (job.get('root'), job.get('wav'), os.environ.get('USERPROFILE')):
        if private:
            detail = detail.replace(str(private), '[local]')
    atomic(job['progress'], json.dumps(dict(step=step, percent=str(percent), detail=detail[:3500], phase=step)))


def digest(file):
    """Bind checkpoints to the immutable source using bounded streaming reads."""
    with open(file, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def backend(root, language, variant='0.6B'):
    """Load pinned local safetensors on CPU without remote code or network lookup."""
    import torch
    from transformers import AutoProcessor, AutoModelForMultimodalLM
    torch.set_num_threads(4)
    model_path = str(Path(root) / ('models-1.7b' if variant == '1.7B' else 'models'))
    processor = AutoProcessor.from_pretrained(model_path, local_files_only=True, trust_remote_code=False)
    model = AutoModelForMultimodalLM.from_pretrained(model_path, local_files_only=True, trust_remote_code=False, dtype=torch.float32).eval()

    def decode(samples):
        """Decode a 16 kHz waveform with a bounded generation length."""
        import numpy as np
        inputs = processor.apply_transcription_request(audio=np.asarray(samples, dtype=np.float32), language=None if language == 'auto' else language, sampling_rate=16000)
        with torch.inference_mode():
            output = model.generate(**inputs, max_new_tokens=1024, do_sample=False)
        generated = output[0, inputs['input_ids'].shape[1]:]
        if generated.numel() >= 1024:
            raise RuntimeError('Generation reached token limit')
        return processor.decode(generated, return_format='transcription_only').strip()
    return decode


def run(job, factory=backend):
    """Transcribe bounded 20-second windows with source-bound resumable checkpoints."""
    if job.get('language') not in ('auto', 'zh', 'en', 'yue', 'ja', 'ko'):
        raise ValueError('invalid_language')
    variant = job.get('variant', '0.6B')
    if variant not in ('0.6B', '1.7B'):
        raise ValueError('invalid_model')
    progress(job, 'load')
    decode = factory(job['root'], job['language'], variant)
    if job.get('prepare'):
        progress(job, 'ready', 100)
        return
    wav = job['wav']
    source = digest(wav)
    suffix = '.qwen3asr-1.7b.transcript' if variant == '1.7B' else '.qwen3asr.transcript'
    output = Path(wav + suffix + '.json')
    text_file = Path(wav + suffix + '.txt')
    with wave.open(wav, 'rb') as audio:
        rate = audio.getframerate()
        frames = audio.getnframes()
        if audio.getnchannels() != 1 or audio.getsampwidth() != 2 or rate not in (8000, 16000) or audio.getcomptype() != 'NONE' or frames > rate * 12 * 3600:
            raise ValueError('wav')
        identity = 'qwen3-asr-1.7b-hf-bcd2b5b7' if variant == '1.7B' else 'qwen3-asr-0.6b-hf-7f1569a4'
        result = dict(schema=1, model=identity, engine='transformers-5.13.0', language=job['language'], sourceSha256=source, duration=frames/rate, status='running', segments=[], text='')
        offset = 0
        try:
            if output.stat().st_size <= 16*1024*1024:
                old = json.loads(output.read_text('utf-8'))
                if all(old.get(k) == result[k] for k in ('model','engine','language','sourceSha256')) and old.get('status') != 'complete':
                    end = 0
                    for seg in old['segments']:
                        if not isinstance(seg['text'], str) or seg['start'] != end or not end < seg['end'] <= frames/rate:
                            raise ValueError('checkpoint')
                        end = seg['end']
                    offset = round(end * rate)
                    result['segments'] = old['segments']
        except (OSError, ValueError, KeyError, TypeError):
            pass

        def save():
            """Keep each completed window durable in the selected model's own files."""
            result['text'] = '\n'.join(s['text'] for s in result['segments'] if s['text'])
            atomic(text_file, result['text'])
            atomic(readable_transcript(wav, variant), '\ufeff' + result['text'].replace('\n', '\r\n'))
            atomic(output, json.dumps(result, ensure_ascii=False))
        save()
        while offset < frames:
            audio.setpos(offset)
            count = min(rate*20, frames-offset)
            raw = audio.readframes(count)
            if len(raw) != count*2:
                raise ValueError('wav')
            pcm = array('h', raw)
            if sys.byteorder != 'little':
                pcm.byteswap()
            samples = array('f', (value/32768 for value in pcm))
            if rate == 8000:
                resampled = array('f')
                for index, value in enumerate(samples):
                    resampled.extend((value, (value+samples[min(index+1,len(samples)-1)])/2))
                samples = resampled
            text = decode(samples)
            result['segments'].append(dict(start=offset/rate, end=(offset+count)/rate, text=text))
            offset += count
            save()
            progress(job, 'transcribing', offset*100//max(1, frames))
        if digest(wav) != source:
            raise ValueError('source_changed')
        result['status'] = 'complete'
        result['completedAt'] = datetime.now(timezone.utc).isoformat()
        save()
        progress(job, 'complete', 100)


if __name__ == '__main__':
    if sys.argv[1:] == ['--check-runtime']:
        import torch, numpy, soundfile
        from transformers import AutoProcessor, AutoModelForMultimodalLM, Qwen3ASRForConditionalGeneration
        print('Qwen3-ASR CPU imports ready')
    else:
        job = json.loads(Path(sys.argv[1]).read_text('utf-8'))
        try:
            run(job)
        except Exception as error:
            step = str(error) if str(error) in ('wav', 'source_changed', 'invalid_language') else 'inference_failed'
            progress(job, step, detail=type(error).__name__ + ': ' + str(error))
            sys.exit(1)
