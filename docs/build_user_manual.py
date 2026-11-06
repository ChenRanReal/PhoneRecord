"""Build the Traditional Chinese operating manual from verified local contracts."""
from pathlib import Path
from html import escape
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_LEFT
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak, Image, KeepTogether
from reportlab.lib.pagesizes import A4

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'output' / 'pdf'
OUT.mkdir(parents=True, exist_ok=True)
PDF = OUT / 'PhoneRecord_操作與設定手冊.pdf'
pdfmetrics.registerFont(TTFont('JhengHei', r'C:\Windows\Fonts\msjh.ttc', subfontIndex=0))
pdfmetrics.registerFont(TTFont('JhengHeiBold', r'C:\Windows\Fonts\msjhbd.ttc', subfontIndex=0))
pdfmetrics.registerFontFamily('JhengHei', normal='JhengHei', bold='JhengHeiBold', italic='JhengHei', boldItalic='JhengHeiBold')
NAVY = colors.HexColor('#16263e')
BLUE = colors.HexColor('#245bd8')
GRAY = colors.HexColor('#536174')
styles = {
    'body': ParagraphStyle('body', fontName='JhengHei', fontSize=10, leading=16, textColor=NAVY, spaceAfter=8, wordWrap='CJK'),
    'small': ParagraphStyle('small', fontName='JhengHei', fontSize=8, leading=12, textColor=GRAY, spaceAfter=6, wordWrap='CJK'),
    'h1': ParagraphStyle('h1', fontName='JhengHeiBold', fontSize=23, leading=32, textColor=NAVY, spaceAfter=16, wordWrap='CJK'),
    'h2': ParagraphStyle('h2', fontName='JhengHeiBold', fontSize=13, leading=20, textColor=BLUE, spaceBefore=10, spaceAfter=7, wordWrap='CJK'),
    'cell': ParagraphStyle('cell', fontName='JhengHei', fontSize=9, leading=14, textColor=NAVY, wordWrap='CJK'),
    'code': ParagraphStyle('code', fontName='JhengHei', fontSize=9, leading=15, textColor=BLUE, spaceAfter=10, wordWrap='CJK'),
}
story = []
def p(text, kind='body'):
    return Paragraph(escape(text).replace('\n', '<br/>'), styles[kind])
def add(text, kind='body'):
    story.append(p(text, kind))
def heading(number, title):
    if story: story.append(PageBreak())
    add(f'{number:02d}  /  操作與設定', 'small')
    add(title, 'h1')
def sub(title): add(title, 'h2')
def steps(items):
    for n, item in enumerate(items, 1): add(f'{n}.  {item}')
def table(headers, rows, widths=None):
    data = [[p(s, 'cell') for s in headers]] + [[p(str(s), 'cell') for s in row] for row in rows]
    t = Table(data, colWidths=widths or [145, 360], repeatRows=1, hAlign='LEFT')
    t.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#e3ecfc')),
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('LEFTPADDING', (0, 0), (-1, -1), 9), ('RIGHTPADDING', (0, 0), (-1, -1), 9),
        ('TOPPADDING', (0, 0), (-1, -1), 6), ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
        ('LINEBELOW', (0, 0), (-1, -1), .4, colors.HexColor('#d6dfec')),
        ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#f5f7fb')]),
    ]))
    story.append(t)
    story.append(Spacer(1, 9))
def note(text):
    t = Table([[p(text)]], colWidths=[505])
    t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,-1),colors.HexColor('#edf3ff')),('BOX',(0,0),(-1,-1),.6,colors.HexColor('#c8d9fa')),('LEFTPADDING',(0,0),(-1,-1),12),('RIGHTPADDING',(0,0),(-1,-1),12),('TOPPADDING',(0,0),(-1,-1),10)]))
    story.append(t)


