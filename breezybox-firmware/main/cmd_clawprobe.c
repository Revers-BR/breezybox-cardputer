/*
 * cmd_clawprobe.c - TEMPORARY Phase -1 feasibility spike for ESP-Claw.
 *
 * Answers two questions:
 *   1. Can this no-PSRAM Cardputer complete a real TLS request to an LLM API
 *      while streaming the response, and what is the internal-heap low-water
 *      mark when it does?
 *   2. Does repeating the request leak? (`clawprobe -n 3`)
 *
 * A 401 from the API is a PASS: it proves DNS + TLS handshake + cert bundle
 * verification + request write + streamed response read all work. No valid
 * key is needed to measure memory.
 *
 * DELETE THIS FILE once Phase 0 begins.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "clawprobe";

/* Which image is actually running - the whole point of the slim profile is a
 * bigger starting heap, so make it impossible to misread the numbers. */
#if defined(BREEZY_SLIM)
#define PROBE_PROFILE "cardputer-claw (SLIM)"
#else
#define PROBE_PROFILE "cardputer/adv (stock)"
#endif

/* Small fixed request body: Phase -1 measures transport cost only. */
static const char *PROBE_BODY =
    "{\"model\":\"claude-sonnet-5\",\"max_tokens\":16,\"stream\":true,"
    "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

static size_t heap_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static int network_ready(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif) {
        return 0;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return 0;
    }
    return ip_info.ip.addr != 0;
}

/* One request. Returns the HTTP status, or -1 on transport failure.
 * *low_out receives the lowest free-internal seen while streaming. */
static int probe_once(const char *url, const char *key, size_t *low_out)
{
    esp_http_client_config_t config = {
        .url                   = url,
        .method                = HTTP_METHOD_POST,
        .timeout_ms            = 30000,
        .buffer_size           = 1024,
        .buffer_size_tx        = 1024,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        printf("  FAIL - client init\n");
        return -1;
    }

    int status = -1;
    size_t low = (size_t)-1;
    int body_len = (int)strlen(PROBE_BODY);

    esp_http_client_set_header(client, "content-type", "application/json");
    esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(client, "x-api-key", key);

    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        printf("  FAIL - open: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

    if (esp_http_client_write(client, PROBE_BODY, body_len) != body_len) {
        printf("  FAIL - body write\n");
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        printf("  FAIL - fetch headers\n");
        goto cleanup;
    }

    status = esp_http_client_get_status_code(client);

    /* Stream in small reads, tracking the low-water mark. This mirrors what
     * breezy.https on_chunk will do in Phase 0. */
    char buf[256];
    int n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        size_t f = heap_free();
        if (f < low) {
            low = f;
        }
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (low_out) {
        *low_out = low;
    }
    return status;
}

int cmd_clawprobe_main(int argc, char **argv)
{
    const char *url = "https://api.anthropic.com/v1/messages";
    const char *key = "sk-ant-probe-invalid";
    int iterations = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            url = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            key = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
            if (iterations < 1) iterations = 1;
            if (iterations > 20) iterations = 20;
        } else {
            printf("usage: clawprobe [-u <url>] [-k <api-key>] [-n <iterations>]\n");
            printf("  A 401 response is a PASS: TLS and streaming both worked.\n");
            printf("  -n repeats the request to distinguish a leak from a\n");
            printf("     one-time TLS session cache.\n");
            return 0;
        }
    }

    /* Print the profile and heap first: these are useful on their own, and
     * needed to confirm which image is flashed even with no network. */
    printf("clawprobe: profile = %s\n", PROBE_PROFILE);
    printf("clawprobe: heap total=%u internal, free=%u, min_free=%u, largest=%u\n",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_free(),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    if (!network_ready()) {
        printf("clawprobe: no network. Run 'wifi connect <ssid> <pass>' first.\n");
        printf("  (profile and heap above are still valid.)\n");
        return 1;
    }

    printf("clawprobe: POST %s  x%d\n\n", url, iterations);

    size_t first_after = 0;
    int rc = 1;

    printf("  iter   before    after      low   delta   status\n");
    for (int i = 0; i < iterations; i++) {
        size_t before = heap_free();
        size_t low = 0;
        int status = probe_once(url, key, &low);
        /* Let any deferred frees settle before sampling. */
        vTaskDelay(pdMS_TO_TICKS(250));
        size_t after = heap_free();

        printf("  %4d %8u %8u %8u %7d   %d\n",
               i + 1,
               (unsigned)before,
               (unsigned)after,
               (unsigned)(low == (size_t)-1 ? 0 : low),
               (int)after - (int)before,
               status);

        if (i == 0) {
            first_after = after;
        }
        if (status > 0) {
            rc = 0;
        }
    }

    size_t final_free = heap_free();
    printf("\nclawprobe: min_free_internal now %u (gate target >= 40960)\n",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    if (iterations > 1) {
        int drift = (int)final_free - (int)first_after;
        printf("clawprobe: drift after run 1 = %d bytes across %d more runs\n",
               drift, iterations - 1);
        if (drift < -2048) {
            printf("  -> LEAK: heap keeps falling per request. Investigate before Phase 0.\n");
        } else {
            printf("  -> no leak: the run-1 drop was one-time (TLS session cache).\n");
        }
    }

    ESP_LOGD(TAG, "probe done rc=%d", rc);
    return rc;
}
