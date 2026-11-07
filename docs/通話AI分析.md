# 通話 AI 分析（x64）

## 使用方式與範圍

通話 AI 分析是高階功能，預設關閉。CLI 頁的「一鍵安裝」「登入」「更新模型」「測試連線」可直接使用，並會一併打開此開關；「AI 分析」仍須開關為開啟。關閉後停止進行中的安裝或分析，已保存的結果仍可查看。CLI、專案與提示詞各自保存，以穩定 ID 關聯。每通錄音可指定專案，一鍵使用該專案的預設 CLI 與提示詞，結合多份本地轉寫整理需求。x86 不建立入口或分析程序。SenseVoice／Qwen3-ASR 產生的是轉寫，摘要由 CLI 完成。

1. 開啟「通話 AI 分析 → CLI 設定」。左側工具清單顯示 Claude Code、ChatGPT／Codex CLI、Cursor Agent、Antigravity、CodeBuddy、Qwen Code、Kimi Code、Qoder、TRAE CLI；實心圓點與「已安裝」表示本機已找到執行檔，空心圓點表示未安裝。選取已安裝的工具，或一鍵安裝完成後，會自動讀取模型清單，並只放進「預設模型」下拉框。也可直接在該框輸入一個清單以外的模型 ID。「更新模型」可再向工具讀取一次。也可按「官方說明」開啟該工具文件。也可另存自訂 stdin/stdout EXE。逾時為 10–3600 秒。推理強度可沿用工具設定或只覆寫本次程序，不修改工具個人設定。測試連線只使用合成文字，也可能消耗提供者額度。
2. 在「提示詞」選擇一般摘要、專案需求分析、問題排查、開發任務整理。編輯內建模板後保存為自訂副本，不修改內建原稿。
3. 在「專案設定」新增名稱、資料夾、預設 CLI、預設提示詞、專案術語／補充指示及參考檔案相對路徑（每行一個）。仍被專案使用的 CLI／提示詞不能直接刪除。
4. 在錄音列表選取已完成電話，右鍵「AI 分析」。選專案後帶入預設值；單次更換 CLI／模板／來源／輸出語言不改專案預設。
5. 「保存關聯」保存通話對應的專案。「預覽輸入」在背景產生完整輸入，不呼叫 CLI。「AI 分析」保存關聯並啟動工具。完成後，最終 Markdown 直接顯示在頁面；結果選單可切換同一通錄音裡不同 CLI／模型的成功版本。

以 iLab 為例：選取本地 checkout，預設「專案需求分析」，參考清單列出本次涉及的業務文件、畫面、API 或服務檔案。輸出要求區分用戶明確需求、附檔可證明的專案現況、AI 建議與待確認事項，並附原話來源／時間及專案路徑。

第一版不包含直接音訊上傳、說話人分離、即時自動摘要、自動修改專案、結果編輯器或聊天追問。

## 專案上下文與多來源

- 只讀取明確指定的參考檔，不自動檢索整個倉庫，不能宣稱全面檢查專案。清單留空時嘗試讀取 AGENTS.md、README.md；這兩個預設檔不存在時略過。
- 只接受專案內普通 UTF-8 md／txt／cs／vue／ts／js／hpp／cpp／xaml／sql／py。拒絕越界、連結、.git、node_modules 及明確的憑證／個人設定名稱。檔名篩選不能保證內容無敏感資訊，可先預覽核對。
- 最多24個參考檔、單檔48 KiB、合計384 KiB；單份轉寫 JSON 上限2 MiB，完整輸入上限4 MiB。超限明確失敗，不截斷。
- SenseVoice、Qwen 0.6B、Qwen 1.7B 分別勾選。不存在的來源標示缺少；至少須有一份可用轉寫。存在但未完成的所選來源阻止分析，須等待或取消勾選。不隱含安裝／執行 ASR。
- 多份 JSON（含分段時間）分開送入，不覆蓋原始結果。提示要求列出差異，禁止推測說話人、承諾或將模型一致視為正確證明。來源與參考文件附 SHA-256，保存完整輸入快照。
- 專案資料由主程式背景讀取，CLI 收到的是整理後內容，在獨立分析目錄執行；不是以原始碼資料夾作為工作目錄。目錄隔離不等同 OS 沙箱。

