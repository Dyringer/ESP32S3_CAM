/**
 ******************************************************************************
 * @file    files_page.c
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   File manager page for the storage partition.
 *
 *          list, delete and mkdir are quick metadata operations and run in
 *          the httpd handler. Upload and download are handed to the shared
 *          http_panel_transfer worker so the httpd task never blocks on
 *          file I/O or the network.
 *
 *          No auth/CSRF — same LAN-only trust model as the rest of http_panel.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "files_page.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_panel_server.h"
#include "http_panel_transfer.h"
#include "storage.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define FILES_PAGE_QUERY_MAX        400u
#define FILES_PAGE_NAME_MAX         64u
#define FILES_PAGE_LIST_MAX         128u
#define FILES_PAGE_ENTRY_PATH_MAX   (STORAGE_PATH_MAX + FILES_PAGE_NAME_MAX + 1u)
#define FILES_PAGE_ERROR_BODY_MAX   96u

/* LittleFS needs spare blocks for metadata and copy-on-write on top of the data. */
#define FILES_PAGE_FREE_MARGIN      (2u * 4096u)

#define FILES_PAGE_400              "400 Bad Request"
#define FILES_PAGE_404              "404 Not Found"
#define FILES_PAGE_409              "409 Conflict"
#define FILES_PAGE_500              "500 Internal Server Error"
#define FILES_PAGE_503              "503 Service Unavailable"

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct files_page_entry_s
{
    char name[FILES_PAGE_NAME_MAX];
    uint32_t size;
    bool is_dir;
} files_page_entry_t;

/* text holds the target path until the file is open; a download then reuses it
 * for the Content-Disposition value, which httpd keeps by pointer until the
 * response is sent. */
typedef struct files_page_transfer_s
{
    FILE* file;
    size_t content_length;
    char text[STORAGE_PATH_MAX];
} files_page_transfer_t;

_Static_assert(
    sizeof(files_page_transfer_t) <= HTTP_PANEL_TRANSFER_CONTEXT_MAX,
    "transfer context does not fit the http_panel_transfer copy");


/* Private variables ------------------------------------------------------- */

static const char* files_page_tag = "files_page";

extern const uint8_t files_page_html_start[] asm("_binary_files_page_html_start");
extern const uint8_t files_page_html_end[] asm("_binary_files_page_html_end");


/* Private functions ------------------------------------------------------- */

/* ---- Helpers ---- */

static void files_page_send_error(httpd_req_t* request, const char* status, const char* message)
{
    char body[FILES_PAGE_ERROR_BODY_MAX] = {0};

    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t files_page_send_ok(httpd_req_t* request)
{
    httpd_resp_set_type(request, "application/json");

    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static int32_t files_page_hex_value(char digit)
{
    int32_t value = -1;

    if ((digit >= '0') && (digit <= '9'))
    {
        value = (int32_t)(digit - '0');
    }
    else if ((digit >= 'a') && (digit <= 'f'))
    {
        value = (int32_t)(digit - 'a') + 10;
    }
    else if ((digit >= 'A') && (digit <= 'F'))
    {
        value = (int32_t)(digit - 'A') + 10;
    }
    else
    {
        /* Not a hex digit. */
    }

    return value;
}

/* '+' stays literal: clients encode a space as %20 and a plus as %2B. */
static esp_err_t files_page_decode(char* text)
{
    esp_err_t error_code = ESP_OK;
    const char* source   = text;
    char* target         = text;

    while ((*source != '\0') && (error_code == ESP_OK))
    {
        if (*source == '%')
        {
            const int32_t high = files_page_hex_value(source[1]);
            const int32_t low  = (high >= 0) ? files_page_hex_value(source[2]) : -1;
            const int32_t byte = (high * 16) + low;

            if ((high < 0) || (low < 0) || (byte == 0))
            {
                error_code = ESP_ERR_INVALID_ARG;
            }
            else
            {
                *target = (char)byte;
                target++;
                source += 3;
            }
        }
        else
        {
            *target = *source;
            target++;
            source++;
        }
    }

    *target = '\0';

    return error_code;
}

static esp_err_t files_page_get_path(httpd_req_t* request, char* full_path, size_t full_path_size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        char query[FILES_PAGE_QUERY_MAX] = {0};
        char value[FILES_PAGE_QUERY_MAX] = {0};

        error_code = httpd_req_get_url_query_str(request, query, sizeof(query));

        if (error_code != ESP_OK)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        error_code = httpd_query_key_value(query, "path", value, sizeof(value));

        if (error_code != ESP_OK)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        error_code = files_page_decode(value);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = storage_resolve_path(value, full_path, full_path_size);
    } while (0);

    return error_code;
}

/* Sends the error response itself when it fails. */
static esp_err_t files_page_prepare(httpd_req_t* request, char* full_path, size_t full_path_size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!storage_is_mounted())
        {
            files_page_send_error(request, FILES_PAGE_503, "storage not mounted");
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        error_code = files_page_get_path(request, full_path, full_path_size);

        if (error_code != ESP_OK)
        {
            files_page_send_error(request, FILES_PAGE_400, "bad path");
        }
    } while (0);

    return error_code;
}

