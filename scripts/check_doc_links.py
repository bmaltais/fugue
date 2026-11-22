#!/usr/bin/env python3
"""Checks the links in README.md, docs/*.md and docs/**/*.md: a relative link must name a file that
exists, and a #fragment must be a heading of the page it points at (GitHub's anchor rule).

    scripts/check_doc_links.py        # prints each broken link, exits 1 if there are any
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|!\[[^\]]*\]\(([^)\s]+)\)|(?:src|href)=\"([^\"]+)\"")
problems = []


def anchors(path):
    found = set()
    seen = {}
    in_code = False
    for line in open(path, encoding="utf-8"):
        if line.startswith("```"):
            in_code = not in_code
        m = None if in_code else re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
        if m:
            text = re.sub(r"[`*_]|\[([^\]]*)\]\([^)]*\)", lambda x: x.group(1) or "", m.group(1))
            slug = re.sub(r"[^\w\- ]", "", text.lower()).replace(" ", "-")
            n = seen.get(slug, 0)
            seen[slug] = n + 1
            found.add(slug if n == 0 else f"{slug}-{n}")
    return found


cache = {}
pages = [os.path.join(ROOT, "README.md")] + sorted(glob.glob(os.path.join(ROOT, "docs", "**", "*.md"), recursive=True))
for page in pages:
    text = open(page, encoding="utf-8").read()
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    for match in LINK.finditer(text):
        target = next(g for g in match.groups() if g)
        if re.match(r"^[a-z][a-z0-9+.-]*:", target):
            continue
        path, _, fragment = target.partition("#")
        dest = page if not path else os.path.normpath(os.path.join(os.path.dirname(page), path))
        where = os.path.relpath(page, ROOT)
        if not os.path.exists(dest):
            problems.append(f"{where}: {target} does not exist")
        elif fragment and dest.endswith(".md"):
            if dest not in cache:
                cache[dest] = anchors(dest)
            if fragment not in cache[dest]:
                problems.append(f"{where}: {target}: no such heading")
for problem in problems:
    print(problem, file=sys.stderr)
print(f"{len(pages)} pages, {len(problems)} broken links")
sys.exit(1 if problems else 0)
