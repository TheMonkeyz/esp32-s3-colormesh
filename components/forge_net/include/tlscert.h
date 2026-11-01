#pragma once
#include <stdbool.h>
#include <stddef.h>

// Returns this device's TLS certificate and private key (PEM, lengths include the NUL).
// Generates and saves them in NVS on first use (about a second). Needs NVS initialised.
bool tlscert_get(const char **cert, size_t *cert_len, const char **key, size_t *key_len);
void tlscert_log_fingerprint(void);
