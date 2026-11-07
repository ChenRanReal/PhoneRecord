"""Exercise native contact synchronization using synthetic loopback HTTP, RTP and owned windows."""

from __future__ import annotations

import argparse
import json
import pathlib
import tempfile
import time

from desktop_smoke import USER, title
from desktop_workflow import main_window
from localization_smoke import resources, select_language
from product_smoke import App, FakePhone, emit_rtp, start_frame, wait_for


class DirectoryPhone(FakePhone):
    """Extend the existing authenticated fixture with the two read-only XML endpoints."""

    def __init__(self):
        """Install synthetic XML responses without changing the established login protocol."""
        super().__init__("synthetic-directory")
        self.contacts = '<contacts><contact><name>測試 &amp; 聯絡人😀</name><tel1>900-01</tel1><tel2>90002</tel2></contact></contacts>'
        self.history = '<calllog><record><name>歷史姓名</name><num>90001</num></record><record><name>歷史補充</name><num>90003</num></record></calllog>'
        self.delay = 0
        self.reads = []
        handler = self.server.RequestHandlerClass
        original = handler.do_GET
        owner = self

        def read(request):
            """Serve only local XML data and never print the payload or credentials."""
            path = request.path.split("?")[0]
            if path not in ("/contacts.xml", "/calllog.xml"):
                return original(request)
            owner.reads.append(path)
            if owner.delay:
                time.sleep(owner.delay)
            try:
                request.answer(owner.contacts if path == "/contacts.xml" else owner.history)
            except (ConnectionError, OSError):
                pass  # The native idle reader deliberately cancels when a call starts.

        handler.do_GET = read


def record(app, session, number, name=""):
    """Resolve a synthetic caller through the real control receiver and finalized metadata."""
    frame = start_frame(session).replace(b"remote_number=offline", f"remote_number={number}".encode())
    frame = frame.replace(b"remote_name=", f"remote_name={name}".encode())
    reply = app.request(frame)
    assert reply["result"] == "success", reply
    emit_rtp(int(reply["port"]), [0x80, 0xA0] * 20)
    app.stop_session(session)
    _, data = app.metadata(session)
    return data["display_name"]


def synchronize(app, window, code="DIRECTORY_SYNC_OK"):
    """Click the actual native sync command and await a fresh completion event."""
    wait_for(lambda: USER.IsWindowEnabled(USER.GetDlgItem(window, 230)), timeout=15)
    count = app.log().count(code)
    USER.PostMessageW(window, 0x0111, 230, 0)
    wait_for(lambda: app.log().count(code) > count, timeout=20)


