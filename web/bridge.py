import json, queue, threading, serial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

PORT = "/dev/ttyUSB0"
clients = []
lock = threading.Lock()
write_lock = threading.Lock()
ser = None


def read_serial():
    global ser
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.dtr = False   # set before open, so the board is not reset
    s.rts = False
    s.open()
    ser = s
    print("Serial port open:", PORT)
    s.readline()    # throw away the first partial line
    while True:
        line = s.readline().decode("utf-8", errors="ignore").strip()
        if not line:
            continue
        with lock:
            for q in clients:
                q.put(line)


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/events":
            return super().do_GET()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        q = queue.Queue()
        with lock:
            clients.append(q)
        try:
            while True:
                line = q.get()
                self.wfile.write(("data: " + json.dumps(line) + "\n\n").encode())
                self.wfile.flush()
        except Exception:
            pass
        finally:
            with lock:
                clients.remove(q)

    def do_POST(self):
        if self.path != "/command" or ser is None:
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > 256:
            self.send_response(400)
            self.end_headers()
            return
        body = self.rfile.read(length).decode("utf-8", errors="ignore")
        try:
            json.loads(body)            # only forward valid JSON
        except ValueError:
            self.send_response(400)
            self.end_headers()
            return
        with write_lock:
            ser.write((body.strip() + "\n").encode())
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


threading.Thread(target=read_serial, daemon=True).start()
print("Open http://localhost:8000/dashboard.html")
ThreadingHTTPServer(("", 8000), Handler).serve_forever()
