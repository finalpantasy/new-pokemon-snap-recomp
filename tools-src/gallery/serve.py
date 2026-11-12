"""Localhost photo gallery: python -E -P serve.py [--profile P] [--port 8777 --port-file F] [--no-open]. Also: open_gallery(profile)."""
import argparse, subprocess, sys, webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

APP = Path(__file__).resolve().parent
sys.path.insert(0, str(APP))
import gallery


def make_handler(data_dir):
    root = Path(data_dir).resolve()

    class H(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            p = path.split('?')[0]
            if p in ('/', '/index.html'):
                return str(APP / 'index.html')
            if p.startswith('/data/'):
                t = (root / p[6:]).resolve()
                if t.is_relative_to(root):
                    return str(t)
            return str(APP / 'does-not-exist')

        def log_message(self, *a):
            pass
    return H


class _Srv(ThreadingHTTPServer):
    allow_reuse_address = False   # on Windows SO_REUSEADDR lets a second socket share a busy port; we want the bind to fail instead


def serve(profile='default', port=8777, open_browser=True, refresh=True, port_file=None):
    out = gallery.ROOT / 'build' / 'gallery' / profile
    if refresh:
        try:
            gallery.main(['--profile', profile])
        except SystemExit as e:
            print(e)
    out.mkdir(parents=True, exist_ok=True)
    srv = None
    for p in dict.fromkeys([port, 8777, 0]):   # asked port, then 8777, then any free port chosen by the OS
        try:
            srv = _Srv(('127.0.0.1', p), make_handler(out))
            break
        except OSError:
            continue
    if srv is None:
        raise SystemExit('no free port')
    port = srv.server_address[1]
    if port_file:
        Path(port_file).write_text(str(port))   # the F1 menu reads this to know which port to open
    url = 'http://127.0.0.1:%d/' % port
    print('gallery at', url, flush=True)
    if open_browser:
        webbrowser.open(url)
    srv.serve_forever()


def open_gallery(profile='default', port=8777):
    """Hook for the F1 menu: launches the server detached, returns its URL."""
    subprocess.Popen([sys.executable, '-E', '-P', str(APP / 'serve.py'), '--profile', profile, '--port', str(port)],
                     creationflags=0x08000000 if sys.platform == 'win32' else 0)
    return 'http://127.0.0.1:%d/' % port


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--profile', default='default')
    ap.add_argument('--port', type=int, default=8777)
    ap.add_argument('--port-file')
    ap.add_argument('--no-open', action='store_true')
    ap.add_argument('--no-refresh', action='store_true')
    a = ap.parse_args()
    serve(a.profile, a.port, not a.no_open, not a.no_refresh, a.port_file)
