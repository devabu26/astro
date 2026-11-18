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

char *astro_matrix_get_rooms_json(
    AstroMatrixClient *handle,
    char **error_out
);

char *astro_matrix_get_room_timeline_json(
    AstroMatrixClient *handle,
    const char *room_id,
    char **error_out
);

char *astro_matrix_paginate_room_timeline_json(
    AstroMatrixClient *handle,
    const char *room_id,
    char **error_out
);

char *astro_matrix_get_cached_rooms_json(
    AstroMatrixClient *handle,
    char **error_out
);

int32_t astro_matrix_logout(
    AstroMatrixClient *handle,
    char **error_out
);

/*
 * Downloads a thumbnail for a plain MXC media URI.
 *
 * Returns:
 *   1  Success; data_out/len_out contain the allocated bytes.
 *   0  Success, but the media response contained no bytes.
 *  -1  Error; error_out receives the message.
 *
 * The returned byte buffer must be released with astro_media_bytes_free().
 */
int32_t astro_matrix_thumbnail(
    AstroMatrixClient *handle,
    const char *mxc_uri,
    uint32_t width,
    uint32_t height,
    uint8_t **data_out,
    uintptr_t *len_out,
    char **error_out
);

void astro_media_bytes_free(
    uint8_t *data,
    uintptr_t len
);

void astro_matrix_close(AstroMatrixClient *handle);

void astro_string_free(char *value);

#ifdef __cplusplus
}
#endif
