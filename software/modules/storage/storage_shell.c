/**
 ******************************************************************************
 * @file    storage_shell.c
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   Shell command "fs": ls, df, rm, mkdir, cat, format.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "storage_shell.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app_shell.h"
#include "sdkconfig.h"
#include "storage.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define STORAGE_SHELL_USAGE \
    "Usage: fs <ls [path] | df | rm <path> | mkdir <path> | cat <path> | format yes>\n"

/* Commands run on the UART console task (4 KB stack) or the httpd task, and
 * the web console splits lines at 256 bytes: keep chunks small. */
#define STORAGE_SHELL_CAT_CHUNK     128u
#define STORAGE_SHELL_CAT_MAX       4096u

#define STORAGE_SHELL_ENTRY_PATH_MAX \
    (STORAGE_PATH_MAX + (size_t)CONFIG_LITTLEFS_OBJ_NAME_LEN)

#define STORAGE_SHELL_DIR_MODE      0755u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static bool storage_shell_resolve(
    const char* relative_path,
    char* full_path,
    size_t full_path_size,
    app_shell_print_fn_t print)
{
    const esp_err_t error_code =
        storage_resolve_path(relative_path, full_path, full_path_size);

    if (error_code != ESP_OK)
    {
        print("Invalid path: %s\n", relative_path);
    }

    return (error_code == ESP_OK);
}

static void storage_shell_print_entry(
    const char* directory_path,
    const char* name,
    app_shell_print_fn_t print)
{
    char entry_path[STORAGE_SHELL_ENTRY_PATH_MAX] = {0};
    struct stat info                              = {0};

    const int length =
        snprintf(entry_path, sizeof(entry_path), "%s/%s", directory_path, name);

    if ((length < 0) || ((size_t)length >= sizeof(entry_path)))
    {
        print("  %-40s (path too long)\n", name);
    }
    else if (stat(entry_path, &info) != 0)
    {
        print("  %-40s (stat failed, errno %d)\n", name, errno);
    }
    else if (S_ISDIR(info.st_mode))
    {
        print("  %s/\n", name);
    }
    else
    {
        print("  %-40s %10u\n", name, (unsigned)info.st_size);
    }
}

/* ---- Commands ---- */

static void storage_shell_cmd_ls(const char* relative_path, app_shell_print_fn_t print)
{
    char full_path[STORAGE_PATH_MAX] = {0};

    if (storage_shell_resolve(relative_path, full_path, sizeof(full_path), print))
    {
        DIR* const directory = opendir(full_path);

        if (directory == NULL)
        {
            print("Can't open %s: errno %d\n", full_path, errno);
        }
        else
        {
            uint32_t count             = 0u;
            const struct dirent* entry = readdir(directory);

            print("%s:\n", full_path);

            while (entry != NULL)
            {
                /* Hidden names (upload temp file) are unreachable by path. */
                if (entry->d_name[0] != '.')
                {
                    storage_shell_print_entry(full_path, entry->d_name, print);
                    count++;
                }

                entry = readdir(directory);
            }

            closedir(directory);
            print("%u entr%s\n", (unsigned)count, (count == 1u) ? "y" : "ies");
        }
    }
}

static void storage_shell_cmd_df(app_shell_print_fn_t print)
{
    size_t total_bytes = 0u;
    size_t used_bytes  = 0u;

    const esp_err_t error_code = storage_get_info(&total_bytes, &used_bytes);

    if (error_code != ESP_OK)
    {
        print("Info failed: %s\n", esp_err_to_name(error_code));
    }
    else
    {
        print(
            "Total: %u bytes, used: %u bytes, free: %u bytes\n",
            (unsigned)total_bytes,
            (unsigned)used_bytes,
            (unsigned)(total_bytes - MIN(used_bytes, total_bytes)));
    }
}

static void storage_shell_cmd_rm(const char* relative_path, app_shell_print_fn_t print)
{
    char full_path[STORAGE_PATH_MAX] = {0};

    if (storage_shell_resolve(relative_path, full_path, sizeof(full_path), print))
    {
        const esp_err_t error_code = storage_remove(full_path);

        if (error_code == ESP_OK)
        {
            print("Removed %s\n", full_path);
        }
        else if (error_code == ESP_ERR_NOT_FOUND)
        {
            print("Not found: %s\n", full_path);
        }
        else if (error_code == ESP_ERR_INVALID_STATE)
        {
            print("Can't remove %s: not empty, busy or not mounted\n", full_path);
        }
        else
        {
            print("Can't remove %s: %s\n", full_path, esp_err_to_name(error_code));
        }
    }
}

