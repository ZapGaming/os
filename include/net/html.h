#ifndef NET_HTML_H
#define NET_HTML_H

#include <stdint.h>

#define HTML_MAX_LINES    240
#define HTML_MAX_LINE_LEN 100
#define HTML_MAX_TITLE    128

enum html_style {
    HTML_STYLE_NORMAL,
    HTML_STYLE_HEADING,
    HTML_STYLE_BOLD,
    HTML_STYLE_LINK,
    HTML_STYLE_LISTITEM,
};

struct html_line {
    char text[HTML_MAX_LINE_LEN];
    enum html_style style;
};

struct html_doc {
    char title[HTML_MAX_TITLE];
    struct html_line lines[HTML_MAX_LINES];
    int line_count;
};

/* A "reader mode" HTML interpreter, not a layout engine: no CSS, no
 * boxes/floats/positioning. It walks the tag stream, recognizes a
 * pragmatic subset (headings, paragraphs, bold/strong, links, lists,
 * line breaks), skips <script>/<style> content entirely, decodes the
 * common entities, and produces a flat list of styled text lines ready
 * to draw top-to-bottom. */
void html_parse(const char *html, uint32_t len, struct html_doc *out);

#endif
