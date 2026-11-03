# PhoneRecord 圖示

本專案原創圖示：沿用介面的藍色，搭配白色聽筒及紅色錄音圓點；圖示不含文字，方便三語介面共用。

- `phonerecord.svg`：可編輯的向量原稿。
- `phonerecord.png`：256 × 256 透明預覽。
- `phonerecord.ico`：16、20、24、32、40、48、64、128、256 像素的透明 Windows 圖示。

執行檔、視窗／工作列與系統托盤使用同一個內嵌資源，不需要隨程式附帶這些圖片。
重新生成可在專案根目錄執行 `python tools/build_app_icon.py`，開發環境需要 Pillow；產品執行時不需要 Python。

曾查看 [Lucide 電話圖示](https://lucide.dev/icons/phone) 作為類型參考，最終檔案採本專案自行繪製的幾何圖形，未使用第三方圖庫資產。
