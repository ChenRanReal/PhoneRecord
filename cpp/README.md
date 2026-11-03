# PhoneRecord 2.4.0 — C++ 電話錄音工具

原生 Windows 桌面程式，直接建置為 `PhoneRecord.exe`，不需要 Python 或額外的 C++ Runtime 安裝。
2.3.1 加入[原創電話錄音圖示](assets/README.md)，包含九種尺寸並內嵌於執行檔；視窗、工作列與系統托盤共用。
已驗證的 Fanvil X3S 錄音流程保留。2.3 加入首次啟動引導、可關閉的托盤氣泡，以及不列實際 IP 的唯讀適配檢查。首次使用請讀 [快速上手](../docs/快速上手.md)，English guide: [Quick start](../docs/QuickStart.en.md)。

## 啟動

雙擊專案根目錄的 [Start-PhoneRecord.cmd](../Start-PhoneRecord.cmd)，或直接執行本目錄的 `PhoneRecord.exe`。
預設讀取與 exe 同目錄的 `config.ini`，所有相對路徑都以該配置所在目錄解析，從捷徑或其他工作目錄啟動也一致。

exe 可單獨首次啟動，沒有配置或必要欄位未填時直接開啟設定頁，位址及網頁帳號不預填。可選用 [空白配置範本](config.example.ini)。保存連接設定後需按左下「退出程式」再重開；右上 X 通常只收起到托盤。
預設事件埠為 `18080`，可改成其他未占用埠；一般介面保存不自動改寫話機。
`hook_port=0` 是外部觸發模式，介面會明確標示。自動觸發所用事件埠必須與話機 Action URL 一致。

## 自動設定話機

連線檢查成功僅表示能登入電話。設定頁可「產生話機設定腳本」，保存重啟後產生、無通話時退出程式再執行。腳本先預覽，輸入 APPLY 才套用，保留可讀取的其他欄位，先加密備份再回讀驗證；失敗時嘗試還原。只需 exe 與 Windows PowerShell 5+，不需 Python。完整流程見 [話機自動設定](../docs/話機自動設定.md)。

## 操作

| 頁面 | 能做什麼 |
|---|---|
| 运行概览 | 觀察觸發、HTTP 指令、TCP、控制回應、RTP、WAV、品質；自動錄音、獨立健康燈、本機／話機檢查 |
| 录音资料 | 搜尋姓名／號碼／日期，內建播放／暫停／拖曳進度、定位檔案、檢查既有 WAV，分別顯示訊號品質與來源 |
| 诊断中心 | 毫秒時間線、搜尋、警告／錯誤篩選、完整事件與建議、複製／匯出已遮蔽報告 |
| 说明与适配 | 離線首次設定對照、逐欄說明、通用回呼模板、唯讀適配檢查與適配報告，隨語言即時更新 |
| 连接设置 | 編輯連接與目錄、關閉行為、氣泡通知及檢查頻率；連接變更重啟套用 |

內建播放器使用 Windows waveOut，不啟動系統預設播放器。支援暫停／繼續、停止及拖曳進度。
關閉視窗可設為系統匣、工作列最小化或直接退出；預設保留系統匣。側欄或系統匣「退出程序」始終直接退出。
托盤氣泡預設關閉，設定頁勾選或取消後保存即生效。
預設每 60 秒在空閒時檢查；通話中暫停話機 HTTP 檢查，掛斷後保留至少 20 秒安靜間隔。
通話中退出會提示；正常退出會收尾本機 WAV 並清理工作執行緒。

首次語言依 Windows 選擇；側欄可即時切換簡體、繁體及 English；語言獨立保存至 `<config>.ui`，不提交連接草稿。
逐欄用途、事件埠遷移及機型要求見 [操作與機型適配](../docs/操作與機型適配.md)。

適配檢查只用 GET 讀錄音與回呼設定，必要時先完成網頁登入；不發送 Start／Stop 或改寫設定。它辨識目前支援的介面欄位，不推測具體型號。通過後仍須真實通話驗收及雙向試聽。說明與適配報告不列實際 IP；診斷日誌仍保留排錯所需環境資料。

詳細錯誤代碼及實機調試步驟見 [使用與排查指南](../docs/PhoneRecord_使用與排查.md)。

## 錄音與日誌

- PCM WAV：8kHz、16-bit、單聲道；支援 PCMA／PCMU，實際 RTP payload type 決定解碼。
- `*.wav.quality.json`：訊號分類、來源、RMS、peak、有效包、丟包、重複、亂序及收尾原因。
- `calls.csv`：保留九欄既有台帳，業務欄位正確引用，避免姓名中的逗號破壞欄位。
- `PhoneRecord.log`：毫秒時間、執行緒與穩定代碼；以 `cid` 與非原始 ID 的會話指紋關聯。
- 記憶體最多保留 2,000 條事件，介面顯示最近符合條件的 500 條；單份磁碟日誌約 2 MiB，保留三份輪替檔。
- 報告只含本次運行的遮蔽事件與狀態，不匯出登入密碼、cookie、nonce 或通話姓名／號碼。

