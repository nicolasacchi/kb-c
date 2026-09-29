/* html.h — the HTML sanitiser.
 *
 * WHY THIS EXISTS, and it is not a stage-5 nicety. kb-c's indexer accepts
 * `.html` and `.htm` as indexable (`is_indexable`, app.c:273), so an HTML file
 * that arrives in a corpus by `git checkout`, `scp`, `kb add <dir>` or a sync is
 * indexed and served. Before this module existed, the capture route refused
 * HTML with a 415 — and that refusal protected nothing, because capture was
 * never the way HTML got in. The corpus was.
 *
 * The containment that does exist today is a CSP header on the parent origin,
 * and it is two things at once: not enough, and invisible. Not enough because
 * the artifact SUBDOMAIN sends only `frame-ancestors` and no `sandbox` (ADR-009),
 * so a script in a document served there EXECUTES. Invisible because a header
 * is a property of one route, and this has to be a property of the bytes.
 *
 * No dependency. The operator's rule, narrowed in ADR-008, binds the C library:
 * libkbc.a links sqlite3, libm and libpthread and nothing else. The markdown
 * renderer is hand-rolled for the same reason, and the cost of that decision
 * is the size of this file.
 *
 * WHAT IT COSTS, stated honestly. The ALLOWLIST below is about 200 lines. A
 * faithful tree, which is what byte-equality with the original would need, is
 * not: ammonia is html5ever's tokenizer, then HTML5 tree construction — the
 * insertion-mode state machine, the adoption agency algorithm, foster
 * parenting — then a per-element filter, then a serializer. Reproducing only
 * the filter produces a DIFFERENT TREE on any mis-nested p/table/form, because
 * unwrapping a disallowed element keeps its children where the PARSER put them
 * and a stream filter has no parser to tell it where that was. That is benign
 * for security and fatal for a byte-diff gate, which is why the gate question
 * is called out at the bottom of this header rather than left implicit.
 */
#ifndef KBC_HTML_H
#define KBC_HTML_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sanitises `html` against the allowlist, appending the result to `out`.
 *
 * The allowlist is ammonia 4.1.4's, as kb configures it (kb-core/src/capture.rs:687-699
 * — `default().add_generic_attributes(["id"]).add_clean_content_tags(["title"])`,
 * and kb never calls `add_tags`).
 *
 * TAGS, 75 (ammonia lib.rs:378-388):
 *   a abbr acronym area article aside b bdi bdo blockquote br caption center
 *   cite code col colgroup data dd del details dfn div dl dt em figcaption
 *   figure footer h1..h6 header hgroup hr i img ins kbd li map mark nav ol p
 *   pre q rp rt rtc ruby s samp small span strike strong sub summary sup
 *   table tbody td th thead time tr tt u ul var wbr
 *
 * DELIBERATELY ABSENT, and each absence is a security decision: html, head,
 * body, title, meta, script, style, iframe, object, embed, form, input, svg,
 * math, link, base, noscript, template, audio, video, source, track, canvas,
 * applet, frame, frameset, marquee.
 *
 * PER-TAG ATTRIBUTES (ammonia lib.rs:390-435), reduced to the EFFECTIVE set.
 * `tfoot` has an attributes row in ammonia but is NOT in the tag set, so it is
 * always unwrapped and its configuration is dead — porting it would be porting
 * a lie. 16 effective rows:
 *   a[href, hreflang] · bdo[dir] · blockquote[cite]
 *   col[align,char,charoff,span] · colgroup[align,char,charoff,span]
 *   del,ins[cite,datetime] · hr[align,size,width]
 *   img[align,alt,height,src,width] · ol[start] · q[cite]
 *   table[align,char,charoff,summary]
 *   tbody,thead[align,char,charoff] · td[align,char,charoff,colspan,headers,rowspan]
 *   th[align,char,charoff,colspan,headers,rowspan,scope] · tr[align,char,charoff]
 * Note `span` is on col/colgroup ONLY; an earlier draft of this header put it on
 * tbody/thead as well, and the implementer caught it against lib.rs:382-435.
 *
 * GENERIC, on ANY allowed tag (ammonia :389, capture.rs:690): `lang`, `title`,
 * `id`. NOTE `title` is BOTH a generic attribute and a CLEAN-CONTENT TAG; the
 * tag wins, so a document's `<title>` text is dropped rather than left as
 * orphaned prose (capture.rs:660-670 explains why).
 *
 * CLASS AND STYLE ARE NEVER ALLOWED. `allowed_classes` is empty (:462) and
 * `style_properties` is None (:482), so `class` and `style` are stripped from
 * every element. That is stricter than it looks and it is deliberate.
 *
 * URL SCHEMES, 25 (ammonia :437-461):
 *   bitcoin ftp ftps geo http https im irc ircs magnet mailto mms mx news
 *   nntp openpgp4fpr sip sms smsto ssh tel url webcal wtai xmpp
 * A relative URL passes through unchanged (ammonia :477, url_relative = PassThrough).
 *
 * COMMENTS ARE STRIPPED (:480). LINK RELATIONSHIPS on a surviving `a` gain
 * `rel="noopener noreferrer"` (:479).
 *
 * On a malformed or unterminated construct, the safest reading wins: a tag that
 * cannot be parsed is unwrapped and its text kept, and a URL whose scheme
 * cannot be read is DROPPED along with its attribute rather than emitted and
 * hoped for. A sanitiser that passes a construct it could not parse is not a
 * sanitiser.
 *
 * THE GATE QUESTION, and it is not a detail: byte-equality with ammonia's
 * output is out of reach without reproducing the parser, so the harness that
 * checks this work needs an AGREED-DIVERGENCE ALLOWLIST, or it reports correct
 * behaviour as a failure and gets ignored. That has already happened twice in
 * this port.
 *
 * A SANITISER THAT IS SAFE IS NOT THE SAME AS ONE THAT IS BYTE-IDENTICAL, and
 * every item below is safe-but-different. Measured on 2026-09-29 against real
 * ammonia 4.1.4, 78 sampled documents: 71 byte-identical, 7 different, and all
 * 7 are the three classes below.
 *
 *   1. NON-ASCII NAMED CHARACTER REFERENCES pass through verbatim. Only the
 *      57 references that decode to an ASCII codepoint are in the table; the
 *      other ~2174 come back as their six literal bytes where ammonia emits
 *      UTF-8. `&eacute;` renders identically. A reference that decodes to a
 *      non-ASCII codepoint cannot open a tag, end an attribute value, or
 *      contribute a byte to a URL scheme, so it cannot change a decision the
 *      filter makes. THE TABLE IS NOT APPROXIMATED: `&fjlig;`, `&underbar;`,
 *      `&nvgt;` and `&nvlt;` were briefly mapped to nearby ASCII characters,
 *      which is neither faithful nor safe — `&nvlt;` rendered as a bare `<`
 *      the operator never wrote — and they are now the ordinary pass-through
 *      case. The four longest names are the ASCII ones.
 *   2. THE ADOPTION AGENCY IS SIMPLIFIED. A formatting element at the top of
 *      the open stack is popped rather than run through the full algorithm, so
 *      mis-nested formatting (`<p><b>bold<div>x</div></b></p>`) produces
 *      different NESTING. The allowlist decision is identical either way; only
 *      the shape of the tree differs.
 *   3. SVG AND MathML ARE PARSED WITH HTML RULES. There is no foreign-content
 *      namespace, so `<math><mi>x</mi></math>` keeps its text where ammonia
 *      drops it. Text is kept, markup is not, and neither can carry a script:
 *      both elements are unwrapped by the filter.
 */
