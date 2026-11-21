# PhoneRecord 2.8.2

2.8.0 與 2.8.1 的程式都已在 `main`，標籤也已推送，但兩次發布都在上傳前失敗。**2.8.0 與 2.8.1 都沒有 GitHub Release。** 這次以 2.8.2 發出那兩版的全部內容。產品行為與 2.8.1 相同；2.8.2 只調整錄音冒煙測試的等待與逾時診斷。

## 2.8.2 測試

`start_guard_invalidation` 與其他登入確認（`AUTH_VERIFIED`、`AUTH_CACHE_VALID`）的等待上限改為 84.1 秒。依據是 Fanvil 路徑的實際上限：一次登入最多 4 次 HTTP（GET `/`、GET nonce、POST `/`、GET 受保護頁），單次讀取最多 6 秒，後續每次交換前等待 1100 毫秒，最多重試 3 輪，輪與輪之間還有 300 毫秒乘以輪次的退避；第 2、3 輪開頭的 1100 毫秒間隔與退避重疊的部分不重複計算。這是慢速登入的上限，不是平常耗時。

逾時斷言現在寫出等待目標、已等待時間，以及當時日誌最後 40 行。所有冒煙用例共用這一個 `wait_for`。

## 2.8.1 修正

2.8.0 的程式已合併，但離線打包在準備 SenseVoice 時失敗，發布步驟沒有執行。2.8.1 修正打包，發布則在 `start_guard_invalidation` 等待 `AUTH_VERIFIED` 滿 10 秒時失敗，同樣沒有建立 Release。同一提交上的 main 校驗以約 16.7 秒通過該用例。

標準執行檔的自動更新方式沒有改：仍只替換 exe，簽章格式、公鑰與 64 MB 上限維持原樣。

離線打包會先把安裝腳本要讀的檔案放進暫存安裝根目錄，再執行腳本：

- SenseVoice 的 `install.ps1` 從安裝根目錄執行 `worker.cjs`，來源是 `cpp/speech/worker.cjs`。
- Qwen 的安裝腳本從自己旁邊讀取 `install.ps1`、`qwen-assets.json` 與 `qwen-worker.py`。這三個檔與 `qwen-install.ps1` 會先複製到暫存根目錄，再從那裡執行。

這些工具檔不放進給使用者的壓縮包。

## 可載入的話機型號元件

錄音、事件與話機網頁不再把 Fanvil X3S 寫死在核心裡。型號差異放在可匹配的元件中。

- Fanvil X3S 是內建元件 `fanvil.x3s`，也是沒有更強證據時的回退。既有設定不必修改。
- 啟動時再讀取執行檔旁邊的 `plugins` 目錄，Windows 為 `*.dll`，最多 32 個。缺少這個目錄不是錯誤。相同 id 的外部庫不能替換內建 X3S。
- `config.ini` 的 `device_model` 可釘住型號 id。留空則自動匹配；未知 id 仍回退到 X3S。
- 自動更新只替換 `PhoneRecord.exe`，不會清掉 `plugins`。

## 離線安裝包

標準的 `PhoneRecord.exe` 與 `PhoneRecord-x64.exe` 仍是按需下載語音模型。另有 x64 離線壓縮包，把已按長度與 SHA-256 核對的模型放在 `bundled-speech`。x86 沒有語音功能，因此沒有離線變體。

- `PhoneRecord-x64-offline-sensevoice.zip`：SenseVoice。權重約 241 MB。
- `PhoneRecord-x64-offline-qwen06.zip`：Qwen3-ASR 0.6B。成品小於 GitHub 每個資產 2 GiB 才上傳；否則上傳 `PhoneRecord-x64-offline-qwen06.skipped.txt`，不阻擋這次發布。
- Qwen 1.7B 權重本身已超過 2 GiB，不放入 Release。本機腳本仍可打包，供隨身碟散發。

模型包使用另一份 `PhoneRecord-modelpack-v1` 清單與同一把公鑰。舊版用戶端不讀這份清單。模型包下載或簽章失敗時，不擋住 exe 更新，也不先刪除已有模型。

## 錄音冒煙測試

合成錄音測試不再匯入已刪除的 Python `src/g711.py`。它改為把程式寫出的 WAV 與送入的 G.711 載荷對照。

## 尚未完成的確認

這次沒有在真機上重做通話，也沒有在斷網的 Windows 上解壓離線包後做辨識。上線使用前仍須確認：

- Fanvil X3S：網頁登入與 cookie、一鍵設定兩條 Action URL、DHCP 的啟動完成與 IP 變更回呼、接通起錄、掛斷收尾、通訊錄讀取。
- 離線包：斷網冷啟動後能否直接辨識，以及自動更新實際下載並替換模型包。Qwen 0.6B 成品的位元組數以本 Release 的資產為準。

CI 能打出壓縮包，不能代替上述確認。虛擬聲卡驅動不在任何安裝包內。這次沒有改寫 Windows 預設音訊裝置。

**完整變更**：https://github.com/ChenRanReal/PhoneRecord/compare/v2.7.7...v2.8.2

## 國際化與多語言交付

客戶端仍是簡體中文、繁體中文、English，沒有新增語系。預設語言仍由既有 Windows UI 語言規則選擇，之後使用既有的使用者偏好。這份發布說明是繁體中文原文，程式不會自動翻譯。型號 id、檔名、雜湊與變體名稱維持 ASCII，不用翻譯文字當識別值。2.8.1 與 2.8.2 都沒有新增介面字串。