「訊號與變化通過」只代表幅度與完整秒 RMS 有變化；還須確認新檔、話機來源及實際內容，才能計作真實錄音成功。

## 配置相容性

沿用扁平 `key=value` 格式。保留 `phone_pass` 明文舊格式讀取；新版介面保存時改用 `phone_pass_dpapi`，並將已驗證 cookie 加密保存。
加密以目前 Windows 帳戶為範圍；換使用者或電腦時應在介面重新輸入密碼。[Windows DPAPI 文件](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)。

保存完整包含 `phone_port`，不會把運行中的 socket 配置一半改成新值。舊 `ui_port` 保留讀寫相容性；新版使用原生介面，不需要 8090 網頁管理埠。
話機的公司 IT Action URL 不由本工具改寫。背景不週期性拉取 calllog，以免擠占錄音指令配額。

## 建置

```powershell
cmd /c .\cpp\build.bat
```

唯一入口為 `build.bat`：MSVC 14.51.36231、Windows SDK 10.0.26100.0、C++17、x86、靜態 CRT。
包含 exe 版本資源、DPI／Common Controls manifest、GS、ASLR 與 DEP 設定。
API 編譯目標保留 Windows 7，但目前只驗證本機 Windows 環境；未宣稱舊系統已驗收。

| 元件 | 職責 |
|---|---|
| `phonerecord.cpp` | 既有 G.711、RTP、配置、通訊錄與錄音清單，整合單一編譯單元 |
| `diagnostics.hpp` | 有界日誌、診斷快照、完整發送與 PCM 品質分析 |
| `recording_core.hpp` | TCP 切幀、UDP 接收、會話／執行緒生命週期 |
| `phone_client.hpp` | Fanvil challenge、快取驗證、1.1 秒安靜間隔、單次 Start 與唯讀設定檢查 |
| `compatibility.hpp` | 有界 HTML 欄位解析、地址不外露的檢查結果、檢查後新通話的證據邊界 |
| `event_server.hpp` | 有界 HTTP 事件服務，完整回應後才排程動作 |
| `application.hpp` | 指令序列、本機檢查、WAV 分析、配置保存與診斷報告 |
| `audio_player.hpp` | 內建非同步 WAV 播放，獨立擁有 waveOut 裝置與檔案 |
| `json_text.hpp` | JSON 字串跳脫及 Unicode／surrogate pair 解析 |
| `desktop_view.hpp`／`ui_text.hpp`／`ui_resources.hpp` | 原生五頁介面、三語即時切換及完整內建說明 |
| `numeric_edit.hpp` | 鍵盤、貼上及替換的正整數輸入政策，保存時再次驗證 |
| `desktop_ui.hpp` | 配置路徑、單例、服務啟停與清理 |

## 開發驗證

從專案根目錄設定 UTF-8 環境，再用已安裝的 Python 執行測試；不會聯絡真實話機。

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
py -3 tests\product_smoke.py --output docs\probe\onboarding_20261103
py -3 tests\desktop_smoke.py --output docs\probe\onboarding_20261103
```

新增操作回歸：`tests\desktop_workflow.py --output docs\probe\onboarding_20261103\workflow`，包括實際音訊輸出、CSV／Unicode、健康恢復、捲動、關閉與 30 秒排程。

三語／輸入／說明：`tests\localization_smoke.py --output docs\probe\onboarding_20261103\localization`。
事件埠遷移保護：`tests\event_port_migration.py`，使用合成表單驗證只改兩欄及回讀失敗的有界回復。

介面測試使用 Pillow 產生離線 UI 預覽，產品程式本身不依賴 Pillow 或 Python。
首次使用／適配／通知：`tests\onboarding_smoke.py --output docs\probe\onboarding_20261103\onboarding`。
話機自動設定：`tests\phone_setup_smoke.py --output docs\probe\setup_20261103\phone_setup`。
測試範圍、重現方式與限制見 [開發與驗證](../docs/開發與驗證.md)；本機結果存放於 Git 忽略的 `docs/probe/`。

## 國際化與多語言交付

產品支援簡體中文、繁體中文與 English，文案集中於 `ui_resources.hpp`；`ui_text.hpp` 負責語言選擇與診斷建議。
每個資源鍵必須提供三語文字，缺列不能編譯，缺漏鍵明確標記；語言偏好即時獨立保存。
技術代碼、協議及資料格式保持穩定；Windows 系統對話框按作業系統語言顯示。文件使用繁體中文，涵蓋 UTF-8 無 BOM、CJK 與三語原生版面驗證。
