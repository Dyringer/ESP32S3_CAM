/**
 ******************************************************************************
 * @file    dashboard.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Dashboard page — device status and configuration.
 *
 * @note    Private to the http_panel component.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DASHBOARD_H_
#define DASHBOARD_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Initialise the dashboard page.
 *        Registers URI handlers with the server core and starts the
 *        periodic status push timer.
 *        Call from http_panel_init().
 */
esp_err_t dashboard_init(void);

#endif /* DASHBOARD_H_ */

/**** END OF FILE ****/
