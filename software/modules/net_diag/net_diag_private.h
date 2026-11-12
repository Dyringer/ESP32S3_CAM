/**
 ******************************************************************************
 * @file    net_diag_private.h
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network diagnostics — job queue shared by the tool sources.
 *
 *          The *_run() functions execute on the net_diag worker task only.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NET_DIAG_PRIVATE_H_
#define NET_DIAG_PRIVATE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"
#include "net_diag.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum net_diag_job_type_e
{
    NET_DIAG_JOB_SCAN         = 0,
    NET_DIAG_JOB_PING         = 1,
    NET_DIAG_JOB_PING_CLEANUP = 2,
    NET_DIAG_JOB_IPERF_START  = 3,
    NET_DIAG_JOB_IPERF_STOP   = 4,
} net_diag_job_type_t;

typedef struct net_diag_job_s
{
    union
    {
        net_diag_ping_config_t ping;
        net_diag_iperf_config_t iperf;
    } config;
    net_diag_job_type_t type;
} net_diag_job_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_submit(const net_diag_job_t* job);

esp_err_t net_diag_scan_init(void);
void net_diag_scan_run(void);

esp_err_t net_diag_ping_init(void);
void net_diag_ping_run(const net_diag_ping_config_t* config);
void net_diag_ping_cleanup(void);

esp_err_t net_diag_iperf_init(void);
void net_diag_iperf_run_start(const net_diag_iperf_config_t* config);
void net_diag_iperf_run_stop(void);

#endif /* NET_DIAG_PRIVATE_H_ */

/**** END OF FILE ****/
