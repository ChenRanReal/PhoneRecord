# Speech runtime notices

The original PhoneRecord worker uses the CPU SenseVoice / Silero configuration and preparation workflow described by DeepSeek Harness 0.2.2-rc.1. DeepSeek Harness code is available under the following MIT license. PhoneRecord retains its existing repository license.

MIT License

Copyright (c) 2026 DeepSeek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Node.js includes its license and third-party notices inside the verified ZIP.
sherpa-onnx 1.13.8 is Apache-2.0; ONNX Runtime and Silero VAD are MIT.
Keep the notices included in the runtime packages when redistributing them.

SenseVoiceSmall model weights have their own terms, independent of the MIT
code license. Source: FunAudioLLM / FunASR, ONNX conversion by csukuangfj.
See https://huggingface.co/FunAudioLLM/SenseVoiceSmall and
https://github.com/modelscope/FunASR/blob/main/MODEL_LICENSE .
The model bytes are not bundled into the PhoneRecord executable.


## Optional Qwen3-ASR CPU runtime

Qwen/Qwen3-ASR-0.6B-hf, revision 7f1569a48a89f3e3f4dc3a5c9d28bddd903bc76c,
is published under Apache-2.0: https://huggingface.co/Qwen/Qwen3-ASR-0.6B-hf .
Weights are downloaded only by explicit user action and are not bundled.
Python 3.12.10 (PSF license), Transformers 5.13.0 (Apache-2.0),
PyTorch 2.9.1 CPU (BSD-style), NumPy (BSD) and SoundFile (BSD) are installed
as separate, pinned upstream packages. Their distributions retain upstream
licenses and third-party notices. The application uses native Transformers
support with local safetensors and does not execute model repository code.

Qwen/Qwen3-ASR-1.7B-hf is also available by explicit installation, under Apache-2.0:
https://huggingface.co/Qwen/Qwen3-ASR-1.7B-hf , revision bcd2b5b7f32b480ab5790554cfa8347f246a14f3.
It shares the pinned CPU runtime but uses separate weights, readiness and results.