static const char* files_page_display_path(const char* full_path)
{
    const char* path = full_path + strlen(STORAGE_MOUNT_POINT);

    if (*path == '\0')
    {
        path = "/";
    }

    return path;
}

static int files_page_entry_compare(const void* left, const void* right)
{
    const files_page_entry_t* entry_left  = (const files_page_entry_t*)left;
    const files_page_entry_t* entry_right = (const files_page_entry_t*)right;
    int result                            = 0;

    if (entry_left->is_dir != entry_right->is_dir)
    {
        result = entry_left->is_dir ? -1 : 1;
    }
    else
    {
        result = strcmp(entry_left->name, entry_right->name);
    }

    return result;
}

static size_t files_page_list_collect(
    DIR* dir,
    const char* full_path,
    files_page_entry_t* entries,
    bool* truncated)
{
    size_t count = 0u;

    const struct dirent* item = readdir(dir);

    while (item != NULL)
    {
        const size_t name_length = strlen(item->d_name);

        if ((item->d_name[0] != '.') && (name_length < FILES_PAGE_NAME_MAX))
        {
            if (count >= FILES_PAGE_LIST_MAX)
            {
                *truncated = true;
                break;
            }

            char entry_path[FILES_PAGE_ENTRY_PATH_MAX] = {0};
            struct stat info                           = {0};

            const int written =
                snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, item->d_name);

            if ((written > 0) && ((size_t)written < sizeof(entry_path))
                && (stat(entry_path, &info) == 0))
            {
                const bool is_dir = S_ISDIR(info.st_mode);

                strlcpy(entries[count].name, item->d_name, sizeof(entries[count].name));
                entries[count].is_dir = is_dir;
                entries[count].size   = is_dir ? 0u : (uint32_t)info.st_size;
                count++;
            }
        }

        item = readdir(dir);
    }

    return count;
}

static char* files_page_list_build_json(
    const char* full_path,
    const files_page_entry_t* entries,
    size_t count,
    bool truncated)
{
    char* json        = NULL;
    size_t total      = 0u;
    size_t used       = 0u;
    cJSON* const root = cJSON_CreateObject();

    if (storage_get_info(&total, &used) != ESP_OK)
    {
        total = 0u;
        used  = 0u;
    }

    if (root != NULL)
    {
        cJSON_AddStringToObject(root, "path", files_page_display_path(full_path));
        cJSON_AddNumberToObject(root, "total", (double)total);
        cJSON_AddNumberToObject(root, "used", (double)used);

        if (truncated)
        {
            cJSON_AddTrueToObject(root, "truncated");
        }

        cJSON* const array = cJSON_AddArrayToObject(root, "entries");

        for (size_t i = 0u; (array != NULL) && (i < count); i++)
        {
            cJSON* const item = cJSON_CreateObject();

            if (item == NULL)
            {
                break;
            }

            cJSON_AddStringToObject(item, "name", entries[i].name);
            cJSON_AddBoolToObject(item, "dir", entries[i].is_dir);
            cJSON_AddNumberToObject(item, "size", (double)entries[i].size);
            cJSON_AddItemToArray(array, item);
        }

        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }

    return json; /* caller must free() */
}

