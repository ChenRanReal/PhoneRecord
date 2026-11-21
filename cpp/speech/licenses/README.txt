離線安裝包隨附的模型與執行環境授權。請與權重一起保存，不要刪除本目錄。

- SenseVoiceSmall（model.int8.onnx、tokens.txt）：FunASR 模型授權，見 FunASR-MODEL-LICENSE.txt。來源為 FunAudioLLM / FunASR，ONNX 轉換為 csukuangfj。散布時須保留來源、作者與模型名稱。
- silero_vad.onnx：MIT，見 SILERO-MIT.txt。
- Qwen3-ASR 0.6B / 1.7B 權重：Apache-2.0，見 Apache-2.0.txt。0.6B revision 7f1569a48a89f3e3f4dc3a5c9d28bddd903bc76c；1.7B revision bcd2b5b7f32b480ab5790554cfa8347f246a14f3。
- sherpa-onnx 1.13.8：Apache-2.0。ONNX Runtime：MIT。Node.js 24.11.0 的授權在 node 目錄內的 LICENSE。
- Python、PyTorch CPU、Transformers 與其依賴沿用各自上游授權，位於 qwen/python 內，僅在對應的 Qwen 離線包中出現。

模型位元組不進入 Git。正式安裝檔由打包腳本在建置時下載並以 cpp/offline_rules.hpp 的長度與 SHA-256 核對。