static void storage_shell_cmd_mkdir(const char* relative_path, app_shell_print_fn_t print)
{
    char full_path[STORAGE_PATH_MAX] = {0};

    if (storage_shell_resolve(relative_path, full_path, sizeof(full_path), print))
    {
        if (mkdir(full_path, STORAGE_SHELL_DIR_MODE) == 0)
        {
            print("Created %s\n", full_path);
        }
        else if (errno == EEXIST)
        {
            print("Already exists: %s\n", full_path);
        }
        else
        {
            print("Can't create %s: errno %d\n", full_path, errno);
        }
    }
}

static void storage_shell_put_text(char* text, size_t length, app_shell_print_fn_t print)
{
    for (size_t i = 0u; i < length; i++)
    {
        const uint8_t character = (uint8_t)text[i];

        if ((character < 0x20u) && (character != (uint8_t)'\n')
            && (character != (uint8_t)'\t'))
        {
            text[i] = '.';
        }
    }

    text[length] = '\0';
    print("%s", text);
}

static void storage_shell_cmd_cat(const char* relative_path, app_shell_print_fn_t print)
{
    char full_path[STORAGE_PATH_MAX] = {0};

    if (storage_shell_resolve(relative_path, full_path, sizeof(full_path), print))
    {
        FILE* const file = fopen(full_path, "r");

        if (file == NULL)
        {
            print("Can't open %s: errno %d\n", full_path, errno);
        }
        else
        {
            char buffer[STORAGE_SHELL_CAT_CHUNK + 1u] = {0};
            size_t total_length                       = 0u;
            size_t length = fread(buffer, 1u, STORAGE_SHELL_CAT_CHUNK, file);

            while ((length > 0u) && (total_length < STORAGE_SHELL_CAT_MAX))
            {
                storage_shell_put_text(buffer, length, print);
                total_length += length;
                length = fread(buffer, 1u, STORAGE_SHELL_CAT_CHUNK, file);
            }

            if (length > 0u)
            {
                print("\n... (truncated at %u bytes)\n", (unsigned)total_length);
            }
            else if (ferror(file) != 0)
            {
                print("\nRead error: errno %d\n", errno);
            }
            else
            {
                print("\n");
            }

            fclose(file);
        }
    }
}

static void storage_shell_cmd_format(app_shell_print_fn_t print)
{
    print("Formatting...\n");

    const esp_err_t error_code = storage_format();

    if (error_code == ESP_OK)
    {
        print("Formatted\n");
    }
    else if (error_code == ESP_ERR_INVALID_STATE)
    {
        print("Can't format: not mounted or a file is open\n");
    }
    else
    {
        print("Format failed: %s\n", esp_err_to_name(error_code));
    }
}

static void storage_shell_dispatch(int argc, char** argv, app_shell_print_fn_t print)
{
    const char* const command  = argv[1];
    const char* const argument = (argc == 3) ? argv[2] : NULL;
    const bool confirmed       = (argument != NULL) && (strcmp(argument, "yes") == 0);

    if ((strcmp(command, "ls") == 0) && (argc <= 3))
    {
        storage_shell_cmd_ls((argument != NULL) ? argument : "", print);
    }
    else if ((strcmp(command, "df") == 0) && (argc == 2))
    {
        storage_shell_cmd_df(print);
    }
    else if ((strcmp(command, "rm") == 0) && (argc == 3))
    {
        storage_shell_cmd_rm(argv[2], print);
    }
    else if ((strcmp(command, "mkdir") == 0) && (argc == 3))
    {
        storage_shell_cmd_mkdir(argv[2], print);
    }
    else if ((strcmp(command, "cat") == 0) && (argc == 3))
    {
        storage_shell_cmd_cat(argv[2], print);
    }
    else if ((strcmp(command, "format") == 0) && confirmed)
    {
        storage_shell_cmd_format(print);
    }
    else
    {
        print(STORAGE_SHELL_USAGE);
    }
}

static void storage_shell_cmd(int argc, char** argv, app_shell_print_fn_t print)
{
    if (argc < 2)
    {
        print(STORAGE_SHELL_USAGE);
    }
    else if (strcmp(argv[1], "format") == 0)
    {
        /* storage_format() takes no hold of its own. */
        storage_shell_dispatch(argc, argv, print);
    }
    else
    {
        /* Hold first, then check, so a format can't start in between. */
        storage_hold();

        if (storage_is_mounted())
        {
            storage_shell_dispatch(argc, argv, print);
        }
        else
        {
            print("Storage not mounted\n");
        }

        storage_release();
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t storage_shell_register(void)
{
    return app_shell_register_cmd(
        "fs", "LittleFS files: fs ls|df|rm|mkdir|cat|format", storage_shell_cmd);
}

/**** END OF FILE ****/
