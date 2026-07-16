#!/usr/bin/env python3
import http.server
import json
import os
import threading
import time
import urllib.parse

HTDOCS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lib', 'lwip', 'httpd', 'htdocs')

MOCK_UBOOT_VERSION = "U-Boot 2026.07-00762-g59060dde7b91 (Jul 12 2026 - 03:24:15 +0000)"
MOCK_DETECTED_LAYOUT = "2.0"

prog_phase = 0
prog_done = 0
prog_total = 0
prog_erase_done = 0
prog_erase_total = 0
prog_write_done = 0
prog_write_total = 0
prog_lock = threading.Lock()


def reset_progress():
    global prog_phase, prog_done, prog_total
    global prog_erase_done, prog_erase_total
    global prog_write_done, prog_write_total
    with prog_lock:
        prog_phase = 0
        prog_done = 0
        prog_total = 0
        prog_erase_done = 0
        prog_erase_total = 0
        prog_write_done = 0
        prog_write_total = 0


def simulate_flash(target, layout, size):
    global prog_phase, prog_done, prog_total
    global prog_erase_done, prog_erase_total
    global prog_write_done, prog_write_total

    total_size = max(size, 1 * 1024 * 1024)
    erase_size = total_size

    with prog_lock:
        prog_phase = 1
        prog_total = total_size + erase_size
        prog_erase_total = erase_size
        prog_write_total = total_size

    for i in range(50):
        time.sleep(0.06)
        with prog_lock:
            if prog_phase != 1:
                return
            prog_erase_done = int(erase_size * (i + 1) / 50)
            prog_done = prog_erase_done

    with prog_lock:
        prog_phase = 2
        prog_erase_done = erase_size
        prog_done = erase_size

    for i in range(100):
        time.sleep(0.05)
        with prog_lock:
            if prog_phase != 2:
                return
            prog_write_done = int(total_size * (i + 1) / 100)
            prog_done = erase_size + prog_write_done

    with prog_lock:
        prog_phase = 3
        prog_write_done = total_size
        prog_done = total_size + erase_size


class RecoveryHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == '/' or path == '/index.html':
            self.serve_static('index.html')
            return
        if path == '/fail.html':
            self.serve_static('fail.html')
            return
        if path == '/about':
            self.send_json({
                "u_boot": MOCK_UBOOT_VERSION,
                "detected_layout": MOCK_DETECTED_LAYOUT
            })
            return
        if path == '/status':
            with prog_lock:
                ok = (prog_phase == 3)
                err = (prog_phase == -1)
                in_progress = (prog_phase > 0 and prog_phase < 3)
                self.send_json({
                    "in_progress": 1 if in_progress else 0,
                    "done": prog_done,
                    "total": prog_total,
                    "erase_done": prog_erase_done,
                    "erase_total": prog_erase_total,
                    "write_done": prog_write_done,
                    "write_total": prog_write_total,
                    "ok": 1 if ok else 0,
                    "error": 1 if err else 0,
                    "phase": prog_phase
                })
            return

        self.send_error(404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        content_length = int(self.headers.get('Content-Length', 0))

        if path.startswith('/upload/firmware'):
            layout = query.get('layout', ['2.0'])[0]
            if layout not in ('2.0', '1.5', '1.0'):
                self.send_response(400)
                self.end_headers()
                return

            if content_length < 1 * 1024 * 1024:
                print(f"[mock] content_len {content_length} below min for firmware")
                self.send_response(400)
                self.end_headers()
                return

            body = self.rfile.read(content_length)
            print(f"[mock] firmware upload: {content_length} bytes, layout={layout}")

            reset_progress()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(b'<html><body>OK</body></html>')

            t = threading.Thread(target=simulate_flash, args=('firmware', layout, content_length))
            t.daemon = True
            t.start()
            return

        if path.startswith('/upload/uboot'):
            if content_length > 1 * 1024 * 1024:
                print(f"[mock] content_len {content_length} exceeds max for uboot")
                self.send_response(400)
                self.end_headers()
                return

            body = self.rfile.read(content_length)
            print(f"[mock] uboot upload: {content_length} bytes")

            reset_progress()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(b'<html><body>OK</body></html>')

            t = threading.Thread(target=simulate_flash, args=('uboot', None, content_length))
            t.daemon = True
            t.start()
            return

        self.send_error(404)

    def serve_static(self, filename):
        filepath = os.path.join(HTDOCS_DIR, filename)
        if not os.path.isfile(filepath):
            self.send_error(404)
            return
        with open(filepath, 'rb') as f:
            content = f.read()
        self.send_response(200)
        if filename.endswith('.html'):
            self.send_header('Content-Type', 'text/html; charset=utf-8')
        else:
            self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def send_json(self, data):
        body = (json.dumps(data) + '\n').encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        print(f"[mock] {args[0]} - {args[1] if len(args) > 1 else ''}")


def main():
    port = 8080
    server = http.server.HTTPServer(('127.0.0.1', port), RecoveryHandler)
    print(f"XR1710G Recovery Mock Server running at http://127.0.0.1:{port}")
    print(f"  - GET  /about    -> version info")
    print(f"  - GET  /status   -> progress status")
    print(f"  - POST /upload/firmware?layout=2.0")
    print(f"  - POST /upload/uboot")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == '__main__':
    main()
