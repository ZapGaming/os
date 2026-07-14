#include <net/layout.h>

/*
 * The legacy compositor owned browser image fetching and decoding, so the
 * layout engine called back into it through layout_get_image(). Nova replaces
 * that compositor but the browser/layout objects are still linked into the
 * kernel. Keep the ABI alive here until image loading is extracted into a
 * standalone browser service.
 *
 * Returning 0 is the layout API's documented "not loaded / unsupported"
 * result. net/layout.c will render the element using its CSS or fallback size
 * without dereferencing an invalid pixel buffer.
 */
int layout_get_image(const struct dom_node *node, int *out_w, int *out_h,
                     const uint32_t **out_pixels) {
    (void)node;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_pixels) *out_pixels = 0;
    return 0;
}
