#!/usr/bin/env python3
"""Lint src/help.json against what the device will actually draw.

Three things go wrong with help files, and all three are silent:

  1. THE TOP LEVEL MUST HAVE "children". The host's loader is one line —
     `if (helpData.children) helpMap[id] = helpData.children;` — so a file
     that parses as valid JSON but names its topics anything else is
     discarded without a word, and the viewer reports no help content as
     though the file were absent. A 2026-08 sweep of the catalog found 12
     modules in exactly that state, several carrying kilobytes nobody could
     read.

  2. LINES ARE DRAWN, NEVER WRAPPED AND NEVER TRUNCATED. drawScrollableText
     calls print(4, y, line) and print() walks the string a glyph at a time;
     pixels past x=127 are dropped by set_pixel with no error. An over-long
     line loses its tail, with no ellipsis and nothing in the log. The same
     sweep found 27 modules shipping lines that run off the screen, the
     worst by 100px.

  3. THE BUDGET IS PIXELS, NOT CHARACTERS. load_font trims every glyph to
     its own inked extent, so the atlas is fixed-pitch and the screen is
     proportional: "." advances 3px and "W" 6px. Twenty characters is a
     rule of thumb; this measures instead.

  4. ASCII ONLY, plus the handful of accented glyphs the atlas carries.
     Anything else — an em dash, a curly quote, an arrow — has no glyph and
     renders as a bare 1px gap. Our own prose is full of em dashes, which
     is exactly why this check exists.

The widths below are derived from the FONT table in schwung's
scripts/generate_font.py, which schwung_host.c names as the single source
of truth for the atlas, using the same trimming rule its own
tests/host/test_help_content_width.sh applies. Copied rather than read so
this suite does not need a schwung checkout; if the font ever changes,
re-derive them.
"""

import json
import os
import sys

SCREEN_WIDTH = 128   # scrollable_text.mjs
ORIGIN_X     = 4     # print(4, y, line)
CHAR_SPACING = 1     # load_font("font.png", 1)

WIDTHS = {" ": 5,"!": 1,"\"": 3,"#": 5,"$": 5,"%": 5,"&": 5,"'": 2,"(": 3,")": 3,"*": 5,"+": 5,",": 2,"-": 5,".": 2,"/": 5,"0": 5,"1": 3,"2": 5,"3": 5,"4": 5,"5": 5,"6": 5,"7": 5,"8": 5,"9": 5,":": 2,";": 2,"<": 4,"=": 5,">": 4,"?": 5,"@": 5,"A": 5,"B": 5,"C": 5,"D": 5,"E": 5,"F": 5,"G": 5,"H": 5,"I": 3,"J": 5,"K": 5,"L": 5,"M": 5,"N": 5,"O": 5,"P": 5,"Q": 5,"R": 5,"S": 5,"T": 5,"U": 5,"V": 5,"W": 5,"X": 5,"Y": 5,"Z": 5,"[": 3,"\\": 5,"]": 3,"^": 5,"_": 5,"`": 3,"a": 5,"b": 5,"c": 4,"d": 5,"e": 5,"f": 5,"g": 5,"h": 5,"i": 3,"j": 4,"k": 4,"l": 3,"m": 5,"n": 5,"o": 5,"p": 5,"q": 5,"r": 5,"s": 5,"t": 5,"u": 5,"v": 5,"w": 5,"x": 5,"y": 5,"z": 5,"{": 3,"|": 1,"}": 3,"~": 5,"\u00b0": 3,"\u00c4": 4,"\u00d6": 5,"\u00dc": 5,"\u00e4": 5,"\u00f6": 4,"\u00fc": 4,"\u2020": 5,"\u2021": 5,"\u20ac": 5}

def right_edge(line):
    """Rightmost inked pixel, mirroring print()/glyph() exactly."""
    x, last = ORIGIN_X, ORIGIN_X - 1
    for ch in line:
        w = WIDTHS.get(ch)
        if w is None:            # glyph() miss: a bare gap, nothing drawn
            x += CHAR_SPACING
            continue
        last = x + w - 1
        x += w + CHAR_SPACING
    return last


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "src", "help.json")
    fails = []

    with open(path) as f:
        help_data = json.load(f)

    if not isinstance(help_data.get("children"), list) or not help_data["children"]:
        fails.append('top level must have a non-empty "children" array — it is '
                     'the only key the host loader reads, and a file without '
                     'it is discarded in silence')

    lines_seen = [0]
    widest = [0, ""]

    def walk(node, trail):
        where = trail + "/" + str(node.get("title", "?"))
        kids, lines = node.get("children"), node.get("lines")
        if kids is None and lines is None:
            fails.append(where + ": node has neither children nor lines")
        if kids is not None and lines is not None:
            fails.append(where + ": node has both children and lines")
        for line in lines or []:
            lines_seen[0] += 1
            for ch in line:
                if WIDTHS.get(ch) is None:
                    fails.append("%s: %r has no glyph in the atlas (%r) — it "
                                 "renders as a bare gap" % (where, line, ch))
                    break
            edge = right_edge(line)
            if edge > widest[0]:
                widest[0], widest[1] = edge, line
            if edge >= SCREEN_WIDTH:
                fails.append("%s: %r reaches x=%d, past the %d-pixel screen — "
                             "the tail is dropped silently"
                             % (where, line, edge, SCREEN_WIDTH - 1))
        for kid in kids or []:
            walk(kid, where)

    for kid in help_data.get("children") or []:
        walk(kid, "")

    if fails:
        for f in fails:
            sys.stderr.write("FAIL: " + f + "\n")
        sys.stderr.write("help.json: %d problem(s)\n" % len(fails))
        return 1
    print("help.json: %d lines, widest reaches x=%d of %d  (%r)"
          % (lines_seen[0], widest[0], SCREEN_WIDTH - 1, widest[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
