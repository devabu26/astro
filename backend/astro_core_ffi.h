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

int32_t astro_matrix_device_management_url(
    AstroMatrixClient *handle,
    const char *device_id,
    char **url_out,
    char **error_out
);

int32_t astro_matrix_register_push_token(
    AstroMatrixClient *handle,
    const char *push_token,
    const char *privacy_level,
    char **error_out
);

int32_t astro_matrix_unregister_push_token(
    AstroMatrixClient *handle,
    const char *push_token,
    char **error_out
);

char *astro_matrix_send_room_message(
    AstroMatrixClient *handle,
    const char *room_id,
    const char *message,
    char **error_out
);

/* Matrix reaction toggle: 1 added, 0 removed, -1 error. */
int32_t astro_matrix_toggle_room_reaction(
    AstroMatrixClient *handle,
    const char *room_id,
    const char *event_id,
    const char *reaction_key,
    char **error_out
);

/* Matrix device verification actions: 1 success, -1 error. */
int32_t astro_matrix_accept_verification_request(
    AstroMatrixClient *handle,
    const char *user_id,
    const char *flow_id,
    char **error_out
);

int32_t astro_matrix_cancel_verification_request(
    AstroMatrixClient *handle,
    const char *user_id,
    const char *flow_id,
    char **error_out
);

int32_t astro_matrix_confirm_verification(
    AstroMatrixClient *handle,
    const char *user_id,
    const char *flow_id,
    char **error_out
);

/* Change the display name of a registered Matrix device. */
int32_t astro_matrix_rename_device(
    AstroMatrixClient *handle,
    const char *device_id,
    const char *display_name,
    char **error_out
);

/* Sign out another registered Matrix device using account password UIAA. */
int32_t astro_matrix_revoke_device(
    AstroMatrixClient *handle,
    const char *device_id,
    const char *password,
    char **error_out
);

/* Start SAS verification with a registered Matrix device. */
int32_t astro_matrix_request_device_verification(
    AstroMatrixClient *handle,
    const char *device_id,
    char **error_out
);

int32_t astro_matrix_mismatch_verification(
    AstroMatrixClient *handle,
    const char *user_id,
    const char *flow_id,
    char **error_out
);

char *astro_matrix_get_devices_json(
    AstroMatrixClient *handle,
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

char *astro_matrix_refresh_room_timeline_json(
    AstroMatrixClient *handle,
    const char *room_id,
    char **error_out
);

int32_t astro_matrix_mark_room_as_read(
    AstroMatrixClient *handle,
    const char *room_id,
    char **error_out
);

char *astro_matrix_paginate_room_timeline_json(
    AstroMatrixClient *handle,
    const char *room_id,
    char **error_out
);

char *astro_matrix_poll_room_timeline_updates_json(
    AstroMatrixClient *handle,
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

/*
 * Downloads an image from a serialised Matrix MediaSource.
 * Supports both plain and encrypted media sources.
 *
 * Returns:
 *   1  Success; data_out/len_out contain the allocated bytes.
 *   0  Success, but the media response contained no bytes.
 *  -1  Error; error_out receives the message.
 *
 * The returned byte buffer must be released with astro_media_bytes_free().
 */
/*
 * Downloads a thumbnail from a serialised Matrix MediaSource.
 * Supports both plain and encrypted media sources.
 *
 * The returned byte buffer must be released with astro_media_bytes_free().
 */
int32_t astro_matrix_media_thumbnail(
    AstroMatrixClient *handle,
    const char *media_source_json,
    uint32_t width,
    uint32_t height,
    uint8_t **data_out,
    uintptr_t *len_out,
    char **error_out
);

int32_t astro_matrix_image(
    AstroMatrixClient *handle,
    const char *media_source_json,
    uint8_t **data_out,
    uintptr_t *len_out,
    char **error_out
);

/*
 * Downloads a video from a serialised Matrix MediaSource.
 * Supports both plain and encrypted media sources.
 *
 * Returns:
 *   1  Success; data_out/len_out contain the allocated bytes.
 *   0  Success, but the media response contained no bytes.
 *  -1  Error; error_out receives the message.
 *
 * The returned byte buffer must be released with astro_media_bytes_free().
 */
int32_t astro_matrix_video(
    AstroMatrixClient *handle,
    const char *media_source_json,
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