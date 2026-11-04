# PhoneRecord 2.6 quick start

PhoneRecord automatically records connected calls from a compatible network-recording phone to a Windows PC. Recordings play inside the app. No Python installation is needed.

The verified combination is Fanvil X3S, hardware V1.1, firmware 2.4.7.9.1. Other devices require inspection and a real call test. SIP calling support alone does not establish compatibility.

1. Put `PhoneRecord.exe` in a writable folder and run it. An exe-only first launch opens Settings. Choose English at the lower left. Offline help is built in: **Help & devices → First setup: PC and phone**.
2. Enter your own phone IPv4, HTTP web port (usually 80), authorized web username/password, and a local PC IPv4 reachable from the phone. Find the phone address on its network-status page and the PC address in Windows connection details or `ipconfig`. The web login is separate from the SIP extension account.
3. Start with TCP control port **10000**, event port **18080**, and UDP **20000–20100**, or coordinate available ports with your administrator. Leave External trigger off for normal automatic recording. Select a writable recording folder.
4. Save, use **Exit** at the lower left, then reopen the app. **X normally hides to the tray and does not restart the app.**
5. For automatic phone configuration, use **Settings → Phone setup script**. Save/restart first, generate the CMD, then exit while idle and run it. Review and type `APPLY`. It configures the fields below, creates an encrypted backup, verifies readback and reopens the app. Requires Windows PowerShell 5+, with no Python installation. Unknown interfaces or unrelated callbacks stop the operation. Alternatively, on the phone web page, enable recording, select **Network** and **PCMA or PCMU**, set the recording server to the app's reachable local IPv4, and set the server port to its **TCP control port**. Allow the phone to reach the PC's control/event TCP and audio UDP ports.
6. Set the phone's Call Established and Call Terminated recording Action URLs below. Replace the placeholders with your own PC IPv4 and **event port**. Preserve unrelated company callbacks. If either event already serves another company workflow, have an administrator coordinate forwarding before replacing it.

```text
http://<PC_IP>:<EVENT_PORT>/event?ev=est
http://<PC_IP>:<EVENT_PORT>/event?ev=end
```

The event port and recording server port have different roles. Event 18080 does not replace control 10000.

7. Run **Check local**, **Check phone**, and **Check compatibility** while idle. Inspection reads the recording/callback pages; it does not change the phone, dial or toggle recording. Follow the individual findings. Matching settings means you are ready for call acceptance, rather than proof that audio works.
8. Dialing/ringing alone does not trigger recording. Connect a real call for about **20 seconds**, let both people speak, hang up, then play the newest recording. Confirm its time and both speakers. A green indicator or successful signal check alone is insufficient.

An unknown interface means an adapter may be required; it does not establish that the phone hardware cannot record. An unreadable page requires checking the web port, permissions, session and connection. Inspection waits during a call. Unsaved/unapplied connection changes require saving and restarting first.

For daily use, keep the PC awake and the app running, then answer/dial normally. Tray balloons are **off by default** and can be enabled in Settings. Closing behavior is configurable: tray, taskbar minimize or exit. Use the tray icon to restore the window. The lower-left/tray Exit command actually stops the app.

To name a historical call, select a recording and choose **Rename** or press **F2**. Save a single-line title of up to 120 Unicode characters. It is searchable and persists after reopening. Save blank or **Restore default** to use the original title again. Cancel discards the draft. Original contacts, numbers, times, audio filenames and the ledger are retained. Copy the recording's sidecar files along with the audio when backing up or moving a record.

**Startup at sign-in is off by default.** In Settings, check **Start at sign-in (background)** to register it for your current Windows user. Checking or clearing it applies immediately without Save. It opens in the tray at your next sign-in; double-click the tray icon to open. The preference stays with Windows, rather than a copied configuration. Put the exe in a permanent folder first. After moving it, turn this option off and on to update its path. If you previously created a startup BAT, shortcut or scheduled task, remove that older entry separately. If disabled through Windows Startup apps, re-enable it in Windows. Incomplete settings or an unavailable tray show the main window.

For problems, follow the recording chain and Diagnostics guidance. A missing trigger usually points to callbacks/event connectivity; missing audio after control points to the recording destination, codec or UDP connectivity. Use Copy compatibility report for an address-free checklist. General diagnostic reports can still contain technical environment details.

To hand the app to another person, provide the exe and this guide, optionally the blank [config.example.ini](../cpp/config.example.ini). Have them enter their own settings. Do not reuse another person's configuration/session cache or include their recordings and logs. Saved credentials are tied to the current Windows user and must be re-entered on another account/PC.
