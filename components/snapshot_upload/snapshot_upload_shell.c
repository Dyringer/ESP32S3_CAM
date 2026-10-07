/**
 ******************************************************************************
 * @file    snapshot_upload_shell.c
 * @author  Daniel Dyringer
 * @date    2026-10-04
 * @brief   Snapshot uploader shell commands — status, url, auth, interval,
 *          on/off.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "snapshot_upload_shell.h"

#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "esp_timer.h"
#include "snapshot_upload.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SNAPSHOT_UPLOAD_SHELL_USAGE \
    "Usage: upload <status | url <url> | auth <user> <password> | auth off |\n" \
    "               interval <seconds> | on | off>\n" \
    "  Quote values containing spaces: upload auth cam \"my secret\"\n"

// clang-format on


/* Private functions ------------------------------------------------------- */

/** @brief Print "<n> s ago" for an esp_timer timestamp, or "never". */
static void snapshot_upload_shell_print_ago(app_shell_print_fn_t print, const char *label, int64_t at_us)
{
    if (at_us == 0)
    {
        print("%-12s: never\n", label);
        return;
    }

    print("%-12s: %lld s ago\n", label, (long long)((esp_timer_get_time() - at_us) / 1000000));
}

static void snapshot_upload_shell_cmd_status(app_shell_print_fn_t print)
{
    snapshot_upload_status_t status;

    snapshot_upload_get_status(&status);

    print("%-12s: %s\n", "Uploads", status.enabled ? "on" : "off");
    print("%-12s: %s\n", "URL", status.url[0] ? status.url : "(not set)");

    if (status.user[0])
    {
        print("%-12s: %s / %s\n", "Auth", status.user, status.has_password ? "********" : "(no password)");
    }
    else
    {
        print("%-12s: none\n", "Auth");
    }

    print("%-12s: %u s\n", "Interval", (unsigned)status.interval_s);
    print("\n");
    print("%-12s: %s\n", "Last result", status.last_result);
    snapshot_upload_shell_print_ago(print, "Last attempt", status.last_attempt_us);
    snapshot_upload_shell_print_ago(print, "Last OK", status.last_ok_us);

    if (status.last_attempt_us != 0)
    {
        print("%-12s: %u bytes in %u ms\n", "Last upload",
              (unsigned)status.last_size, (unsigned)status.last_duration_ms);
    }

    print("%-12s: %u ok, %u failed\n", "Since boot",
          (unsigned)status.ok_count, (unsigned)status.fail_count);
}

static void snapshot_upload_shell_cmd(int argc, char **argv, app_shell_print_fn_t print)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0)
    {
        if (argc < 2)
        {
            print(SNAPSHOT_UPLOAD_SHELL_USAGE "\n");
        }

        snapshot_upload_shell_cmd_status(print);
        return;
    }

    const char *sub        = argv[1];
    esp_err_t   error_code = ESP_OK;

    if (strcmp(sub, "url") == 0 && argc == 3)
    {
        error_code = snapshot_upload_set_url(argv[2]);

        if (error_code == ESP_ERR_INVALID_ARG)
        {
            print("URL must start with http:// or https:// and be under %u characters\n",
                  (unsigned)SNAPSHOT_UPLOAD_URL_MAX);
            return;
        }
    }
    else if (strcmp(sub, "auth") == 0 && argc == 3 && strcmp(argv[2], "off") == 0)
    {
        error_code = snapshot_upload_set_auth(NULL, NULL);
    }
    else if (strcmp(sub, "auth") == 0 && argc == 4)
    {
        error_code = snapshot_upload_set_auth(argv[2], argv[3]);

        if (error_code == ESP_ERR_INVALID_ARG)
        {
            print("User and password must each be under %u characters\n",
                  (unsigned)SNAPSHOT_UPLOAD_CRED_MAX);
            return;
        }
    }
    else if (strcmp(sub, "interval") == 0 && argc == 3)
    {
        char         *end   = NULL;
        const long    value = strtol(argv[2], &end, 10);

        if (end == argv[2] || *end != '\0' || value < 0)
        {
            print("Usage: upload interval <seconds>\n");
            return;
        }

        error_code = snapshot_upload_set_interval((uint32_t)value);

        if (error_code == ESP_ERR_INVALID_ARG)
        {
            print("Interval must be %u..%u s\n",
                  (unsigned)SNAPSHOT_UPLOAD_INTERVAL_MIN_S, (unsigned)SNAPSHOT_UPLOAD_INTERVAL_MAX_S);
            return;
        }
    }
    else if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0)
    {
        error_code = snapshot_upload_set_enabled(strcmp(sub, "on") == 0);

        if (error_code == ESP_ERR_INVALID_STATE)
        {
            print("Set a URL first: upload url <url>\n");
            return;
        }
    }
    else
    {
        print(SNAPSHOT_UPLOAD_SHELL_USAGE);
        return;
    }

    if (error_code != ESP_OK)
    {
        print("Failed: %s\n", esp_err_to_name(error_code));
        return;
    }

    print("OK (saved)\n");
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t snapshot_upload_shell_register(void)
{
    return app_shell_register_cmd(
        "upload",
        "Snapshot upload: upload <status|url|auth|interval|on|off>",
        snapshot_upload_shell_cmd);
}


/**** END OF FILE ****/
