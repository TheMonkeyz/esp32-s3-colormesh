#!/usr/bin/env python3
"""Checks docs/DIAGRAMS.md, the levelled architecture diagrams that docs/diagrams.html (the zoomable explorer)
reads as is. The explorer turns each "Zoom in" line into a clickable box, so the rules it relies on are checked
here (CI's host-tests job runs this; run it by hand after editing the file):

  - one Mermaid diagram per section (every "## " heading);
  - each "Zoom in" line names a box by its id in that section's diagram and links to a section that exists, with
    the section's exact title as the link text, and never to itself;
  - every section can be reached by zooming in from the first one, and is listed in the map at the top;
  - the map's nesting is the explorer's breadcrumb trail, so each parent on it zooms into its children;
  - no ';' inside a sequence-diagram message (it ends the statement and breaks the render) and no Mermaid flowchart
    keyword used as a node id (the usual traps: end, call, click, style, class, default, subgraph, graph).

Exit code 0 when everything holds, 1 with one line per problem. Usage: python tools/check_diagrams.py [path]
"""
import re
import sys
from collections import deque
from pathlib import Path

ZOOM = re.compile(r"^- (.*)\(`([\w-]+)`\) → \[([^\]]+)\]\(#([\w-]+)\)\s*$", re.M)
MERMAID = re.compile(r"```mermaid\n(.*?)```", re.S)
LINK = re.compile(r"\]\(#([\w-]+)\)")
MAP_ITEM = re.compile(r"^( *)- \[[^\]]+\]\(#([\w-]+)\)\s*$", re.M)
FLOW_KEYWORDS = {"end", "call", "click", "style", "linkStyle", "classDef", "class", "default", "subgraph", "graph",
                 "flowchart", "direction", "href", "interpolate"}
NODE_DEF = re.compile(r"(?:^|[\s&>-])([A-Za-z_][\w]*)[\[\(\{]")


def slug(title):
    """GitHub's heading anchor: lower case, punctuation dropped, spaces to hyphens."""
    out = []
    for ch in title.lower():
        if ch.isascii() and (ch.isalnum() or ch in "-_"):
            out.append(ch)
        elif ch == " ":
            out.append("-")
    return "".join(out)


def load(path):
    text = Path(path).read_text(encoding="utf-8").replace("\r", "")
    parts = ("\n" + text).split("\n## ")
    intro = parts[0]
    sections = []
    for p in parts[1:]:
        title, _, body = p.partition("\n")
        diagrams = MERMAID.findall(body)
        zooms = [dict(label=m[0].strip(), box=m[1], title=m[2], slug=m[3]) for m in ZOOM.findall(body)]
        sections.append(dict(title=title, slug=slug(title), body=body, diagrams=diagrams,
                             diagram=diagrams[0] if diagrams else "", zooms=zooms))
    return intro, sections


def check(path):
    problems = []
    intro, sections = load(path)
    if not sections:
        return ["no sections"]
    by_slug = {s["slug"]: s for s in sections}

    for s in sections:
        if len(s["diagrams"]) != 1:
            problems.append(f'"{s["title"]}": {len(s["diagrams"])} diagrams, want 1')
        if "**Zoom in:**" in s["body"] and not s["zooms"]:
            problems.append(f'"{s["title"]}": a Zoom in list the explorer can\'t read')
        d = s["diagram"]
        first = d.strip().split("\n", 1)[0].strip() if d else ""
        if first.startswith("sequenceDiagram"):
            for line in d.split("\n"):
                if re.match(r"\s*\w+\s*(-->>|->>|-->|->|-x|--x|-\))", line) and ";" in line:
                    problems.append(f'"{s["title"]}": a ";" inside a sequence message breaks the render: {line.strip()}')
        elif first.startswith(("flowchart", "graph")):
            for m in NODE_DEF.finditer(d):
                if m.group(1) in FLOW_KEYWORDS:
                    problems.append(f'"{s["title"]}": "{m.group(1)}" is a Mermaid keyword, not a usable node id')
        for z in s["zooms"]:
            box = re.compile(r"(^|[\s&>-])" + re.escape(z["box"]) + r"[\[\(\{]")
            if not box.search(d):
                problems.append(f'"{s["title"]}": no box "{z["box"]}" in its diagram')
            target = by_slug.get(z["slug"])
            if target is None:
                problems.append(f'"{s["title"]}": links to #{z["slug"]}, which isn\'t a section')
            elif target["title"] != z["title"]:
                problems.append(f'"{s["title"]}": link text "{z["title"]}", but the section is "{target["title"]}"')
            if z["slug"] == s["slug"]:
                problems.append(f'"{s["title"]}": zooms into itself')
    if not any(s["zooms"] for s in sections):
        problems.append("no Zoom in links at all")

    # reachability from the top, and the map
    seen = {sections[0]["slug"]}
    queue = deque([sections[0]["slug"]])
    while queue:
        for z in by_slug[queue.popleft()]["zooms"]:
            if z["slug"] in by_slug and z["slug"] not in seen:
                seen.add(z["slug"])
                queue.append(z["slug"])
    on_map = set(LINK.findall(intro))
    stack = []
    for m in MAP_ITEM.finditer(intro):
        depth = len(m.group(1)) // 2
        stack = stack[:depth] + [m.group(2)]
        if depth > 0 and len(stack) > 1:
            parent = by_slug.get(stack[-2])
            if parent and not any(z["slug"] == m.group(2) for z in parent["zooms"]):
                problems.append(f'the map puts #{m.group(2)} under "{parent["title"]}", which has no Zoom in link to it')
        if m.group(2) not in by_slug:
            problems.append(f"the map lists #{m.group(2)}, which isn't a section")
    for s in sections:
        if s["slug"] not in seen:
            problems.append(f'"{s["title"]}" can\'t be reached by zooming in from "{sections[0]["title"]}"')
        if s["slug"] not in on_map:
            problems.append(f'"{s["title"]}" is missing from the map at the top')
    return problems


def main(argv):
    path = argv[1] if len(argv) > 1 else Path(__file__).resolve().parent.parent / "docs" / "DIAGRAMS.md"
    problems = check(path)
    for p in problems:
        print(p)
    _, sections = load(path)
    zooms = sum(len(s["zooms"]) for s in sections)
    print(f"{'FAIL' if problems else 'ok'}: {len(sections)} sections, {zooms} zoom links, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
