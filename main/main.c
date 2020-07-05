/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"

#include "esp_camera.h"
#include "esp_timer.h"
#include "ssd1306.h"

#include "event_source.h"

#include <esp_http_server.h>

//TTGO Camera PIN Map
#define CAM_PIN_PWDN    26//-1 //power down is not used
#define CAM_PIN_RESET   -1 //software reset will be performed
#define CAM_PIN_XCLK    32//21
#define CAM_PIN_SIOD    13//26
#define CAM_PIN_SIOC    12//27

#define CAM_PIN_D7      39//35
#define CAM_PIN_D6      36//34
#define CAM_PIN_D5      23//39
#define CAM_PIN_D4      18//36
#define CAM_PIN_D3      15//19
#define CAM_PIN_D2      4//18
#define CAM_PIN_D1      14// 5
#define CAM_PIN_D0      5// 4
#define CAM_PIN_VSYNC   27//25
#define CAM_PIN_HREF    25//23
#define CAM_PIN_PCLK    19//22

static camera_config_t camera_config = {
    .pin_pwdn  = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,//YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_UXGA,//QQVGA-QXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 12, //0-63 lower number means higher quality
    .fb_count = 1 //if more than one, i2s runs in continuous mode. Use only with JPEG
};

static char *TAG = "TTGO_Camera_v2";

esp_err_t camera_init2(){
    //power up the camera if PWDN pin is defined
    if(CAM_PIN_PWDN != -1){
        gpio_set_direction(CAM_PIN_PWDN, GPIO_MODE_OUTPUT);
        int level = 0;
		gpio_set_level(CAM_PIN_PWDN, level);
    }

    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
            (uint32_t)(_jpg_buf_len/1024),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
    }

    last_frame = 0;
    return res;
}
static const httpd_uri_t camera = {
	.uri = "/camera",
	.method = HTTP_GET,
	.handler = jpg_stream_httpd_handler,
	.user_ctx = "camera"
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &camera);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}

SSD1306_t dev;
#define PIR_SENSOR_PIN 33
int blink = 0;

// Handler which executes when the timer expiry event gets executed by the loop.
static void timer_expiry_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
	if(gpio_get_level(PIR_SENSOR_PIN))
	{
		ssd1306_display_text(&dev, 6, "Pir Sensor On ", 14, true);
	}
	else
	{
		ssd1306_display_text(&dev, 6, "Pir Sensor Off", 14, false);
	}
	if(blink)
	{
		blink = 0;
		ssd1306_display_text(&dev, 7, " ", 1, false);
	}
	else
	{
		blink = 1;
		ssd1306_display_text(&dev, 7, " ", 1, true);
	}
}

// Callback that will be executed when the timer period lapses. Posts the timer expiry event
// to the default event loop.
static void timer_callback(void* arg)
{
     ESP_ERROR_CHECK(esp_event_post(TIMER_EVENTS, TIMER_EVENT_EXPIRY, NULL, 0, portMAX_DELAY));
}

esp_timer_handle_t TIMER;
/* Event source periodic timer related definitions */
ESP_EVENT_DEFINE_BASE(TIMER_EVENTS);

void app_main()
{
    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET

    camera_init2();

    /* Start the server for the first time */
    server = start_webserver();

#if CONFIG_I2C_INTERFACE
	ESP_LOGI(TAG, "INTERFACE is i2c");
	ESP_LOGI(TAG, "CONFIG_SDA_GPIO=%d",CONFIG_SDA_GPIO);
	ESP_LOGI(TAG, "CONFIG_SCL_GPIO=%d",CONFIG_SCL_GPIO);
	ESP_LOGI(TAG, "CONFIG_RESET_GPIO=%d",CONFIG_RESET_GPIO);
	i2c_master_init(CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
#if CONFIG_SSD1306_128x64
	ESP_LOGI(TAG, "Panel is 128x64");
	i2c_init(&dev, 128, 64, 0x3C);
#endif // CONFIG_SSD1306_128x64
#endif // CONFIG_I2C_INTERFACE

	ssd1306_clear_screen(&dev, false);
	ssd1306_contrast(&dev, 0xff);
	ssd1306_display_text(&dev, 0, TAG, 14, false);
	ssd1306_display_text(&dev, 1, " SSD1306 128x64", 15, false);

	tcpip_adapter_ip_info_t s_ip_info;
	char char_ip_addr[16];
	int strLen;

	ssd1306_display_text(&dev, 3, "IP address:", 11, false);

	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &s_ip_info);
	strLen = sprintf(char_ip_addr, IPSTR, IP2STR(&s_ip_info.ip));
	ssd1306_display_text(&dev, 4, char_ip_addr, strLen, false);

    gpio_set_direction(PIR_SENSOR_PIN, GPIO_MODE_INPUT);

    ESP_ERROR_CHECK(esp_event_handler_register(TIMER_EVENTS, TIMER_EVENT_EXPIRY, timer_expiry_handler, NULL));

    // Create and start the event sources
    esp_timer_create_args_t timer_args = {
        .callback = &timer_callback,
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &TIMER));
    ESP_ERROR_CHECK(esp_timer_start_periodic(TIMER, TIMER_PERIOD));
}
