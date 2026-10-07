idf_component_register(
    SRCS
        "sysmon.c"
        "sysmon_shell.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        common           # CLAMP
        esp_timer        # periodic sampler, wall clock
        freertos
        log
        shell            # app_shell_register_cmd
)
