#include <gui/browser.h>
#include <kernel/kheap.h>
#include <net/http.h>
#include <string.h>

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    if (!src) src = "";
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int parse_url(const char *url, int *tls, char *host, int host_cap,
                     uint16_t *port, char *path, int path_cap) {
    const char *p = url;
    *tls = 0;
    *port = 80;
    if (strncmp(p, "https://", 8) == 0) { *tls = 1; *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }

    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < host_cap - 1) host[hi++] = *p++;
    host[hi] = 0;
    if (hi == 0) return 0;

    if (*p == ':') {
        p++;
        uint32_t value = 0;
        while (*p >= '0' && *p <= '9') { value = value * 10 + (uint32_t)(*p - '0'); p++; }
        if (value == 0 || value > 65535) return 0;
        *port = (uint16_t)value;
    }

    if (*p == 0) copy_text(path, path_cap, "/");
    else copy_text(path, path_cap, p);
    return 1;
}

static void clear_document(struct nova_browser *b) {
    if (b->dom) { dom_free(b->dom); b->dom = 0; }
    css_stylesheet_free(&b->css);
    css_stylesheet_init(&b->css);
    b->layout.item_count = 0;
    b->layout.link_count = 0;
    b->layout.content_height = 0;
}

void nova_browser_init(struct nova_browser *b) {
    memset(b, 0, sizeof(*b));
    b->body = (char *)kmalloc(NOVA_BROWSER_BODY + 1);
    css_stylesheet_init(&b->css);
    layout_doc_alloc(&b->layout);
    copy_text(b->title, sizeof(b->title), "Nova Browser");
}

void nova_browser_destroy(struct nova_browser *b) {
    clear_document(b);
    layout_doc_free(&b->layout);
    if (b->body) kfree(b->body);
    b->body = 0;
}

void nova_browser_relayout(struct nova_browser *b, int viewport_width) {
    if (!b->dom) return;
    if (viewport_width < 160) viewport_width = 160;
    layout_run(b->dom, &b->css, viewport_width, &b->layout);
    if (b->scroll_y > b->layout.content_height) b->scroll_y = b->layout.content_height;
}

int nova_browser_navigate(struct nova_browser *b, const char *url, int viewport_width) {
    if (!b || !b->body || !url) return 0;
    int tls;
    uint16_t port;
    char host[128], path[192];
    if (!parse_url(url, &tls, host, sizeof(host), &port, path, sizeof(path))) {
        b->error = 1;
        b->status = 0;
        copy_text(b->title, sizeof(b->title), "Invalid URL");
        return 0;
    }

    b->loading = 1;
    b->error = 0;
    b->scroll_y = 0;
    copy_text(b->url, sizeof(b->url), url);
    clear_document(b);

    uint32_t len = 0;
    int status = 0;
    int ok = http_get(tls, host, port, path, &status, b->body, NOVA_BROWSER_BODY,
                      &len, b->content_type, sizeof(b->content_type));
    b->loading = 0;
    b->status = status;
    if (!ok) {
        b->error = 1;
        b->body_len = 0;
        copy_text(b->title, sizeof(b->title), "Network error");
        return 0;
    }

    b->body_len = len;
    b->body[len] = 0;
    css_extract_style_blocks(&b->css, b->body, len);
    css_resolve_custom_properties(&b->css);
    b->dom = dom_parse(b->body, len, b->title, sizeof(b->title));
    if (!b->dom) {
        b->error = 1;
        copy_text(b->title, sizeof(b->title), "Document parse error");
        return 0;
    }
    nova_browser_relayout(b, viewport_width);
    return 1;
}
