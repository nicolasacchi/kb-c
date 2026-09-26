/* log.h — leveled logging, thread-safe, human or JSON lines.
 *
 * The only global mutable state in libkbc. One mutex inside, initialized
 * lazily, so logging is legal from any thread at any time including during
 * thread teardown. Default level is KBC_LOG_INFO on stderr.
 */
#ifndef KBC_LOG_H
#define KBC_LOG_H

#include "kbc/kbc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  KBC_LOG_ERROR = 0,
  KBC_LOG_WARN = 1,
  KBC_LOG_INFO = 2,
  KBC_LOG_DEBUG = 3
} kbc_log_level;

const char *kbc_log_level_str(kbc_log_level lvl);
/* Accepts error|warn|info|debug, case-insensitive. Returns false otherwise. */
bool kbc_log_level_parse(const char *s, kbc_log_level *out);

void kbc_log_init(kbc_log_level level, bool json_lines);
void kbc_log_set_level(kbc_log_level level);
kbc_log_level kbc_log_get_level(void);

void kbc_log(kbc_log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define KBC_LOGE(...) kbc_log(KBC_LOG_ERROR, __VA_ARGS__)
#define KBC_LOGW(...) kbc_log(KBC_LOG_WARN, __VA_ARGS__)
#define KBC_LOGI(...) kbc_log(KBC_LOG_INFO, __VA_ARGS__)
#define KBC_LOGD(...) kbc_log(KBC_LOG_DEBUG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* KBC_LOG_H */
