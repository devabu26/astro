#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstroStorageManager AstroStorageManager;
typedef struct AstroMatrixClient AstroMatrixClient;

const char *astro_get_status(void);

AstroStorageManager *astro_storage_open(
    const char *path,
    const char *passphrase,
    char **error_out
);

void astro_storage_close(AstroStorageManager *handle);

AstroMatrixClient *astro_matrix_login(
    const char *path,
    const char *passphrase,
    const char *homeserver,
    const char *username,
    const char *password,
    char **error_out
);

/*
 * Returns:
 *   1  Session restored; client_out receives the client.
 *   0  No saved session.
 *  -1  Error; error_out receives the message.
 */
int32_t astro_matrix_restore(
    const char *path,
    const char *passphrase,
    AstroMatrixClient **client_out,
    char **error_out
);

int32_t astro_matrix_logout(
    AstroMatrixClient *handle,
    char **error_out
);

void astro_matrix_close(AstroMatrixClient *handle);

void astro_string_free(char *value);

#ifdef __cplusplus
}
#endif
