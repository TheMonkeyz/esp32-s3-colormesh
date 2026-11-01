// Per-device TLS certificate for the settings page.
// The first boot generates an EC P-256 key and a self-signed certificate and keeps both in NVS
// (namespace "tls"). Every board gets its own key, so nothing secret ships in the firmware or the repo.
// Erasing the flash (e.g. "Erase device" in the web flasher) makes the next boot generate a new pair.
#include "tlscert.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

static const char *TAG = "tlscert";
#define NS        "tls"
#define CERT_MAX  1200
#define KEY_MAX   400

static char *cert_pem, *key_pem;

static bool load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t cl = 0, kl = 0;
    bool ok = nvs_get_str(h, "cert", NULL, &cl) == ESP_OK && nvs_get_str(h, "key", NULL, &kl) == ESP_OK &&
              cl > 1 && kl > 1;
    if (ok) {
        cert_pem = malloc(cl);
        key_pem = malloc(kl);
        ok = cert_pem && key_pem && nvs_get_str(h, "cert", cert_pem, &cl) == ESP_OK &&
             nvs_get_str(h, "key", key_pem, &kl) == ESP_OK;
    }
    nvs_close(h);
    if (ok) {                                   // make sure the pair is usable
        mbedtls_x509_crt c;
        mbedtls_x509_crt_init(&c);
        ok = mbedtls_x509_crt_parse(&c, (const unsigned char *)cert_pem, strlen(cert_pem) + 1) == 0;
        mbedtls_x509_crt_free(&c);
    }
    if (!ok) { free(cert_pem); free(key_pem); cert_pem = key_pem = NULL; }
    return ok;
}

static int generate(void)
{
    int ret;
    mbedtls_pk_context key;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509write_crt_init(&crt);
    char *cbuf = calloc(1, CERT_MAX), *kbuf = calloc(1, KEY_MAX);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char subject[96];
    snprintf(subject, sizeof(subject), "CN=%s %02X%02X%02X,O=%s", CONFIG_FORGE_TLS_NAME, mac[3], mac[4], mac[5],
             CONFIG_FORGE_PRODUCT[0] ? CONFIG_FORGE_PRODUCT : esp_app_get_description()->project_name);
    unsigned char serial[16];

    const char *pers = "forge-tls";
    if (!cbuf || !kbuf) { ret = -1; goto out; }
    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)pers, strlen(pers))))
        goto out;
    if ((ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)))) goto out;
    if ((ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg)))
        goto out;
    if ((ret = mbedtls_ctr_drbg_random(&drbg, serial, sizeof(serial)))) goto out;
    serial[0] &= 0x7F;                           // positive
    serial[0] |= 0x40;                           // and 16 bytes long

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, subject))) goto out;
    if ((ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject))) goto out;
    if ((ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)))) goto out;
    // Fixed validity: the board may not know the time yet, and there's no CA to renew with
    if ((ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20991231235959"))) goto out;
    if ((ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1))) goto out;
    if ((ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cbuf, CERT_MAX, mbedtls_ctr_drbg_random, &drbg)))
        goto out;
    if ((ret = mbedtls_pk_write_key_pem(&key, (unsigned char *)kbuf, KEY_MAX))) goto out;

    nvs_handle_t h;
    if ((ret = nvs_open(NS, NVS_READWRITE, &h)) == ESP_OK) {
        ret = nvs_set_str(h, "cert", cbuf);
        if (!ret) ret = nvs_set_str(h, "key", kbuf);
        if (!ret) ret = nvs_commit(h);
        nvs_close(h);
    }
    if (!ret) { cert_pem = cbuf; key_pem = kbuf; cbuf = kbuf = NULL; }
out:
    if (ret) ESP_LOGE(TAG, "certificate generation failed: -0x%04x", (unsigned)-ret);
    if (kbuf) memset(kbuf, 0, KEY_MAX);
    free(cbuf);
    free(kbuf);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ret;
}

// Key generation needs more stack than the calling tasks have spare, and NVS writes need an
// internal-RAM stack: run it once in a short-lived task.
static SemaphoreHandle_t done;
static void gen_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    if (generate() == 0)
        ESP_LOGI(TAG, "new device certificate generated in %lld ms", (esp_timer_get_time() - t0) / 1000);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

bool tlscert_get(const char **cert, size_t *cert_len, const char **key, size_t *key_len)
{
    if (!cert_pem && !load()) {
        ESP_LOGI(TAG, "no device certificate yet, generating one");
        done = xSemaphoreCreateBinary();
        xTaskCreate(gen_task, "tlsgen", 8192, NULL, 5, NULL);
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(done);
    }
    if (!cert_pem) return false;
    *cert = cert_pem;
    *cert_len = strlen(cert_pem) + 1;          // mbedtls wants the NUL counted for PEM
    *key = key_pem;
    *key_len = strlen(key_pem) + 1;
    return true;
}

void tlscert_log_fingerprint(void)
{
    if (!cert_pem) return;
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    if (mbedtls_x509_crt_parse(&c, (const unsigned char *)cert_pem, strlen(cert_pem) + 1) == 0) {
        char subj[96];
        mbedtls_x509_dn_gets(subj, sizeof(subj), &c.subject);
        char sn[64];
        mbedtls_x509_serial_gets(sn, sizeof(sn), &c.serial);
        ESP_LOGI(TAG, "certificate: %s, serial %.23s...", subj, sn);
    }
    mbedtls_x509_crt_free(&c);
}
