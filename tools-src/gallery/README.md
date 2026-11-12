# Photo gallery

Local, private viewer for saved photos (runtime data, git-ignored under `build\gallery\<profile>\`).

    python -E -P tools-src\gallery\serve.py [--profile default] [--port 8777] [--port-file F] [--no-open]
    python -E -P tools-src\gallery\gallery.py --profile default      # extract only
    python -I -m unittest discover -s tools-src\gallery\tests

`serve.py` re-extracts from `%APPDATA%\SnapRecomp\save` (or `profiles\<id>\save`) then serves http://127.0.0.1:8777/ (or the next free port; the chosen one is printed and written to --port-file) with course and
species filters, sort by score or date, and a full-size lightbox. The F1 menu hook is `serve.open_gallery(profile)` (detached, returns the URL).
Course/species/score/date need `META_LAYOUT` in `gallery.py` filled once the 0x148 metadata layout is decoded from a real photo run;
until then records carry `raw_words`. Format: `analysis\save\album.md`.
