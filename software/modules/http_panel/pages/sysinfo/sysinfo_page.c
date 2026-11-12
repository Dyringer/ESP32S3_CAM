/**
 ******************************************************************************
 * @file    sysinfo_page.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   System info page — FreeRTOS tasks, per-core load and heap usage.
 *
 *          Polled by the page; task loads come from the last sysmon window,
 *          so a request never waits for a measurement.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sysinfo_page.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "sysmon.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct sysinfo_page_item_s
{
    const char* label;
    hp_value_fn_t get_value;
} sysinfo_page_item_t;

typedef struct sysinfo_page_heap_s
{
    const char* name;
    uint32_t caps;
} sysinfo_page_heap_t;


/* Private variables ------------------------------------------------------- */

extern const uint8_t sysinfo_page_html_start[] asm("_binary_sysinfo_page_html_start");
extern const uint8_t sysinfo_page_html_end[] asm("_binary_sysinfo_page_html_end");

static const sysinfo_page_item_t sysinfo_page_info_items[] = {
    {"Board", hp_get_board},
    {"Chip", hp_get_chip},
    {"Revision", hp_get_chip_revision},
    {"Cores", hp_get_core_count},
    {"CPU clock", hp_get_cpu_freq},
    {"FreeRTOS tick", hp_get_tick_rate},
    {"Tasks", hp_get_task_count},
    {"Uptime", hp_get_uptime},
    {"Reset reason", hp_get_reset_reason},
};

static const sysinfo_page_item_t sysinfo_page_flash_items[] = {
    {"Size", hp_get_flash_size},
    {"Chip", hp_get_flash_id},
    {"Mode", hp_get_flash_mode},
    {"Clock", hp_get_flash_speed},
    {"App partition", hp_get_app_partition},
};

static const sysinfo_page_item_t sysinfo_page_psram_items[] = {
    {"Size", hp_get_psram_size},
    {"Mode", hp_get_psram_mode},
    {"Clock", hp_get_psram_speed},
    {"malloc() uses PSRAM", hp_get_psram_malloc_threshold},
    {"Internal kept free", hp_get_psram_internal_reserve},
};

static const sysinfo_page_heap_t sysinfo_page_heaps[] = {
    {"Internal SRAM", MALLOC_CAP_INTERNAL},
    {"PSRAM", MALLOC_CAP_SPIRAM},
};


/* Private functions ------------------------------------------------------- */

static const char* sysinfo_page_task_state_name(eTaskState state)
{
    const char* name = NULL;

    switch (state)
    {
        case eRunning:
        {
            name = "running";
        }
        break;
        case eReady:
        {
            name = "ready";
        }
        break;
        case eBlocked:
        {
            name = "blocked";
        }
        break;
        case eSuspended:
        {
            name = "suspended";
        }
        break;
        case eDeleted:
        {
            name = "deleted";
        }
        break;
        default:
        {
            name = "invalid";
        }
        break;
    }

    return name;
}

static void sysinfo_page_add_items(
    cJSON* root,
    const char* name,
    const sysinfo_page_item_t* items,
    size_t count)
{
    cJSON* const array                         = cJSON_AddArrayToObject(root, name);
    char buffer[HTTP_PANEL_PROVIDER_VALUE_LEN] = {0};

    for (size_t i = 0u; i < count; i++)
    {
        buffer[0] = '\0';
        items[i].get_value(buffer, sizeof(buffer));

        cJSON* const item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", items[i].label);
        cJSON_AddStringToObject(item, "value", buffer);
        cJSON_AddItemToArray(array, item);
    }
}

static void sysinfo_page_add_heaps(cJSON* root)
{
    cJSON* const array = cJSON_AddArrayToObject(root, "heaps");

    for (size_t i = 0u; i < ARRAY_SIZE(sysinfo_page_heaps); i++)
    {
        multi_heap_info_t info = {0};
        heap_caps_get_info(&info, sysinfo_page_heaps[i].caps);

        cJSON* const item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", sysinfo_page_heaps[i].name);
        cJSON_AddNumberToObject(
            item, "total", (double)heap_caps_get_total_size(sysinfo_page_heaps[i].caps));
        cJSON_AddNumberToObject(item, "free", (double)info.total_free_bytes);
        cJSON_AddNumberToObject(item, "min_free", (double)info.minimum_free_bytes);
        cJSON_AddNumberToObject(item, "largest", (double)info.largest_free_block);
        cJSON_AddItemToArray(array, item);
    }
}