kbc_status kbc_html_sanitize(const char *html, size_t len, kbc_str *out,
                            kbc_err *err);

/* Splits an HTML document into `<head>` content and body, synthesising a
 * `<head>` when the source has none. This is the port of
 * `stamp_html_head` (kb-core/src/capture.rs:645-656), which splices the
 * capture's `<meta>` keys into the head — the same seven keys, in the same
 * order, that the Markdown path writes into front matter.
 *
 * THE ORDER MATTERS and it is counter-intuitive: the reference SANITISES FIRST
 * and stamps second (capture.rs:220-225, then :235). Stamping first would put
 * the provenance `<meta>` into the document and then hand it to ammonia, which
 * strips `meta` — the capture would be stored with its own provenance removed.
 * If you ever see this reversed, that is the bug and not the other way round. */
kbc_status kbc_html_split_head(const char *html, size_t len, kbc_str *head,
                               kbc_str *body, kbc_err *err);

/* ---------------------------------------------------------------------------
 * THE SERVE-LAYER OBLIGATIONS. kb-c had none of these; the original has them at
 * kb-core/src/scrub.rs and kb-server/src/scrub.rs. They are here because a
 * sanitiser is a property of the bytes, and these are a property of what goes
 * out on the wire.
 * ------------------------------------------------------------------------- */

/* Removes the `kb-prompt` template from an outbound document. The prompt
 * template is the daemon's own construction, never the operator's content, and
 * it must not reach a client: it is not a document. */
kbc_status kbc_html_strip_kb_prompt(const char *html, size_t len, kbc_str *out,
                                    kbc_err *err);

/* Whether the request came from outside loopback, which is what decides
 * whether redactions apply at all. Walks the `X-Forwarded-For` chain only as
 * far as `trusted_proxies` allows, and returns TRUE — remote, redact — when
 * the peer address is unknown or empty. It FAILS CLOSED, because the
 * alternative is skipping redaction for a request whose origin nobody could
 * establish. Read the boolean as "redact?", not as "is it a public address?":
 * a trusted proxy with no XFF, which is the operator's own curl, answers
 * FALSE.
 *
 * SIGNATURE, corrected 2026-09-29. An earlier draft of this declaration took
 * `(const kbc_request *, const kbc_config *)`, which does not compile — the
 * header does not include httpd.h or config.h, so `kbc_request` and
 * `kbc_config` are undeclared at this point. It was also not implementable
 * from them: `kbc_request` carries `client_addr` but no `X-Forwarded-For`
 * value and no general header accessor, and `kbc_config` has no trusted-proxy
 * list at all, so the XFF walk this function is REQUIRED to perform has
 * nothing to walk. The three arguments below are what the implementation
 * actually takes, and they are the ones httpd.c has. */
bool kbc_html_looks_non_loopback(const char *peer_addr, const char *xff,
                                 const char *const *trusted_proxies,
                                 size_t n_trusted_proxies);

/* The full outbound pass: strip the prompt template, then redact when the
 * request is non-loopback. Run this on every document leaving the daemon. */
kbc_status kbc_html_scrub_outbound(const char *peer_addr, const char *xff,
                                   const char *const *trusted_proxies,
                                   size_t n_trusted_proxies, const char *html,
                                   size_t len, kbc_str *out, kbc_err *err);


#ifdef __cplusplus
}
#endif

#endif /* KBC_HTML_H */
