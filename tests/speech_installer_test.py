"""Run the Windows PowerShell installer error paths against synthetic loopback responses only."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading
from product_smoke import startup_info


class Fixture(BaseHTTPRequestHandler):
    """Return deterministic small HTTP bodies; no model bytes or outbound requests."""
    def do_GET(self):
        payload=b'synthetic-asset'
        self.send_response(503 if self.path == '/unavailable' else 200)
        self.send_header('Content-Length',str(len(payload)))
        self.end_headers();self.wfile.write(payload)

    def log_message(self,*args):
        """Keep test evidence free of incidental endpoint logs."""


def main():
    """Execute the real functions without invoking the main installation or touching a user cache."""
    server=ThreadingHTTPServer(('127.0.0.1',0),Fixture)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='PhoneRecord-install-errors-') as root:
            result=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(Path(__file__).with_name('speech_installer.ps1')),
                            '-Root',root,'-TestUrl',f'http://127.0.0.1:{server.server_port}'],capture_output=True,text=True,encoding='utf-8',errors='replace',startupinfo=startup_info(),creationflags=subprocess.CREATE_NO_WINDOW)
            assert result.returncode == 0, result.stdout+result.stderr
            print(result.stdout.strip())
    finally:
        server.shutdown();server.server_close();thread.join()


if __name__ == '__main__':
    main()
