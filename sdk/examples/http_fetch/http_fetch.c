/* http_fetch -- a small ZapOS SDK example demonstrating SYS_HTTP_REQUEST
 * (`zos_http_request()`, see sdk/zapos.h), the generic outbound
 * HTTP(S) client capability: GET/POST/any method, with custom headers
 * and a request body, exposed to a plain user-mode ZapOS app.
 *
 * READ THIS BEFORE ASSUMING OTHERWISE: ZapOS has NO bundled AI
 * integration and ships NO API key. This program makes one plain,
 * unauthenticated HTTP GET to example.com (a domain reserved by IANA
 * for documentation/examples, already used elsewhere in this kernel's
 * own test/verification passes -- see net/http.c and the Browser's
 * default homepage) and prints the status code and the first ~200
 * bytes of the response body. Nothing here talks to any AI provider or
 * other web API -- that's covered ONLY in the comment near the bottom
 * of this file, which is illustrative, NOT working/executable code
 * (there is no real API key to test it against).
 *
 * Build (the "full gcc path" -- see sdk/README.md section 2):
 *   gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
 *       -fno-builtin -nostdlib -O2 -mno-sse -mno-sse2 -mno-mmx \
 *       -mno-80387 -mgeneral-regs-only -I../.. \
 *       -c http_fetch.c -o http_fetch.o
 *   ld -m elf_i386 -T ../../../userprogs/user.ld -nostdlib \
 *       -o HTTPFETC.ELF http_fetch.o
 * Then get HTTPFETC.ELF onto zapos_disk.img (see sdk/README.md) and run
 * it from the Terminal (`run HTTPFETC.ELF`) or the File Manager. Needs
 * a NIC + working network stack (same requirement the Browser has) --
 * with no network, zos_http_request() just returns (unsigned)-1.
 */
#include "../../zapos.h"

/* Generous but bounded -- example.com's homepage is a small, static
 * page (well under a few KB), so this comfortably fits the whole
 * decoded response body in one shot. */
#define RESPONSE_CAP 4096

static char response_buf[RESPONSE_CAP];
static char content_type_buf[64];

/* Hand-written unsigned-to-decimal -- no sprintf without libc, same
 * idiom sdk/examples/aipet/aipet.c's append_uint() uses. */
static void append_uint(char *buf, int *pos, unsigned int val) {
    char digits[12];
    int n = 0;
    if (val == 0) digits[n++] = '0';
    while (val > 0) { digits[n++] = (char)('0' + val % 10); val /= 10; }
    while (n > 0) buf[(*pos)++] = digits[--n];
}

void _start(void) {
    struct zos_http_request req;
    req.host = "example.com";
    req.port = 0;              /* 0 = default -- 80, since use_tls is 0 below */
    req.use_tls = 0;           /* plain HTTP; set to 1 (and port 0 or 443) for HTTPS */
    req.method = "GET";
    req.path = "/";
    req.extra_headers = 0;     /* NULL -- no custom headers needed for a plain GET */
    req.body = 0;              /* NULL -- GET has no body */
    req.body_len = 0;
    req.response_buf = response_buf;
    req.response_cap = sizeof(response_buf) - 1; /* leave room for our own NUL below */
    req.content_type_buf = content_type_buf;
    req.content_type_cap = sizeof(content_type_buf);
    req.status_out = 0;         /* written by the kernel */
    req.response_len_out = 0;   /* written by the kernel */

    zos_write("http_fetch: requesting http://example.com/ ...\n");

    /* BLOCKS until the response arrives (or the attempt fails) --
     * exactly like zos_sleep()/zos_ipc_send()/zos_ipc_recv() already
     * block, yielding the CPU internally rather than spinning. */
    unsigned int r = zos_http_request(&req);
    if (r == (unsigned int)-1) {
        zos_write("http_fetch: request failed (DNS/TCP/TLS) -- no network reachable?\n");
        zos_exit();
    }

    char line[64];
    int pos = 0;
    const char *p1 = "http_fetch: status=";
    for (const char *p = p1; *p; p++) line[pos++] = *p;
    append_uint(line, &pos, (unsigned int)req.status_out);
    line[pos++] = '\n';
    line[pos] = 0;
    zos_write(line);

    unsigned int n = req.response_len_out;
    if (n > 200) n = 200; /* only the first ~200 bytes, per this example's scope */
    char preview[201];
    for (unsigned int i = 0; i < n; i++) preview[i] = response_buf[i];
    preview[n] = 0;

    zos_write("http_fetch: first ~200 bytes of the response body:\n");
    zos_write(preview);
    zos_write("\n");

    zos_exit();
}

/* --------------------------------------------------------------------
 * ILLUSTRATIVE ONLY, NOT EXECUTABLE CODE: how a developer would adapt
 * the exact same zos_http_request() call above into a POST against a
 * real AI provider's API. This is deliberately left as a comment, not
 * live code, because this repo has no real API key to test it against
 * and must not pretend otherwise. To actually use this, a developer
 * would:
 *
 *   1. Fill in their own host/path for whatever AI provider's API they
 *      want to call (a chat-completions-style endpoint, for example).
 *   2. Set method to "POST".
 *   3. Set use_tls to 1 (essentially every real API requires HTTPS).
 *   4. Put their OWN API key into extra_headers, e.g.:
 *
 *        static const char *headers =
 *            "Authorization: Bearer YOUR_API_KEY_HERE\r\n"
 *            "Content-Type: application/json\r\n";
 *
 *   5. Build a JSON request body themselves (there's no JSON library
 *      here either -- hand-build the string, same as append_uint()
 *      above hand-builds decimal numbers), e.g.:
 *
 *        static const char *json_body =
 *            "{\"model\":\"some-model\",\"messages\":"
 *            "[{\"role\":\"user\",\"content\":\"hello\"}]}";
 *
 *   6. Point body/body_len at it:
 *
 *        struct zos_http_request req;
 *        req.host = "api.example-ai-provider.com"; // the developer's own choice
 *        req.port = 0;
 *        req.use_tls = 1;
 *        req.method = "POST";
 *        req.path = "/v1/chat/completions";         // whatever that provider's API defines
 *        req.extra_headers = headers;
 *        req.body = json_body;
 *        req.body_len = 0; // set to strlen(json_body) for real (no libc strlen here --
 *                          // count it by hand, or track the length as you build it)
 *        req.response_buf = response_buf;
 *        req.response_cap = sizeof(response_buf) - 1;
 *        req.content_type_buf = content_type_buf;
 *        req.content_type_cap = sizeof(content_type_buf);
 *        zos_http_request(&req);
 *        // req.status_out / req.response_len_out / response_buf now hold
 *        // whatever that provider's API actually returned.
 *
 * That's the entire adaptation -- zos_http_request()'s mechanics (DNS,
 * TCP, TLS, headers, body, redirects-only-for-GET, no caching for POST)
 * are identical either way. ZapOS supplies none of the host/path/key/
 * body above; a real developer supplies all four themselves.
 * -------------------------------------------------------------------- */
