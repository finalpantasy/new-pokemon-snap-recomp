import json, sys, tempfile, threading, unittest, urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gallery, serve

JPG = b'\xff\xd8\xff\xe0\x00\x10JFIF\x00' + b'x' * 20 + b'\xff\xd9'


class F:
    def __init__(s, ents):
        s.e, s.count = ents, 1200

    def read(s, i):
        return ({}, bytearray(s.e[i])) if i in s.e else None


class T(unittest.TestCase):
    def test_extract(self):
        files = {7: F({3: bytes(16) + JPG + b'\0' * 9, 5: bytes(16) + JPG}), 10: F({3: bytes(16) + JPG}),
                 6: F({3: bytes(0x148)})}
        with tempfile.TemporaryDirectory() as d:
            ph = gallery.extract(files, d)
            self.assertEqual([p['id'] for p in ph], [3, 5])
            self.assertEqual((Path(d) / 'full/0003.jpg').read_bytes(), JPG)
            self.assertIn('thumb', ph[0])
            self.assertNotIn('thumb', ph[1])
            self.assertEqual(len(ph[0]['raw_words']), 0x148 // 4)

    def test_serve(self):
        with tempfile.TemporaryDirectory() as d:
            gallery.extract({7: F({1: bytes(16) + JPG})}, d)
            srv = ThreadingHTTPServer(('127.0.0.1', 0), serve.make_handler(Path(d)))
            threading.Thread(target=srv.serve_forever, daemon=True).start()
            u = 'http://127.0.0.1:%d' % srv.server_address[1]
            self.assertIn(b'Photo gallery', urllib.request.urlopen(u + '/').read())
            self.assertEqual(json.load(urllib.request.urlopen(u + '/data/photos.json'))['photos'][0]['id'], 1)
            self.assertEqual(urllib.request.urlopen(u + '/data/full/0001.jpg').read(), JPG)
            with self.assertRaises(Exception):
                urllib.request.urlopen(u + '/data/../serve.py')
            srv.shutdown()


if __name__ == '__main__':
    unittest.main()
