"""Read-only localhost server for the version 1 HD asset cache."""
import argparse
import json
import mimetypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

APP = Path(__file__).resolve().parent
DEFAULT_CACHE = APP.parents[1] / 'build' / 'hd' / 'cache'
BUILD_COMMAND = r'python -E -P scripts\hd\build_cache.py ...'
mimetypes.add_type('text/javascript', '.js')
mimetypes.add_type('model/gltf+json', '.gltf')


def resolve_under(root, relative):
    if '\\' in relative or ':' in relative or '\x00' in relative:
        raise ValueError('Invalid path')
    target = (root / relative).resolve()
    if not target.is_relative_to(root.resolve()):
        raise ValueError('Path outside served directory')
    return target


def merge_alt_models(cache, external, data):
    """Attach alt_models.json entries to assets (by id, then case-insensitive name).

    Each alt gets a browser `url` under /cache/ or /external/. Missing or invalid
    files are skipped; the manifest still loads.
    """
    alt_file = cache / 'alt_models.json'
    if not alt_file.is_file():
        return
    try:
        alts = json.loads(alt_file.read_text(encoding='utf-8-sig'))['alt_models']
        if not isinstance(alts, list):
            raise ValueError('alt_models is not a list')
    except (OSError, ValueError, KeyError, TypeError) as error:
        data['alt_models_note'] = f'alt_models.json ignored: {error}'
        return
    by_id = {a.get('id'): a for a in data['assets']}
    by_name = {str(a.get('name', '')).lower(): a for a in data['assets']}
    for alt in alts:
        try:
            target = by_id.get(alt['asset_id']) or by_name.get(str(alt['asset_id']).lower())
            if target is None:
                continue
            model = (cache / alt['model']).resolve()
            for prefix, base in (('/cache/', cache), ('/external/', external)):
                if model.is_relative_to(base.resolve()):
                    url = prefix + '/'.join(model.relative_to(base.resolve()).parts)
                    break
            else:
                continue
            if model.suffix.lower() not in ('.gltf', '.glb') or not model.is_file():
                continue
            target.setdefault('alt_models', []).append({'id': str(alt['id']), 'label': str(alt.get('label', alt['id'])), 'url': url})
        except (KeyError, TypeError, ValueError, OSError):
            continue


def handler_for(cache, external):
    class Handler(BaseHTTPRequestHandler):
        def do_HEAD(self):
            self.respond(head=True)

        def do_GET(self):
            self.respond()

        def send_bytes(self, status, data, mime, head=False):
            self.send_response(status)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            if not head:
                self.wfile.write(data)

        def respond(self, head=False):
            path = unquote(urlsplit(self.path).path)
            if path == '/api/manifest':
                try:
                    data = json.loads(resolve_under(cache, 'manifest.json').read_text(encoding='utf-8-sig'))
                    if not isinstance(data, dict) or data.get('version') != 1 or not isinstance(data.get('assets'), list) or not isinstance(data.get('variants'), list):
                        raise ValueError('Expected manifest version 1 with assets and variants arrays')
                    merge_alt_models(cache, external, data)
                    payload = data
                except (OSError, ValueError) as error:
                    payload = {'version': 1, 'assets': [], 'variants': [], 'message': f'Cache unavailable: {error}', 'build_command': BUILD_COMMAND}
                self.send_bytes(200, json.dumps(payload).encode(), 'application/json; charset=utf-8', head)
                return
            if path == '/favicon.ico':
                self.send_bytes(204, b'', 'image/x-icon', head)
                return
            try:
                if path.startswith('/cache/'):
                    target = resolve_under(cache, path[len('/cache/'):])
                elif path.startswith('/external/'):
                    target = resolve_under(external, path[len('/external/'):])
                elif path.startswith('/vendor/three/'):
                    target = resolve_under(APP / 'node_modules' / 'three', path[len('/vendor/three/'):])
                elif path in ('/', '/index.html', '/app.js', '/style.css'):
                    target = APP / ('index.html' if path == '/' else path[1:])
                else:
                    raise FileNotFoundError()
                if not target.is_file():
                    raise FileNotFoundError()
                self.send_bytes(200, target.read_bytes(), mimetypes.guess_type(target.name)[0] or 'application/octet-stream', head)
            except ValueError:
                self.send_bytes(403, b'Forbidden', 'text/plain', head)
            except OSError:
                self.send_bytes(404, b'File unavailable', 'text/plain', head)
    return Handler


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=DEFAULT_CACHE)
    parser.add_argument('--external', type=Path, default=None, help='read-only third-party model dir (default: <cache>/../external)')
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), handler_for(args.cache.resolve(), (args.external or args.cache.parent / 'external').resolve()))
    print(f'Asset viewer: http://127.0.0.1:{server.server_port} | cache: {args.cache.resolve()}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