## CLI 契約

內建清單為 Claude Code、ChatGPT／Codex CLI、Cursor Agent、Antigravity、CodeBuddy、Qwen Code、Kimi Code、Qoder、TRAE CLI，另可保存自訂 stdin/stdout EXE。Claude Code 與 CodeBuddy 直接以 `-p --output-format text` 傳入 stdin，並停用內建工具、額外 MCP、會話保存及 hooks；不使用跳過權限或自動同意參數。Codex、Cursor、Antigravity、Qwen、Kimi、Qoder、TRAE CLI 由內嵌 worker 依各工具的非互動參數啟動。Qoder 使用 `-p` 與純文字輸出，需確認的操作維持拒絕。TRAE CLI 使用 `exec`、唯讀沙箱，並把最後一則回覆寫入工作檔。自訂 EXE 不附加參數，須從 UTF-8 stdin 讀取完整請求，向 UTF-8 stdout 輸出 Markdown，非零退出碼表示失敗。

「一鍵安裝」只允許各工具官方 HTTPS 安裝指令碼。選定的執行檔已存在時只檢查版本，不重複下載。安裝可能下載依賴並修改使用者 PATH。登入會開啟該 CLI 自己的互動終端，帳號仍留在各工具內。程式不保存密碼或 Token。

模型清單依工具而不同，介面會標示來源：Cursor、Antigravity、Qoder 讀取 CLI 帳號清單；Codex、CodeBuddy 讀取工具目錄，不代表帳號權限；Claude Code 使用官方別名；Qwen、Kimi、TRAE CLI 讀取本機設定。TRAE CLI 目前沒有獨立的模型清單命令，只顯示設定檔裡的模型名稱。清單只出現在「預設模型」下拉框；也可直接輸入清單以外的一個模型 ID，並存成該設定的預設模型。清單會緩存；重新開啟後標示為上次清單，需按「更新模型」才重新讀取。清單存在不證明該模型可用。

EXE 與模型名稱作獨立 Windows 引數處理，通話與提示詞不插入 shell 命令。Qwen 官方入口是受控的 `.cmd`，其餘官方入口為 EXE 或受控的 PowerShell 指令碼。每次執行一項分析或一項安裝／模型讀取，切頁不中斷；取消／退出回收受管理程序範圍。stdout 上限 2 MiB、stderr 上限 256 KiB。失敗／逾時不換工具、不自動重送、不覆蓋上次成功結果。分析與安裝互斥。

CLI 可能把文字送往提供者雲端；本地 ASR 與雲端分析分開。

