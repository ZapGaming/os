#ifndef GUI_BROWSER_H
#define GUI_BROWSER_H

#include <stdint.h>
#include <net/dom.h>
#include <net/css.h>
#include <net/layout.h>

#define NOVA_BROWSER_URL 256
#define NOVA_BROWSER_BODY (96u * 1024u)

struct nova_browser {
    char url[NOVA_BROWSER_URL];
    char title[DOM_MAX_TITLE];
    char content_type[64];
    int status;
    int loading;
    int error;
    int scroll_y;
    char *body;
    uint32_t body_len;
    struct dom_node *dom;
    struct css_stylesheet css;
    struct layout_doc layout;
};

void nova_browser_init(struct nova_browser *browser);
void nova_browser_destroy(struct nova_browser *browser);
int nova_browser_navigate(struct nova_browser *browser, const char *url, int viewport_width);
void nova_browser_relayout(struct nova_browser *browser, int viewport_width);

#endif
