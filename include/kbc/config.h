/* config.h — kb.toml: a strict subset of TOML, loaded into a flat struct.
 *
 * Supported: comments (#), [table] headers, bare keys, quoted keys, string /
 * integer / float / boolean values, and arrays of strings. Deliberately NOT
 * supported: dotted keys, inline tables, arrays of tables, multi-line strings,
 * datetimes. An unsupported construct is a KBC_ERR_PARSE naming the line, not
 * a silent skip — a config the daemon does not fully understand is a config it
 * must not half-apply.
 */
#ifndef KBC_CONFIG_H
#define KBC_CONFIG_H

#include "kbc/kbc.h"
#include "kbc/log.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *name; /* KBC_OWN */
  char *path; /* KBC_OWN, absolute or relative to the config's dir */
  kbc_strlist ignore; /* KBC_OWN, glob-ish substrings, see watcher.c */
} kbc_corpus_cfg;

typedef struct kbc_config {
  /* Every char* and the corpora array are KBC_OWN; free with kbc_config_free. */
  char *config_path;  /* absolute path of the file loaded, or NULL */
  char *data_dir;     /* where db + index + token live */
  char *db_path;
  char *index_path;
  char *bind_addr;    /* default "127.0.0.1" */
  int port;           /* default 4317 — 4000 is the Rust daemon's, never clash */
  char *token_path;
  char *token;        /* NULL when the daemon runs without a token */
  char *embedder_cmd; /* argv[0] of the sidecar; NULL disables the vector lane */

  kbc_corpus_cfg *corpora; /* KBC_OWN */
  size_t ncorpora;

  /* Ranking. Defaults are the values the Rust daemon ships; changing them is a
   * behaviour change and belongs in the port plan, not in a local edit. */
  double bm25_k1; /* 1.2 */
  double bm25_b;  /* 0.75 */
  int rrf_k;      /* 60 */
  size_t chunk_max_bytes; /* 65536 */
  size_t search_max_hits; /* 50 */
  size_t watcher_debounce_ms; /* 250 */
  size_t http_workers;       /* 4 */
  bool json_logs;
  kbc_log_level log_level; /* KBC_LOG_INFO */
} kbc_config;


/* All defaults applied, corpora empty, token NULL. KBC_OWN. */
kbc_config *kbc_config_defaults(void);

/* Parses `path` over the defaults already in `cfg`. Missing file is fine (the
 * defaults stand); a malformed one is KBC_ERR_PARSE. Relative corpus paths
 * resolve against the config file's directory. */
kbc_status kbc_config_load_file(kbc_config *cfg, const char *path,
                                kbc_err *err);

/* Rejects: unknown keys, out-of-range ports, empty/duplicated corpus names,
 * a corpus path that is not a directory, a bind_addr that is not an IP
 * literal, and a non-loopback bind with no token. */
kbc_status kbc_config_validate(const kbc_config *cfg, kbc_err *err);

/* The token rule, exposed because the httpd enforces it at bind time and the
 * CLI prints the same refusal message. */
bool kbc_config_bind_is_safe(const kbc_config *cfg, char *why, size_t why_cap);

const kbc_corpus_cfg *kbc_config_corpus(const kbc_config *cfg,
                                        const char *name);

void kbc_config_free(kbc_config *cfg);

/* Serializes back to TOML (comments dropped) — used by `kbc config show` and
 * by the round-trip test. */
kbc_status kbc_config_dump(const kbc_config *cfg, kbc_str *out, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_CONFIG_H */