add('PHONERECORD 2.7.4  /  操作手冊  /  2026-11-06', 'small')
add('軟體設定', 'h1')
add('左側「連接設定」分成基本、進階及偏好三區。首次使用先填基本資料，再按「一鍵設定話機」。')
sub('基本設定')
table(['欄位', '填寫方式'], [
('話機 IPv4', '電話目前的 IP。需要 DHCP 自動追蹤時，先到進階設定切換尋址方式。'),
('話機網頁帳號／密碼', '使用可讀取、修改錄音設定並啟停錄音的網頁帳密，不要求管理員身分；與 SIP 分機帳密不同。'),
('錄音保存目錄', '選可寫入且空間足夠的位置；更改目錄不會搬移舊檔。'),
('本機接收位址', '基本頁只顯示結果，預設自動帶入；需要手動修改時到進階設定。'),
])
steps(['填妥基本資料後按「保存設定」，無通話時按左下「退出程式」並重開。', '再按「一鍵設定話機」，核對預覽並確認。設定完成後按「更多操作 → 檢查連線與設定」，再以一通真實電話驗收。'])
sub('進階設定（一般保留預設）')
add('包含話機網頁埠、本機 IPv4、控制 TCP、事件 TCP、音訊 UDP 範圍、空閒逾時、健康檢查頻率及外部觸發。固定 IP 模式不顯示 MAC；DHCP 模式才要求填寫。切換分類或模式不會清空草稿。')
add('預設：話機網頁 TCP 80；話機到電腦的控制 TCP 10000、事件 TCP 18080、音訊 UDP 20000-20100。如有調整，軟體、話機與防火牆規則需一致。', 'small')
sub('偏好設定')
add('通知、關閉視窗方式與開機啟動在此調整。通知及關閉方式保存後立即生效；開機啟動勾選即生效。其他分類的設定值也會保留。')
add('錄音分資料夾：在「偏好設定 → 錄音分資料夾」選不分、每日、每週（以週一日期命名）或自訂。自訂例：{yyyy}/{MM}/{dd}，或「週錄音/{weekStart}」。畫面會預覽今日位置；保存後退出並重開。只影響新錄音，舊檔不移動；跨午夜依開始錄音日期歸檔。', 'small')
note('右上 X 預設只是收至托盤，並未退出。電腦須保持喚醒、程式持續運行，才能錄音。')

heading(2, '電話網頁設定（手動方式）')
add('登入話機管理網頁。以下以支援的 Fanvil 介面為例；選單名稱依韌體可能不同。使用軟體內一鍵設定者可直接看第 3 頁。')
sub('1. 應用／Applications → 錄音管理／Record')
table(['網頁欄位', '設定值'], [
('啟用錄音', '啟用／勾選。'),
('錄音類型／Record Type', 'Network。'),
('錄音編碼／Codec', 'PCMA 或 PCMU；不要選 G.722 或 SIP INFO。'),
('錄音伺服器位址', '軟體的「本機可達 IPv4」（電腦 IP）。'),
('錄音伺服器埠', '軟體的「TCP 控制埠」，預設 10000。'),
])
add('按套用／保存，再重新載入，確認設定已保留。')
sub('2. Action URL／動作 URL')
add('找到以下兩欄，填入對應網址。將 <PC_IP> 與 <EVENT_PORT> 整段替換成電腦 IP 與事件接收埠（預設 18080），不要保留尖括號。')
table(['事件', '網址'], [
('Call Established\n通話建立', 'http://<PC_IP>:<EVENT_PORT>/event?ev=est'),
('Call Terminated\n通話結束', 'http://<PC_IP>:<EVENT_PORT>/event?ev=end'),
], [150,355])
add('按套用／保存，再重新載入確認。使用「通話建立」事件，不是撥號或振鈴事件。')
note('兩欄若已有公司其他用途，不要覆蓋，請 IT 安排轉送。若已有 PhoneRecord 回呼及額外巨集參數，保留原參數。')
sub('3. 回到軟體驗收')
steps(['按「檢查連線與設定」，依報告中的提示修正。', '接通一通約 20 秒的電話，雙方輪流說話，再掛斷。', '在「錄音資料」播放最新一筆，確認雙方聲音、開頭及結尾完整。'])
add('「設定匹配」、綠燈或有 WAV 都不能取代雙向試聽；適配檢查本身不會測試 Start／Stop 權限。', 'small')