依據：[Claude Code](https://code.claude.com/docs/en/setup)、[Codex CLI](https://learn.chatgpt.com/docs/codex/cli)、[Cursor CLI](https://cursor.com/docs/cli/installation)、[Antigravity CLI](https://antigravity.google/docs/cli/install)、[CodeBuddy](https://www.codebuddy.cn/docs/cli/installation)、[Qwen Code](https://qwenlm.github.io/qwen-code-docs/en/users/quickstart/)、[Kimi Code](https://www.kimi.com/code/docs/en/kimi-code-cli/guides/getting-started.html)、[Qoder CLI](https://docs.qoder.com/cli/installation)、[TRAE CLI](https://docs.trae.cn/cli_get-started-with-trae-cli-2)。ZCode 官方 Windows 版是桌面安裝程式，沒有與上述工具相同的官方命令列安裝腳本，因此未列入一鍵安裝。

## 本地資料與結果

選定 INI 旁的 `.ai-profiles` 為 JSON Lines，最多100項設定。通話關聯存於 `call.wav.ai-project.json`，原始 WAV 不移動。每次分析保存獨立版本：

```text
call.wav.ai-history/<unique-id>/prompt.txt
call.wav.ai-history/<unique-id>/request.json
call.wav.ai-history/<unique-id>/result.md
call.wav.ai-history/<unique-id>/stderr.txt
call.wav.ai-latest.json
```

request.json 保存設定快照、輸入 SHA-256 與終態；預覽也保存獨立 prompt.txt。合成連線測試存於 `.ai-profiles.tests`。這些資料可能含通話、專案內容及提供者診斷，均已加入 Git 忽略，不寫入一般產品診斷日誌，不自動匯出至專案。

「上次結果」查看最近成功版本，「歷史紀錄」開啟該通錄音的分析資料夾。結果選單列出各次成功分析的 CLI 名稱、模型與版本目錄；選取後，最終 Markdown 直接顯示在頁面，可複製或手動匯出 UTF-8 Markdown。工具清單與模型緩存位於 INI 旁的 `.ai-profiles.models`，安裝暫存位於 `.ai-profiles.tools`。已刪除的專案關聯提示重新選擇，不默認改綁其他專案。程式重開不自動重送分析。

## 國際化與多語言交付

僅 x64 Windows 原生端新增入口。簡中、繁中、English 的預設、使用者選擇來源及缺漏顯示沿用既有行為；文案集中於 `cpp/ai_resources.inc`，由 `ui_resources.hpp` 載入。內建模板有三語內容，自訂名稱／提示詞保留原文。輸出語言首次跟隨 UI，可另選；切換 UI 不改已選輸出語言或既有結果。模型 ID、雜湊、JSON key 與數值時間不翻譯。

安裝狀態、模型來源說明與結果選單沿用同一套三語資源。驗收涵蓋三語設定／分析頁、長文字捲動、中文路徑及 UTF-8 輸入輸出。系統檔案對話框與官方安裝程式介面沿用 Windows／該工具語言。

## 建置、驗證與限制

```powershell
pwsh -NoProfile -File tools/Build-Desktop.ps1 -Architecture x64 -OutputDirectory tmp/ai-x64
pwsh -NoProfile -File tools/Build-Desktop.ps1 -Architecture x86 -OutputDirectory tmp/ai-x86
pwsh -NoProfile -File tools/Test-AiAnalysis.ps1
py tests/ai_ui_smoke.py --exe tmp/ai-x64/PhoneRecord-x64.exe --fixture <test-build>/ai_cli_fixture.exe --output tmp/ai-ui-evidence
```

原生測試用合成錄音身份、雙轉寫、參考文件與假 CLI，覆蓋設定復原、引用刪除保護、通話關聯、來源未完成、越界、預覽、stdio、非零退出、輸出限制、取消、逾時、更新互斥及原始資料不變。Worker 測試覆蓋九種工具的偵測、模型來源與模擬官方安裝。原生 UI 測試覆蓋工具清單、已存在執行檔的安裝檢查、模型更新與預設模型保存、兩份 CLI 結果切換、專案預設帶入、雙來源預覽及三語切換。UI 測試會先切到簡體中文，避免系統預設繁體時把「已安裝」誤判為失敗。

假 CLI 通過不等於真實提供者成功。本機 CodeBuddy 合成連線實測回傳 `400 the reasoning effort value is not supported by the current model` 至 stderr，但程序退出碼為0；程序覆寫為 medium 後仍遭相同錯誤。已加入 stdout／stderr 的已知提供者錯誤辨識與原生回歸，不把它保存成成功摘要，未修改工具個人設定。需選擇相容的 reasoning effort／模型後再測；Claude Code 尚未在本機安裝，真實聯測待驗證。

真實模型品質、額度／地區可用性、各 CLI 版本、自訂包裝器，以及從官方網址實際下載安裝，須在本機已登入的工具上另行驗證。合成測試不連官方安裝網址。實體話機與雙向聲音沿用獨立驗收範圍。這次不發布、不替換運行中的程式，不修改 iLab 專案與使用者現有個人設定。
