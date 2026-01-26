/*
 * DNS Server for Captive Portal
 * Redirects all DNS queries to the ESP32's IP address
 */

#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>

static const char *TAG = "dns_server";

/* DNS header structure */
typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static TaskHandle_t dns_task_handle = NULL;
static int dns_socket = -1;
static bool dns_running = false;

/* The IP address to respond with (SoftAP default gateway) */
static const uint8_t captive_ip[4] = {192, 168, 4, 1};

static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (dns_running)
    {
        int len = recvfrom(dns_socket, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 0)
        {
            if (dns_running)
            {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            }
            continue;
        }

        if (len < (int)sizeof(dns_header_t))
        {
            continue;
        }

        dns_header_t *req_header = (dns_header_t *)rx_buffer;

        /* Build response */
        memcpy(tx_buffer, rx_buffer, len);
        dns_header_t *resp_header = (dns_header_t *)tx_buffer;

        /* Set response flags: QR=1 (response), AA=1 (authoritative), no error */
        resp_header->flags = htons(0x8400);
        resp_header->ancount = htons(1);

        /* Add answer section after the query */
        int response_len = len;
        uint8_t *answer = tx_buffer + response_len;

        /* Name pointer to original query (0xC00C points to offset 12) */
        answer[0] = 0xC0;
        answer[1] = 0x0C;

        /* Type A (host address) */
        answer[2] = 0x00;
        answer[3] = 0x01;

        /* Class IN */
        answer[4] = 0x00;
        answer[5] = 0x01;

        /* TTL (60 seconds) */
        answer[6] = 0x00;
        answer[7] = 0x00;
        answer[8] = 0x00;
        answer[9] = 0x3C;

        /* Data length (4 bytes for IPv4) */
        answer[10] = 0x00;
        answer[11] = 0x04;

        /* IP address */
        memcpy(&answer[12], captive_ip, 4);

        response_len += 16;

        /* Send response */
        sendto(dns_socket, tx_buffer, response_len, 0,
               (struct sockaddr *)&client_addr, addr_len);

        ESP_LOGD(TAG, "DNS query handled, redirected to captive portal");
    }

    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void)
{
    if (dns_running)
    {
        return ESP_OK; /* Already running */
    }

    /* Create UDP socket */
    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_socket < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind to DNS port 53 */
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind socket: errno %d", errno);
        close(dns_socket);
        dns_socket = -1;
        return ESP_FAIL;
    }

    dns_running = true;

    /* Start DNS server task */
    BaseType_t ret = xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create DNS task");
        dns_running = false;
        close(dns_socket);
        dns_socket = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DNS server started");
    return ESP_OK;
}

esp_err_t dns_server_stop(void)
{
    if (!dns_running)
    {
        return ESP_OK;
    }

    dns_running = false;

    if (dns_socket >= 0)
    {
        shutdown(dns_socket, SHUT_RDWR);
        close(dns_socket);
        dns_socket = -1;
    }

    if (dns_task_handle != NULL)
    {
        /* Give task time to exit */
        vTaskDelay(pdMS_TO_TICKS(100));
        dns_task_handle = NULL;
    }

    ESP_LOGI(TAG, "DNS server stopped");
    return ESP_OK;
}
