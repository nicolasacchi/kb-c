/* kbc.c — status codes and the error carrier. The whole library reports
 * through these two, so they are deliberately allocation-free: an error path
 * must never be able to fail to report itself. */

#include <stdio.h>

#include "kbc/kbc.h"

const char *kbc_status_str(kbc_status s) {
  switch (s) {
  case KBC_OK:
    return "ok";
  case KBC_ERR_INVALID:
    return "invalid";
  case KBC_ERR_NOTFOUND:
    return "not_found";
  case KBC_ERR_CONFLICT:
    return "conflict";
  case KBC_ERR_IO:
    return "io";
  case KBC_ERR_SQL:
    return "sql";
  case KBC_ERR_PARSE:
    return "parse";
  case KBC_ERR_NOMEM:
    return "nomem";
  case KBC_ERR_UNSUPPORTED:
    return "unsupported";
  case KBC_ERR_TIMEOUT:
    return "timeout";
  case KBC_ERR_CANCELED:
    return "canceled";
  case KBC_ERR_INTERNAL:
    return "internal";
  case KBC_STATUS__COUNT:
    break;
  }
  return "unknown";
}

bool kbc_failed(kbc_status s) { return s != KBC_OK; }

void kbc_err_reset(kbc_err *e) {
  if (e == NULL) {
    return;
  }
  e->status = KBC_OK;
  e->msg[0] = '\0';
}

kbc_status kbc_err_set(kbc_err *e, kbc_status s, const char *fmt, ...) {
  if (e == NULL) {
    return s;
  }
  e->status = s;

  if (fmt == NULL) {
    e->msg[0] = '\0';
    return s;
  }

  /* vsnprintf truncates and always NUL-terminates within the cap, so a 4 KB
   * expansion of a "%s" loses its tail but never overflows the 256 bytes. */
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(e->msg, KBC_ERR_MSG_MAX, fmt, ap);
  va_end(ap);

  if (n < 0) {
    /* Encoding error in the message itself: the status still travels, the
     * text is simply empty rather than garbage. */
    e->msg[0] = '\0';
  }
  return s;
}
