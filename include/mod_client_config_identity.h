#pragma once

#include <esp_system.h>
#include <stdio.h>
#include <string.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>
#include <nvs.h>

namespace client_config {

struct Identity {
    char certificate[768];
    char privateKey[384];
    char adminToken[65];
    char clientToken[65];
};

static Identity identity{};

static int fillRandom(void*, unsigned char* buffer, size_t size) {
    esp_fill_random(buffer, size);
    return 0;
}

static void generateSecret(char (&token)[65]) {
    uint8_t bytes[32];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        snprintf(token + index * 2, 3, "%02x", static_cast<unsigned>(bytes[index]));
    }
}

static bool generateIdentity() {
    mbedtls_pk_context key;
    mbedtls_mpi serial;
    mbedtls_x509write_cert certificate;
    mbedtls_pk_init(&key);
    mbedtls_mpi_init(&serial);
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);

    uint8_t serialBytes[16];
    esp_fill_random(serialBytes, sizeof(serialBytes));
    serialBytes[0] = (serialBytes[0] & 0x7f) | 1;
    // DER GeneralNames containing fsd.local and the firmware's AP address.
    const uint8_t names[] = {0x30, 17, 0x82, 9, 'f', 's', 'd', '.', 'l', 'o', 'c', 'a', 'l',
                             0x87, 4, 9, 9, 9, 9};
    const bool ok =
        mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), fillRandom, nullptr) == 0 &&
        mbedtls_mpi_read_binary(&serial, serialBytes, sizeof(serialBytes)) == 0 &&
        mbedtls_x509write_crt_set_serial(&certificate, &serial) == 0 &&
        mbedtls_x509write_crt_set_subject_name(&certificate, "CN=fsd.local") == 0 &&
        mbedtls_x509write_crt_set_issuer_name(&certificate, "CN=fsd.local") == 0 &&
        mbedtls_x509write_crt_set_validity(&certificate, "20240101000000", "20491231235959") == 0 &&
        mbedtls_x509write_crt_set_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0 &&
        mbedtls_x509write_crt_set_extension(&certificate, MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, names, sizeof(names)) == 0 &&
        mbedtls_x509write_crt_pem(&certificate, reinterpret_cast<uint8_t*>(identity.certificate),
            sizeof(identity.certificate), fillRandom, nullptr) == 0 &&
        mbedtls_pk_write_key_pem(&key, reinterpret_cast<uint8_t*>(identity.privateKey),
            sizeof(identity.privateKey)) == 0;

    mbedtls_x509write_crt_free(&certificate);
    mbedtls_mpi_free(&serial);
    mbedtls_pk_free(&key);
    if (ok) {
        generateSecret(identity.adminToken);
        generateSecret(identity.clientToken);
    }
    return ok;
}

static bool loadIdentity(nvs_handle_t storage) {
    size_t length = sizeof(identity);
    const esp_err_t error = nvs_get_blob(storage, "identity", &identity, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return generateIdentity() &&
            nvs_set_blob(storage, "identity", &identity, sizeof(identity)) == ESP_OK &&
            nvs_commit(storage) == ESP_OK;
    }
    return error == ESP_OK && length == sizeof(identity) &&
        memchr(identity.certificate, '\0', sizeof(identity.certificate)) && identity.certificate[0] &&
        memchr(identity.privateKey, '\0', sizeof(identity.privateKey)) && identity.privateKey[0] &&
        identity.adminToken[64] == '\0' && identity.clientToken[64] == '\0' &&
        strlen(identity.adminToken) == 64 && strlen(identity.clientToken) == 64;
}

}  // namespace client_config
