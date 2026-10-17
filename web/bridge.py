import json, queue, threading, serial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

PORT = "/dev/ttyUSB0"
clients = []
lock = threading.Lock()

def read_serial():
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = 115200
    ser.dtr = False   # set before open, so the board is not reset
    ser.rts = False
    ser.open()
    print("Serial port open:", PORT)
    ser.readline()    # throw away the first partial line
    while True:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
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

    def log_message(self, *args):
        pass

threading.Thread(target=read_serial, daemon=True).start()
print("Open http://localhost:8000/dashboard.html")
ThreadingHTTPServer(("", 8000), Handler).serve_forever()
