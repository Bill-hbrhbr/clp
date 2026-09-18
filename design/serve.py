#!/usr/bin/env python3
"""Render and serve the markdown files in this directory as HTML.

Usage:  .venv/bin/python serve.py [port]
Default port: 48217
"""

from __future__ import annotations

import html
import pathlib
import socket
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import markdown

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_PORT = 48217
TITLE = "CLP Design Docs"

# Markdown extensions used for rendering.
EXTENSIONS = ["tables", "fenced_code", "toc", "sane_lists"]

STYLE = """
body { font: 15px/1.6 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
       max-width: 920px; margin: 2rem auto; padding: 0 1rem; color: #1f2328; background: #fff; }
h1, h2, h3 { line-height: 1.25; margin-top: 1.5em; }
table { border-collapse: collapse; margin: 1em 0; }
th, td { border: 1px solid #d0d7de; padding: 6px 13px; text-align: left; }
tr:nth-child(even) { background: #f6f8fa; }
code { background: #f6f8fa; padding: .2em .4em; border-radius: 6px; font-size: 85%; }
pre { background: #f6f8fa; padding: 1em; border-radius: 8px; overflow-x: auto; }
pre code { background: none; padding: 0; font-size: 13px; }
blockquote { border-left: 4px solid #d0d7de; margin: 1em 0; padding: 0 1em; color: #57606a; }
a { color: #0969da; }
.index li { margin: .3em 0; }
"""


def render(md_path: pathlib.Path) -> str:
    text = md_path.read_text(encoding="utf-8")
    body = markdown.markdown(text, extensions=EXTENSIONS)
    return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>{html.escape(md_path.name)}</title>
<style>{STYLE}</style>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
</head>
<body><h1>{html.escape(TITLE)}</h1>{body}
<script>
  document.querySelectorAll('pre > code.language-mermaid').forEach(function(code) {{
    var pre = code.parentElement;
    var div = document.createElement('div');
    div.className = 'mermaid';
    div.textContent = code.textContent;
    pre.replaceWith(div);
  }});
  if (window.mermaid) {{ mermaid.initialize({{startOnLoad: false}}); mermaid.run(); }}
</script>
</body></html>"""


def index_page() -> str:
    files = sorted(p for p in HERE.glob("*.md"))
    items = "".join(
        f'<li><a href="/{f.name}">{html.escape(f.name)}</a></li>' for f in files
    )
    return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>{html.escape(TITLE)}</title>
<style>{STYLE}</style></head>
<body><h1>{html.escape(TITLE)}</h1><h2>Documents</h2>
<ul class="index">{items}</ul></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        path = pathlib.Path(self.path.lstrip("/"))
        if self.path == "/" or self.path == "":
            self._send(200, index_page())
            return
        # Strip query string
        name = pathlib.Path(path.parts[0] if path.parts else "")
        target = HERE / name
        if not target.is_file() or target.suffix != ".md":
            self._send(HTTPStatus.NOT_FOUND, "Not found")
            return
        try:
            self._send(200, render(target))
        except Exception as e:  # noqa: BLE001
            self._send(HTTPStatus.INTERNAL_SERVER_ERROR, f"Render error: {e}")

    def _send(self, status: int, body: str) -> None:
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt: str, *args) -> None:  # quieter logs
        sys.stderr.write(f"{self.address_string()} - {fmt % args}\n")


def lan_ip() -> str:
    """Best-effort primary interface IP (no packets actually sent)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    host = "0.0.0.0"
    server = ThreadingHTTPServer((host, port), Handler)
    print(f"Serving {HERE} on http://{lan_ip()}:{port}  (Ctrl-C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())