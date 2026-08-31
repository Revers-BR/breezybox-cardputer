/* Host stub: only the handle type and the one setter the backends call. */
#pragma once
typedef void *esp_http_client_handle_t;
static inline int esp_http_client_set_header(esp_http_client_handle_t c,
                                             const char *k, const char *v)
{ (void)c; (void)k; (void)v; return 0; }