/* Download name for the Content-Disposition header: no quotes or control bytes. */
static void files_page_sanitize_name(const char* path, char* name, size_t name_size)
{
    const char* base = strrchr(path, '/');
    base             = (base != NULL) ? (base + 1) : path;

    strlcpy(name, base, name_size);

    for (char* character = name; *character != '\0'; character++)
    {
        const unsigned char value = (unsigned char)*character;

        if ((value < 0x20u) || (value == 0x7Fu) || (value == (unsigned char)'"')
            || (value == (unsigned char)'\\'))
        {
            *character = '_';
        }
    }
}

/* ---- Transfer ops ---- */

static esp_err_t files_page_upload_open(void* context, httpd_req_t* request)
{
    esp_err_t error_code            = ESP_OK;
    files_page_transfer_t* transfer = (files_page_transfer_t*)context;

    ARG_UNUSED(request);

    storage_hold();

    do
    {
        char parent[STORAGE_PATH_MAX] = {0};
        struct stat info              = {0};

        if (!storage_is_mounted())
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        strlcpy(parent, transfer->text, sizeof(parent));
        char* separator = strrchr(parent, '/');

        if (separator == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        *separator = '\0';

        if ((stat(parent, &info) != 0) || !S_ISDIR(info.st_mode))
        {
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        if ((stat(transfer->text, &info) == 0) && S_ISDIR(info.st_mode))
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        size_t total = 0u;
        size_t used  = 0u;

        error_code = storage_get_info(&total, &used);

        if (error_code != ESP_OK)
        {
            break;
        }

        const size_t free_bytes = (total > used) ? (total - used) : 0u;

        if ((free_bytes < FILES_PAGE_FREE_MARGIN)
            || ((free_bytes - FILES_PAGE_FREE_MARGIN) < transfer->content_length))
        {
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        transfer->file = fopen(storage_upload_temp_path(), "wb");

        if (transfer->file == NULL)
        {
            ESP_LOGE(files_page_tag, "Temp file open failed: errno %d", errno);
            error_code = ESP_FAIL;
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        storage_release();
    }

    return error_code;
}

static esp_err_t files_page_upload_write(void* context, const uint8_t* data, size_t length)
{
    const files_page_transfer_t* transfer = (const files_page_transfer_t*)context;
    esp_err_t error_code                  = ESP_OK;

    if (fwrite(data, 1u, length, transfer->file) != length)
    {
        ESP_LOGE(files_page_tag, "Write failed: errno %d", errno);
        error_code = ESP_FAIL;
    }

    return error_code;
}

static esp_err_t files_page_upload_close(void* context, bool success)
{
    esp_err_t error_code            = ESP_OK;
    files_page_transfer_t* transfer = (files_page_transfer_t*)context;

    if (fclose(transfer->file) != 0)
    {
        ESP_LOGE(files_page_tag, "Close failed: errno %d", errno);
        error_code = ESP_FAIL;
    }

    transfer->file = NULL;

    if (success && (error_code == ESP_OK))
    {
        /* LittleFS replaces an existing target atomically. */
        if (rename(storage_upload_temp_path(), transfer->text) != 0)
        {
            ESP_LOGE(files_page_tag, "Rename failed: errno %d", errno);
            error_code = ESP_FAIL;
        }
    }

    if (!success || (error_code != ESP_OK))
    {
        unlink(storage_upload_temp_path());
    }

    storage_release();

    return error_code;
}

static esp_err_t files_page_download_open(void* context, httpd_req_t* request)
{
    esp_err_t error_code            = ESP_OK;
    files_page_transfer_t* transfer = (files_page_transfer_t*)context;

    storage_hold();

    do
    {
        struct stat info = {0};

        if (!storage_is_mounted())
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if ((stat(transfer->text, &info) != 0) || S_ISDIR(info.st_mode))
        {
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        transfer->file = fopen(transfer->text, "rb");

        if (transfer->file == NULL)
        {
            ESP_LOGE(files_page_tag, "Open failed: errno %d", errno);
            error_code = ESP_FAIL;
            break;
        }

        char name[FILES_PAGE_NAME_MAX] = {0};

        files_page_sanitize_name(transfer->text, name, sizeof(name));
        snprintf(
            transfer->text, sizeof(transfer->text), "attachment; filename=\"%s\"", name);

        httpd_resp_set_type(request, "application/octet-stream");
        httpd_resp_set_hdr(request, "Content-Disposition", transfer->text);
    } while (0);

    if (error_code != ESP_OK)
    {
        storage_release();
    }

    return error_code;
}

static esp_err_t files_page_download_read(
    void* context,
    uint8_t* buffer,
    size_t capacity,
    size_t* length)
{
    const files_page_transfer_t* transfer = (const files_page_transfer_t*)context;
    esp_err_t error_code                  = ESP_OK;

    *length = fread(buffer, 1u, capacity, transfer->file);

    if ((*length == 0u) && (ferror(transfer->file) != 0))
    {
        error_code = ESP_FAIL;
    }

    return error_code;
}

static esp_err_t files_page_download_close(void* context, bool success)
{
    files_page_transfer_t* transfer = (files_page_transfer_t*)context;

    ARG_UNUSED(success);

    fclose(transfer->file);
    transfer->file = NULL;
    storage_release();

    return ESP_OK;
}

static const http_panel_transfer_ops_t files_page_upload_ops = {
    .open  = files_page_upload_open,
    .write = files_page_upload_write,
    .close = files_page_upload_close,
};

static const http_panel_transfer_ops_t files_page_download_ops = {
    .open  = files_page_download_open,
    .read  = files_page_download_read,
    .close = files_page_download_close,
};

/* ---- HTTP handlers ---- */

static esp_err_t files_page_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(files_page_html_end - files_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)files_page_html_start, (ssize_t)length);
}

static esp_err_t files_page_h_list(httpd_req_t* request)
{
    esp_err_t error_code        = ESP_OK;
    DIR* dir                    = NULL;
    files_page_entry_t* entries = NULL;
    char* json                  = NULL;

    storage_hold();

    do
    {
        char full_path[STORAGE_PATH_MAX] = {0};

        if (files_page_prepare(request, full_path, sizeof(full_path)) != ESP_OK)
        {
            break;
        }

        dir = opendir(full_path);

        if (dir == NULL)
        {
            const bool missing = (errno == ENOENT) || (errno == ENOTDIR);

            files_page_send_error(
                request,
                missing ? FILES_PAGE_404 : FILES_PAGE_500,
                missing ? "not a directory" : "open failed");
            break;
        }

        entries = heap_caps_malloc(
            FILES_PAGE_LIST_MAX * sizeof(files_page_entry_t), MALLOC_CAP_SPIRAM);

        if (entries == NULL)
        {
            files_page_send_error(request, FILES_PAGE_500, "no memory");
            break;
        }

        bool truncated     = false;
        const size_t count = files_page_list_collect(dir, full_path, entries, &truncated);

        qsort(entries, count, sizeof(files_page_entry_t), files_page_entry_compare);
        json = files_page_list_build_json(full_path, entries, count, truncated);

        if (json == NULL)
        {
            files_page_send_error(request, FILES_PAGE_500, "no memory");
            break;
        }

        httpd_resp_set_type(request, "application/json");
        error_code = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    } while (0);

    if (dir != NULL)
    {
        closedir(dir);
    }

    storage_release();
    heap_caps_free(entries);
    free(json);

    return error_code;
}

static esp_err_t files_page_h_upload(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        files_page_transfer_t transfer = {0};

        /* Closing the socket (ESP_FAIL) avoids httpd draining an unread body. */
        if (files_page_prepare(request, transfer.text, sizeof(transfer.text)) != ESP_OK)
        {
            error_code = ESP_FAIL;
            break;
        }

        if (strcmp(transfer.text, STORAGE_MOUNT_POINT) == 0)
        {
            files_page_send_error(request, FILES_PAGE_400, "bad path");
            error_code = ESP_FAIL;
            break;
        }

        transfer.content_length = request->content_len;
        error_code              = http_panel_transfer_submit(
            request, &files_page_upload_ops, &transfer, sizeof(transfer));
    } while (0);

    return error_code;
}

static esp_err_t files_page_h_download(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        files_page_transfer_t transfer = {0};

        if (files_page_prepare(request, transfer.text, sizeof(transfer.text)) != ESP_OK)
        {
            break;
        }

        error_code = http_panel_transfer_submit(
            request, &files_page_download_ops, &transfer, sizeof(transfer));
    } while (0);

    return error_code;
}

static esp_err_t files_page_h_delete(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        char full_path[STORAGE_PATH_MAX] = {0};

        if (files_page_prepare(request, full_path, sizeof(full_path)) != ESP_OK)
        {
            break;
        }

        if (strcmp(full_path, STORAGE_MOUNT_POINT) == 0)
        {
            files_page_send_error(request, FILES_PAGE_400, "cannot delete root");
            break;
        }

        const esp_err_t remove_result = storage_remove(full_path);

        if (remove_result == ESP_OK)
        {
            error_code = files_page_send_ok(request);
        }
        else if (remove_result == ESP_ERR_NOT_FOUND)
        {
            files_page_send_error(request, FILES_PAGE_404, "not found");
        }
        else if (remove_result == ESP_ERR_INVALID_STATE)
        {
            files_page_send_error(
                request, FILES_PAGE_409, "directory not empty or file in use");
        }
        else
        {
            ESP_LOGE(files_page_tag, "Delete failed: %s", esp_err_to_name(remove_result));
            files_page_send_error(request, FILES_PAGE_500, "delete failed");
        }
    } while (0);

    return error_code;
}