heading(3, '首次使用：一鍵設定話機')
add('入口：左側「連接設定」→ 底部「一鍵設定話機」。使用此功能，不必產生或執行 CMD。')
steps(['在「連接設定」填妥資料，按「保存設定」，再從左下「退出程式」並重開。', '重新進入「連接設定」，按底部「一鍵設定話機」。請先結束所有通話與錄音。', '軟體讀取目前話機設定，列出目標話機與待修改項目。核對正確後，在確認視窗按「是」才會套用；按「否」取消。', '等待加密備份、寫入及回讀確認。完成後軟體保持運行，並重新執行適配檢查，不必退出再開。', '接通約 20 秒，雙方輪流說話，掛斷後到「錄音資料」播放最新一筆，確認雙方聲音與開頭完整。'])
sub('如果只有兩條回呼顯示「需修正」')
add('「通話建立回呼／通話結束回呼」需修正，但 Network、編碼、錄音伺服器位址與控制埠皆通過時，仍直接按「一鍵設定話機」。預覽會列出需要修正的回呼，確認後補上或更新為這台電腦的目的地。')
note('若兩條目標事件已有公司其他用途，會停止並提示衝突，不會強制覆蓋；請 IT 協調。')
sub('權限與失敗處理')
table(['狀況', '處理'], [
('提示先保存／重開', '保存連接設定，從左下退出重開後再按一鍵設定。'),
('登入失敗／拒絕寫入', '核對話機 Web 帳密及錄音設定、Action URL 寫入權限。'),
('介面不支援／回呼衝突', '依第 2 頁手動設定，或請 IT 處理既有用途。'),
('套用失敗但已恢復', '原設定已回讀確認恢復；依提示處理失敗原因。'),
('無法確認還原', '保留日誌與 phone-setup-backups/ 加密備份，請 IT 核對。'),
])
add('備份綁定目前 Windows 使用者與目標話機。功能不修改 Windows 防火牆、不撥號，也不以設定匹配代替實際通話驗收；目標設備仍須實測。', 'small')

heading(4, '日常操作與故障排查')
table(['操作', '用法'], [
('錄音', '正常撥接即可，對方接聽後自動開始，掛斷後保存。'),
('搜尋／播放', '到「錄音資料」依名稱、聯絡人或號碼搜尋，雙擊或按播放；可暫停及拖曳進度。'),
('重新命名', '選取後按右鍵 →「重新命名」，或按 F2；留空保存可還原。原號碼、時間與錄音檔保留。'),
('背景／開機啟動', 'X 預設收至托盤。「開機啟動（背景）」勾選立即生效，於目前使用者登入後執行，不是 Windows 服務。'),
('備份', '保留整個錄音資料夾與附屬檔、台帳；不要只備份 WAV。'),
])
sub('常見問題')
table(['現象', '優先檢查'], [
('本機燈紅', 'IP、埠占用、重複實例、目錄寫入權限與磁碟空間。'),
('話機登入失敗', '管理網頁、HTTP 埠、Web 帳密及錄音頁權限。'),
('接通後沒有起錄', '兩條 Action URL、事件埠與話機到電腦的 TCP 放行。'),
('有事件，沒有控制', 'Network、電腦目的地／控制埠、Start／Stop 權限。'),
('有控制，沒有音訊', 'PCMA／PCMU、UDP 放行、本機 IP 與路由。'),
('無聲／單向／開頭斷續', '播放最新檔，核對話機雙向混音與真實通話聽感；記下時間並匯出診斷。'),
('保存後沒變', '按「退出程式」再重開，不是只按 X。'),
])
add('概覽只顯示近期異常；雙擊可進入診斷中心。完整事件與訊號分析保留供排錯，錄音次要操作在右鍵選單。設定指南與話機網頁在「更多操作」；診斷匯出也在該頁的「更多操作」。更新頻率與提醒在更新頁右上「更新偏好」。需要協助時，提供診斷代碼、型號／韌體、發生時間與操作步驟。分享報告或截圖前遮蔽 IP、姓名、號碼、帳號及公司 URL；不要提供密碼或 cookie。')
note('相容性：專案已驗證 Fanvil X3S／硬體 V1.1／韌體 2.4.7.9.1；其他組合需逐台驗證。')
add('介面支援簡體中文／繁體中文／English，左下可切換；本手冊使用繁體中文，保留網頁欄位與診斷代碼原文。本手冊不含個人帳號或連線資料。', 'small')

def footer(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(colors.HexColor('#d6dfec'))
    canvas.line(45, 42, A4[0]-45, 42)
    canvas.setFont('JhengHei', 8)
    canvas.setFillColor(GRAY)
    canvas.drawString(45, 28, 'PhoneRecord  |  操作與設定手冊  |  2026-11-06')
    canvas.drawRightString(A4[0]-45, 28, str(doc.page))
    canvas.restoreState()

doc = SimpleDocTemplate(str(PDF), pagesize=A4, rightMargin=45, leftMargin=45,
                        topMargin=43, bottomMargin=57, title='PhoneRecord 操作與設定手冊',
                        author='PhoneRecord', subject='軟體、電話網頁、一鍵設定與權限要求')
doc.build(story, onFirstPage=footer, onLaterPages=footer)
print(str(PDF).encode('unicode_escape').decode())
