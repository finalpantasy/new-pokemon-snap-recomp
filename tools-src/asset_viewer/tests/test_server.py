"""Exercise real HTTP responses, including incomplete caches, using stdlib only."""
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import urlopen
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('viewer_server', ROOT / 'serve.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'tests')
        self.cache = Path(self.temp.name) / 'cache'
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), module.handler_for(self.cache, self.cache.parent / 'external'))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def manifest(self):
        with urlopen(self.base + '/api/manifest') as response:
            return json.load(response)

    def test_missing_cache(self):
        data = self.manifest()
        self.assertEqual(data['assets'], [])
        self.assertIn('Cache unavailable', data['message'])
        self.assertEqual(data['build_command'], r'python -E -P scripts\hd\build_cache.py ...')

    def test_empty_partial_and_unsupported(self):
        self.cache.mkdir()
        path = self.cache / 'manifest.json'
        path.write_text('{', encoding='utf-8')
        self.assertEqual(self.manifest()['assets'], [])
        path.write_text(json.dumps({'version':2,'assets':[],'variants':[]}), encoding='utf-8')
        self.assertIn('version 1', self.manifest()['message'])
        path.write_text(json.dumps({'version':1,'assets':[],'variants':[]}), encoding='utf-8')
        self.assertNotIn('message', self.manifest())

    def test_static_security(self):
        for path, status in [('/cache/%2e%2e%2fserve.py',403),('/cache/C%3A/private',403),('/cache/a%5Cb',403),('/package.json',404),('/testdata/manifest.json',404)]:
            with self.subTest(path=path), self.assertRaises(HTTPError) as raised:
                urlopen(self.base + path)
            self.assertEqual(raised.exception.code, status)
            raised.exception.close()
        with urlopen(self.base + '/app.js') as response:
            self.assertEqual(response.headers['Content-Type'],'text/javascript')
            self.assertEqual(response.headers['X-Content-Type-Options'],'nosniff')

    def test_default_cache(self):
        self.assertEqual(module.DEFAULT_CACHE, ROOT.parents[1] / 'build' / 'hd' / 'cache')


if __name__ == '__main__': unittest.main()
