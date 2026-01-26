/*
 * DNS Server for Captive Portal
 * Redirects all DNS queries to the ESP32's IP address
 */

#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Start the DNS server for captive portal
     *
     * All DNS queries will be responded to with the device's IP address,
     * causing browsers to redirect to the captive portal.
     *
     * @return ESP_OK on success
     */
    esp_err_t dns_server_start(void);

    /**
     * @brief Stop the DNS server
     *
     * @return ESP_OK on success
     */
    esp_err_t dns_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DNS_SERVER_H */
