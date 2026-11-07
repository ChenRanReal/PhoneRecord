"""Synthetic Qwen orchestration checks; no real audio or model downloads."""
import importlib.util
import json
import tempfile
import unittest
import wave
from pathlib import Path

spec = importlib.util.spec_from_file_location('qwen_worker', Path(__file__).parents[1]/'cpp/speech/qwen-worker.py')
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class QwenWorkerTests(unittest.TestCase):
    """Exercise sidecar separation, resumable checkpoints and waveform bounds."""
    def test_model_size_isolation(self):
        """Both sizes retain independent identities and outputs for the same recording."""
        with tempfile.TemporaryDirectory() as folder:
            wav = Path(folder)/'sizes.wav'
            with wave.open(str(wav),'wb') as out:
                out.setparams((1,2,8000,0,'NONE','not compressed'))
                out.writeframes(b'\0\0'*8000)
            original = wav.read_bytes()
            variants = []
            def factory(root, language, variant):
                """Verify that job selection reaches the actual model loader contract."""
                variants.append(variant)
                return lambda samples: variant + ' result'
            job = dict(root=folder,wav=str(wav),language='auto',progress=str(Path(folder)/'progress.json'))
            worker.run(job,factory)
            small = Path(str(wav)+'.qwen3asr.transcript.json')
            before = small.read_bytes()
            worker.run(dict(job,variant='1.7B'),factory)
            large = json.loads(Path(str(wav)+'.qwen3asr-1.7b.transcript.json').read_text())
            self.assertEqual(variants,['0.6B','1.7B'])
            self.assertIn('1.7b',large['model'])
            self.assertEqual(large['text'],'1.7B result')
            self.assertEqual(small.read_bytes(),before)
            self.assertEqual(wav.read_bytes(),original)
            with self.assertRaisesRegex(ValueError,'invalid_model'):
                worker.run(dict(job,variant='9B'),factory)

    def test_resume_and_preserve_sensevoice(self):
        """A failed second window resumes without touching the SenseVoice version or WAV."""
        with tempfile.TemporaryDirectory() as folder:
            wav = Path(folder)/'synthetic.wav'
            with wave.open(str(wav),'wb') as out:
                out.setparams((1,2,8000,0,'NONE','not compressed'))
                out.writeframes(b'\0\0'*(8000*45))
            original = wav.read_bytes()
            sense = Path(str(wav)+'.transcript.txt'); sense.write_text('Sense version',encoding='utf-8')
            job = dict(root=folder,wav=str(wav),language='zh',progress=str(Path(folder)/'progress.json'))
            calls = []

            def factory(root, language, variant):
                """Provide deterministic results and a deliberate interruption."""
                def decode(samples):
                    calls.append(len(samples))
                    if len(calls) == 2:
                        raise RuntimeError('fixture interruption')
                    return 'Qwen version'
                return decode
            with self.assertRaises(RuntimeError):
                worker.run(job,factory)
            checkpoint = json.loads(Path(str(wav)+'.qwen3asr.transcript.json').read_text())
            self.assertEqual(len(checkpoint['segments']),1)
            calls.clear()
            def retry(root, language, variant):
                """Record resumed sample counts without a second interruption."""
                def decode(samples):
                    calls.append(len(samples)); return 'Qwen resumed'
                return decode
            worker.run(job,retry)
            result = json.loads(Path(str(wav)+'.qwen3asr.transcript.json').read_text())
            self.assertEqual(calls,[320000,80000])
            self.assertEqual(result['status'],'complete')
            self.assertEqual(len(result['segments']),3)
            self.assertEqual(sense.read_text(),'Sense version')
            plain = Path(str(wav)[:-4] + '.qwen.txt').read_text(encoding='utf-8-sig')
            self.assertIn('Qwen resumed', plain)
            self.assertEqual(wav.read_bytes(),original)


if __name__ == '__main__':
    unittest.main()