static void sysinfo_page_add_task(cJSON* array, const sysmon_task_t* task)
{
    cJSON* const item = cJSON_CreateObject();

    cJSON_AddStringToObject(item, "name", task->name);
    cJSON_AddStringToObject(item, "state", sysinfo_page_task_state_name(task->state));
    cJSON_AddNumberToObject(item, "prio", (double)task->priority);
    cJSON_AddNumberToObject(item, "base_prio", (double)task->base_priority);
    cJSON_AddNumberToObject(item, "core", (double)task->core);
    cJSON_AddNumberToObject(item, "cpu", (double)task->cpu_permille);
    cJSON_AddNumberToObject(item, "stack_free", (double)task->stack_free);
    cJSON_AddNumberToObject(item, "stack_size", (double)task->stack_size);
    cJSON_AddBoolToObject(item, "stack_psram", task->stack_in_psram);
    cJSON_AddItemToArray(array, item);
}

static void sysinfo_page_add_tasks(cJSON* root)
{
    sysmon_task_t* const tasks = malloc(SYSMON_MAX_TASKS * sizeof(sysmon_task_t));
    size_t count               = 0u;
    esp_err_t error_code       = ESP_ERR_NO_MEM;

    if (tasks != NULL)
    {
        error_code = sysmon_get_tasks(tasks, SYSMON_MAX_TASKS, &count);
    }

    if (error_code != ESP_OK)
    {
        cJSON_AddStringToObject(root, "tasks_error", esp_err_to_name(error_code));
    }
    else
    {
        cJSON* const array = cJSON_AddArrayToObject(root, "tasks");

        for (size_t i = 0u; i < count; i++)
        {
            sysinfo_page_add_task(array, &tasks[i]);
        }
    }

    free(tasks);
}

static char* sysinfo_page_build_json(void)
{
    char* json  = NULL;
    cJSON* root = cJSON_CreateObject();

    if (root != NULL)
    {
        cJSON_AddNumberToObject(root, "window_ms", (double)SYSMON_SAMPLE_PERIOD_MS);
        hp_add_cpu_load(root);
        sysinfo_page_add_items(
            root, "info", sysinfo_page_info_items, ARRAY_SIZE(sysinfo_page_info_items));
        sysinfo_page_add_items(
            root, "flash", sysinfo_page_flash_items, ARRAY_SIZE(sysinfo_page_flash_items));
        sysinfo_page_add_items(
            root, "psram", sysinfo_page_psram_items, ARRAY_SIZE(sysinfo_page_psram_items));
        sysinfo_page_add_heaps(root);
        sysinfo_page_add_tasks(root);

        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }

    return json; /* caller must free() */
}

/* ---- HTTP handlers ---- */

static esp_err_t sysinfo_page_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(sysinfo_page_html_end - sysinfo_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)sysinfo_page_html_start, (ssize_t)length);
}

static esp_err_t sysinfo_page_h_data(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    char* json           = sysinfo_page_build_json();

    if (json == NULL)
    {
        error_code = httpd_resp_send_500(request);
    }
    else
    {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        error_code = httpd_resp_sendstr(request, json);
        free(json);
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysinfo_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t sysinfo_page_uris[] = {
        {
            .uri     = "/sysinfo",
            .method  = HTTP_GET,
            .handler = sysinfo_page_h_root,
        },
        {
            .uri     = "/sysinfo/data",
            .method  = HTTP_GET,
            .handler = sysinfo_page_h_data,
        },
    };

    for (size_t i = 0u; (error_code == ESP_OK) && (i < ARRAY_SIZE(sysinfo_page_uris)); i++)
    {
        error_code = http_panel_server_register_uri(&sysinfo_page_uris[i]);
    }

    return error_code;
}

/**** END OF FILE ****/