static esp_err_t files_page_h_mkdir(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    storage_hold();

    do
    {
        char full_path[STORAGE_PATH_MAX] = {0};
        struct stat info                 = {0};

        if (files_page_prepare(request, full_path, sizeof(full_path)) != ESP_OK)
        {
            break;
        }

        if (stat(full_path, &info) == 0)
        {
            files_page_send_error(request, FILES_PAGE_409, "already exists");
            break;
        }

        if (mkdir(full_path, 0775) == 0)
        {
            error_code = files_page_send_ok(request);
        }
        else if ((errno == ENOENT) || (errno == ENOTDIR))
        {
            files_page_send_error(request, FILES_PAGE_404, "parent not found");
        }
        else if (errno == EEXIST)
        {
            files_page_send_error(request, FILES_PAGE_409, "already exists");
        }
        else
        {
            ESP_LOGE(files_page_tag, "mkdir failed: errno %d", errno);
            files_page_send_error(request, FILES_PAGE_500, "mkdir failed");
        }
    } while (0);

    storage_release();

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t files_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t files_page_uris[] = {
        {
            .uri     = "/files",
            .method  = HTTP_GET,
            .handler = files_page_h_root,
        },
        {
            .uri     = "/files/list",
            .method  = HTTP_GET,
            .handler = files_page_h_list,
        },
        {
            .uri     = "/files/upload",
            .method  = HTTP_POST,
            .handler = files_page_h_upload,
        },
        {
            .uri     = "/files/download",
            .method  = HTTP_GET,
            .handler = files_page_h_download,
        },
        {
            .uri     = "/files/delete",
            .method  = HTTP_POST,
            .handler = files_page_h_delete,
        },
        {
            .uri     = "/files/mkdir",
            .method  = HTTP_POST,
            .handler = files_page_h_mkdir,
        },
    };

    for (size_t i = 0u; i < ARRAY_SIZE(files_page_uris); i++)
    {
        error_code = http_panel_server_register_uri(&files_page_uris[i]);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                files_page_tag,
                "Register %s failed: %s",
                files_page_uris[i].uri,
                esp_err_to_name(error_code));
            break;
        }
    }

    return error_code;
}


/**** END OF FILE ****/
