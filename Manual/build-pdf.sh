#!/usr/bin/env bash
# build-pdf.sh: the manual as PDFs, from the Markdown sources next to this script.
#
#   ./build-pdf.sh
#
# Writes pdf/: one PDF per chapter (the GUI's Help menu opens these) and
# TreeSync-Manual.pdf with all of them. The .md files are the sources: edit those and
# run this again. Needs python3 with its markdown module (RHEL: python3-markdown) and
# a Chrome or Chromium (headless, to print HTML to PDF), plus pdfunite (poppler-utils).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/pdf"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

BROWSER=""
for b in google-chrome chromium chromium-browser; do
  command -v "$b" > /dev/null && { BROWSER="$b"; break; }
done
[ -n "$BROWSER" ] || { echo "no Chrome or Chromium found (needed to print the PDFs)" >&2; exit 1; }
python3 -c "import markdown" 2> /dev/null || { echo "python3 markdown module missing (RHEL: sudo dnf install python3-markdown)" >&2; exit 1; }
command -v pdfunite > /dev/null || { echo "pdfunite missing (RHEL: sudo dnf install poppler-utils)" >&2; exit 1; }

# Markdown -> HTML, one page per chapter. Links to another chapter's .md go to its PDF.
python3 - "$HERE" "$WORK" <<'PY'
import markdown, os, re, sys
src, work = sys.argv[1], sys.argv[2]
css = """
@page { size: A4; margin: 18mm 16mm 18mm 16mm; }
body { font-family: "DejaVu Sans", "Liberation Sans", sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1a1a1a; }
h1 { font-size: 20pt; border-bottom: 2px solid #2a5db0; padding-bottom: 4px; color: #1d3f78; }
h2 { font-size: 14pt; margin-top: 1.4em; color: #1d3f78; break-after: avoid; }
h3 { font-size: 11.5pt; margin-top: 1.1em; break-after: avoid; }
code { font-family: "DejaVu Sans Mono", "Liberation Mono", monospace; font-size: 9pt; background: #f2f4f7; padding: 0 2px; }
pre { background: #f2f4f7; border: 1px solid #d5dae1; padding: 8px 10px; font-size: 8.5pt; line-height: 1.3;
      white-space: pre-wrap; break-inside: avoid; }
pre code { background: none; padding: 0; font-size: 8.5pt; }
table { border-collapse: collapse; margin: 0.6em 0; width: 100%; break-inside: auto; }
th, td { border: 1px solid #c8ced6; padding: 4px 6px; vertical-align: top; text-align: left; }
th { background: #e8edf5; }
tr { break-inside: avoid; }
blockquote { border-left: 4px solid #d9822b; background: #fdf3e7; margin: 0.8em 0; padding: 4px 12px; }
dt { font-weight: bold; margin-top: 0.7em; }
dd { margin-left: 1.5em; }
a { color: #2a5db0; text-decoration: none; }
.footer { margin-top: 2em; font-size: 8pt; color: #777; border-top: 1px solid #ddd; padding-top: 4px; }
"""
chapters = sorted(f for f in os.listdir(src) if re.match(r"\d-.*\.md$", f))
for name in ["README.md"] + chapters:
    text = open(os.path.join(src, name), encoding="utf-8").read()
    # chapter links: X.md(#anchor) -> X.pdf (a PDF viewer opens the chapter)
    text = re.sub(r"\(((?:\d-[^)#]+|README))\.md(#[^)]*)?\)",
                  lambda m: "(" + ("0-Contents" if m.group(1) == "README" else m.group(1)) + ".pdf)", text)
    body = markdown.markdown(text, extensions=["tables", "fenced_code", "def_list", "toc", "sane_lists"])
    base = "0-Contents" if name == "README.md" else name[:-3]
    title = re.search(r"^# (.+)$", text, re.M).group(1)
    html = ('<!doctype html><html><head><meta charset="utf-8"><title>%s</title><style>%s</style></head>'
            '<body>%s<div class="footer">TreeSync Manual - %s</div></body></html>') % (title, css, body, title)
    open(os.path.join(work, base + ".html"), "w", encoding="utf-8").write(html)
    print(base)
PY

# HTML -> PDF, then all of them in one.
parts=()
for html in "$WORK"/*.html; do
  base="$(basename "$html" .html)"
  "$BROWSER" --headless=new --disable-gpu --no-pdf-header-footer --no-sandbox \
             --print-to-pdf="$OUT/$base.pdf" "file://$html" > /dev/null 2>&1
  [ -s "$OUT/$base.pdf" ] || { echo "could not print $base" >&2; exit 1; }
  parts+=("$OUT/$base.pdf")
  echo "  pdf/$base.pdf"
done
pdfunite "${parts[@]}" "$OUT/TreeSync-Manual.pdf"
echo "  pdf/TreeSync-Manual.pdf (all chapters)"