def main():
    """Check lookup priority, XML decoding, cache retention, deletion, cancellation and locales."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    resources()
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-directory-") as temporary, DirectoryPhone() as phone:
        root = pathlib.Path(temporary)
        app = App(root, args.exe.resolve(), output, "directory", gui=True, offline=False,
                  event_port=0, phone_port=phone.port, password=phone.password)
        (app.root / "contacts.txt").write_text("90002=人工優先\n", encoding="utf-8")
        (app.root / "offline.json").write_text('{"90004":"既有目錄"}', encoding="utf-8")
        with app:
            window = main_window(app)
            wait_for(lambda: "DIRECTORY_SYNC_OK" in app.log(), timeout=20)
            assert record(app, "local", "90001") == "測試 & 聯絡人😀"
            assert record(app, "punctuation", "900-01") == "測試 & 聯絡人😀"
            assert record(app, "manual", "90002") == "人工優先"
            assert record(app, "history", "90003") == "歷史補充"
            assert record(app, "legacy", "90004") == "既有目錄"
            assert record(app, "wire", "90001", "通話提供") == "通話提供"
            assert record(app, "wire-number", "90001", "90001") == "測試 & 聯絡人😀"
            assert record(app, "unknown", "90009") == ""
            checks.append("real control/RTP path: XML entities, Unicode, alternate numbers, punctuation and name-source priority")
            cache = app.root / ".phone_directory.dpapi"
            wait_for(cache.exists)
            before = cache.read_bytes()
            assert "測試".encode() not in before and b"90001" not in before
            for bad in ("<html><body>login</body></html>", "<contacts><contact>",
                        '<!DOCTYPE contacts [<!ENTITY x SYSTEM "file:///never-read">]><contacts/>', '<error/>'):
                phone.contacts = bad
                synchronize(app, window, "DIRECTORY_SYNC_FAILED")
                assert cache.read_bytes() == before
            assert record(app, "retained", "90001") == "測試 & 聯絡人😀"
            checks.append("HTML, malformed XML, DTD and unsupported schema preserve encrypted cache and names")
            for language, caption in ((0, "同步话机通讯录"), (1, "同步話機通訊錄"), (2, "Sync phone contacts")):
                select_language(window, language)
                assert title(USER.GetDlgItem(window, 230)) == caption
            checks.append("three-language sync button and complete resource keys")
        # Reload the same account/device cache even when the next read fails.
        with app:
            wait_for(lambda: app.log().count("APP_START") >= 2)
            window = main_window(app)
            wait_for(lambda: app.log().count("DIRECTORY_SYNC_FAILED") >= 5, timeout=20)
            assert record(app, "restart", "90001") == "測試 & 聯絡人😀"
            checks.append("encrypted cache survives restart and failed synchronization")
            phone.contacts = "<contacts/>"
            phone.history = "<calllog/>"
            synchronize(app, window)
            assert record(app, "deleted", "90001") == ""
            assert record(app, "manual-preserved", "90002") == "人工優先"
            checks.append("successful empty snapshot removes deleted phone contacts and preserves manual sources")
            phone.delay = 4
            reads = len(phone.reads)
            wait_for(lambda: USER.IsWindowEnabled(USER.GetDlgItem(window, 230)), timeout=15)
            USER.PostMessageW(window, 0x0111, 230, 0)
            wait_for(lambda: len(phone.reads) > reads, timeout=8)
            reply = app.request(start_frame("interrupt"))
            wait_for(lambda: "DIRECTORY_SYNC_DEFERRED" in app.log(), timeout=3)
            assert not USER.IsWindowEnabled(USER.GetDlgItem(window, 230))
            assert phone.reads[-1] == "/contacts.xml"
            app.stop_session("interrupt")
            phone.delay = 0
            checks.append("in-flight background XML read yields to a recording before calllog request")
            assert not phone.errors, phone.errors
            assert "測試" not in app.log() and "90001" not in app.log()
            phone.contacts = '<contacts><contact><name>測試快取</name><tel1>90001</tel1></contact></contacts>'
            synchronize(app, window)
            assert record(app, "identity-baseline", "90001") == "測試快取"
        # A different configured web identity must not reuse the previous phone directory.
        config = app.config.read_text(encoding="utf-8").replace("phone_user=offline", "phone_user=other")
        app.config.write_text(config, encoding="utf-8")
        app.args.append("--offline")
        before_reads = len(phone.reads)
        with app:
            window = main_window(app)
            assert record(app, "other-identity", "90001") == ""
            assert not USER.IsWindowEnabled(USER.GetDlgItem(window, 230))
            assert len(phone.reads) == before_reads
            checks.append("offline mode sends no phone requests and disables sync; cache is scoped to configured identity")
        reads = len(phone.reads)
        automatic = App(root, args.exe.resolve(), output, "automatic-startup", offline=False,
                        phone_port=phone.port, password=phone.password)
        with automatic:
            wait_for(lambda: "DIRECTORY_SYNC_OK" in automatic.log(), timeout=25)
            assert len(phone.reads) >= reads + 2
            assert record(automatic, "automatic-name", "90001") == "測試快取"
            checks.append("normal automatic-event startup synchronizes after login and compatibility inspection")
        deferred = App(root, args.exe.resolve(), output, "deferred-startup", offline=False, event_port=0,
                       phone_port=phone.port, password=phone.password)
        with deferred:
            wait_for(lambda: "HTTP_SEND method=GET path=/ bytes=" in deferred.log(), timeout=10)
            reply = deferred.request(start_frame("early-call"))
            assert reply["result"] == "success"
            wait_for(lambda: "OPERATION_END operation=directory" in deferred.log(), timeout=5)
            assert "DIRECTORY_SYNC_OK" not in deferred.log()
            deferred.stop_session("early-call")
            wait_for(lambda: "DIRECTORY_SYNC_OK" in deferred.log(), timeout=35)
            assert record(deferred, "deferred-name", "90001") == "測試快取"
            checks.append("external-trigger startup login yields to an early call and synchronizes automatically after hangup")
    (output / "directory-results.json").write_text(json.dumps({"passed": True, "real_phone_contacted": False, "checks": checks}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"PASS: {len(checks)} directory integration checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
