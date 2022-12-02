/**
    \file at_manager.c
    \brief Brief description	At command for Mqtt and ntp
*/
#include <Arduino.h>
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#if __USE_WITH_ESP_IDF__
#include "log.h"
#endif
#include "list.h"
#include "board_include.h"

#define AT_MANAGER_5 0
#define AT_MANAGER_4 1
#define AT_MANAGER_3 1
#define AT_MANAGER_2 1
#define AT_MANAGER_1 1

#define __CMD_TIMEOUR__ 3 // 3s timeout for command

#define FLAG_MQTT_CONNECTED BIT10
#define FLAG_MQTT_PUBLISHED_OK BIT1
#define FLAG_MQTT_RECONNECT BIT2
#define FLAG_MQTT_POWERON BIT4
#define FLAG_MQTT_PUBLISHED_FAIL BIT5
#define FLAG_RTC_GOT_CLOCK BIT6
#define FLAG_MQTT_STARTED BIT7
#define FLAG_MQTT_TO_PUB BIT8

#define __PUBLISH_TIMEOUT__ (1000 * 4) // 5 s timeout for publish
#define __RECONNECT_COUNT__ 3

enum
{
  mqtt_pub_idle,
  mqtt_pub_get,
  mqtt_pub_free,
  mqtt_timeout,
  mqtt_wait_publish,
  mqtt_wait_publishErr,
  mqtt_wait_reconnect,
  mqtt_flush
} mqtt_pub_state;

enum
{
  __AT,
  __ATE0,
  __ATCSQ,
  __ATCREG_SET,
  __ATCREG_CHECK,
  __ATCGREG_CHECK,
  __ATCGDCONT_SET,
  __ATCGPADDR_GET,
  __ATCNTPCID_SET,
  __ATCNTP_SET,
  __ATCNTP,
  __ATCNTP_GET,
  __ATCCLk_GET,
  __ATCMQTTSTART,
  __ATCMQTTACCQ,
  __ATCMQTTCONNECT,
  __ATCMQTTCONNECT_CHECK,
  __ATCMQTTDISC,
  __ATCMQTTREL,
  __ATCMQTTSTOP,
  __ATCMQTTTOPIC_SET,
  __ATCMQTTPAYLOAD_SET,
  __ATCMQTTPUB,
  __PAYLOAD,
  __TOPIC,
  __SEND_CMD,
  __SEND_DATA
} AT_CMD;

typedef struct node
{
  void *memory;
  struct node *next;
} node_t;

//!< data structure to handle data
typedef struct __at_mqtt_data
{
  char *data;
  char *topic;
  uint32_t size;
} at_mqtt_data;
typedef struct __gsm_data
{
  QueueHandle_t xQueue_mqtt_pub;
  xSemaphoreHandle sem_mqtt_data; //!< create a semaphore global Handler

  at_mqtt_data pub_data;
  uint32_t rssi;
  uint32_t ber;
  char ip[32];
} gsm_data;

typedef struct __at_manager
{
  volatile EventGroupHandle_t  flag;
  uint32_t curCMD;
  uint32_t nxtCMD;
  char payload[1024];
  uint32_t size;
} at_manager;

static char rxBuffer[512];
static node_t *head_mqtt_data = NULL;
static QueueHandle_t xQueue_at_sender,
       xQueue_at_receiver;
static at_manager ATManager;
static gsm_data GSMData;
static esp_timer_handle_t oneshot_timer;
static struct tm timeinfogsm = {0};
static xTimerHandle xTimersAtExpire;

// get current time in EPOCH unix in seconds from task
time_t get_current_gsm_ts()
{
  return mktime(&timeinfogsm);
}

char *get_gsm_ip()
{
  return GSMData.ip;
}

uint32_t get_sim_rssi_at()
{
  return GSMData.rssi;
}

const char *at_cmd_list[] =
{
  "AT\r",
  "ATE0\r",
  "AT+CSQ\r",
  "AT+CREG=1\r",
  "AT+CREG?\r",
  "AT+CGREG?\r",
  "AT+CGDCONT=1,\"IP\",",
  "AT+CGPADDR\r",
  "AT+CNTPCID=1\r",
  "AT+CNTP=\"in.pool.ntp.org\",0\r",
  "AT+CNTP\r",
  "AT+CNTP?\r",
  "AT+CCLK?\r",
  "AT+CMQTTSTART\r",
  "AT+CMQTTACCQ=0,",
  "AT+CMQTTCONNECT=0,",
  "AT+CMQTTCONNECT?\r",
  "AT+CMQTTDISC=0,60\r",
  "AT+CMQTTREL=0\r",
  "AT+CMQTTSTOP\r",
  "AT+CMQTTTOPIC=0,",
  "AT+CMQTTPAYLOAD=0,",
  "AT+CMQTTPUB=0,1,120\r"

};

#if __STATIC_DEBUG__
const char *mqtt_tmp =
  "{\"P_Id\":18,\"name\":\"S17\",\"lat\":0,\"NSO\":\"N\",\"lon\":0,\"EWO\":\"E\",\"Alt\":0,\"sog\":0,\"cog\":0,\"hdop\":0,\"vdop\":0,\"pdop\":0,\"D_Time\":\"2022-08-21 07:36:44\",\"DHT_RH\":77,\"DHT_TEMP\":31.7,\"DF_PM1\":15,\"DF_PM25\":22,\"DF_PM10\":28,\"co2\":517}";
#endif

/*
	function prototype
*/
/// @brief send at cmd priority
void send_to_at_managerFirst(uint32_t code);

void send_ATcmd(char *buffer, uint32_t len);
void send_to_at_manager(uint32_t code);

uint32_t serial2_receive_cb(uint8_t *arg, uint32_t length)
{
  at_manager *pManager = &ATManager;

  memcpy(rxBuffer, arg, length);
  rxBuffer[length] = 0;
  pManager->size = length;

  if (xQueueSend(xQueue_at_receiver,
                 (void *)&pManager->curCMD,
                 (TickType_t)0) != pdPASS)
  {
    xQueueReset(xQueue_at_receiver);
#if AT_MANAGER_1
    /* Failed to post the message, even after 10 ticks. */
#if __USE_WITH_ESP_IDF__
    log_debug("Failed to send queue UART task");
#else
    Serial.println("Failed to send queue UART task");
#endif

#endif
  }
  return 0;
}

void vTimerCallbackExpiredAT(xTimerHandle xTimer)
{
#if AT_MANAGER_1
#if __USE_WITH_ESP_IDF__
  log_info("RST MCU as MQTT PUB failed for sometime");
#else
  Serial.println("RST MCU as MQTT PUB failed for sometime");
#endif
#endif
  //  CMD_UPDATE_FLASH_RST();
}

int32_t check_at_ok(char *buffer)
{
  if (strstr(buffer, "OK"))
  {
    return 1;
  }
  else if (strstr(buffer, "ERROR"))
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

void thread_sync_time_gsm()
{
#define sync_timer 0
#define add_1sec 1
  uint32_t state = 0;
  char buf[64];
  time_t rawtime = 0;
  at_manager *pManager = &ATManager;

  xEventGroupWaitBits(pManager->flag, FLAG_RTC_GOT_CLOCK, pdFALSE, pdTRUE, portMAX_DELAY);

  while (1)
  {
    switch (state)
    {
      case sync_timer:
        rawtime = mktime(&timeinfogsm);
        state = add_1sec;
        break;

      case add_1sec:
        vTaskDelay(pdMS_TO_TICKS(1000));
        rawtime += 1;
        memcpy(&timeinfogsm, localtime(&rawtime), sizeof(struct tm));
        state = sync_timer;

        strftime(buf, sizeof(buf), "%a %m/%d/%Y %r", &timeinfogsm);
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
        log_info("[%s] RTC = %s", __func__, buf);
#else
        Serial.printf("[%s] RTC = %s\n", __func__, buf);
#endif
#endif
        break;
    }
  }
}

void thread_receiver(void *arg)
{

  char *ret;
  int xStatus, at_state;
  at_manager *pManager = &ATManager;

  while (1)
  {
    xStatus = xQueueReceive(xQueue_at_receiver, &at_state, portMAX_DELAY);
    if (xStatus == pdPASS)
    {
      strcpy(pManager->payload, rxBuffer);
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
      log_debug("[%s] state: %d:%s", __func__, at_state, pManager->payload);
#else
      Serial.printf("[%s] state: %d:%s", __func__, at_state, pManager->payload); Serial.println();
#endif
#endif

      if (esp_timer_is_active(oneshot_timer) == 1)
      {

#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
        log_debug("[%s] Stop Timeout esp timer", __func__);
#else
        Serial.printf("[%s] Stop Timeout esp timer", __func__); Serial.println();
#endif
#endif
        ESP_ERROR_CHECK(esp_timer_stop(oneshot_timer));
      }
      /* check for state of GSM */
      switch (at_state)
      {
        case __ATCGDCONT_SET:
          if (check_at_ok(pManager->payload) == 1)
          {
            pManager->curCMD = __ATCGPADDR_GET;
          }
          else
          {
            pManager->curCMD = __ATCGREG_CHECK;
          }
          break;

        case __ATCGPADDR_GET:
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
          log_debug("ret[0] %s", pManager->payload);
#else
          Serial.printf("ret[0] %s", pManager->payload); Serial.println();
#endif
#endif
          // if( check_at_ok( pManager->payload ) == 1 )
          {
            ret = strstr(pManager->payload, "+CGPADDR:");
            if (ret)
            {
              ret = strstr(ret, "1,");

              if (ret)
              {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
                log_debug("ret[1] %s", ret);
#else
                Serial.printf("ret[1] %s", ret); Serial.println();
#endif
#endif
                ret += 2;
#if 0

                GSMData.ip[0] = strtol( ret, NULL, 10 );
                ret = strstr( ret , ".");
                if ( ret ) {
                  ret++;
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
                  log_debug( "ret[2] %s", ret );
#else
                  Serial.printf( "ret[2] %s", ret ); Serial.println();
#endif
#endif
                  GSMData.ip[1] = strtol( ret, NULL, 10 );
                  ret = strstr( ret , ".");
                  ret++;
                  GSMData.ip[2] = strtol( ret, NULL, 10 );
                  ret = strstr( ret , ".");
                  ret++;
                  GSMData.ip[3] = strtol( ret, NULL, 10 );

#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
                  log_debug( "GSM ip: %d.%d.%d.%d", GSMData.ip[0], GSMData.ip[1], GSMData.ip[2], GSMData.ip[3] );
#else
                  Serial.printf( "GSM ip: %d.%d.%d.%d", GSMData.ip[0], GSMData.ip[1], GSMData.ip[2], GSMData.ip[3] ); Serial.println();
#endif
#endif



                } else {
                  pManager->curCMD = __ATCGPADDR_GET;
                }
#endif

#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
                ESP_LOG_BUFFER_HEXDUMP(__func__, ret, strlen(ret), ESP_LOG_INFO);
#endif
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
                log_debug("[%s]Check flag[1] 0x%X", __func__, pManager->flag);
#else
                Serial.printf("[%s]Check flag[1] 0x%X", __func__, pManager->flag); Serial.println();
#endif
#endif

                char *ret1 = strstr(ret, "\r");

                memset(GSMData.ip, 0, 32);

                memcpy(GSMData.ip, ret, ret1 - ret);

#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
                log_debug("[%s]Check flag[2] 0x%X", __func__, pManager->flag);
#else
                Serial.printf("[%s]Check flag[2] 0x%X", __func__, pManager->flag); Serial.println();
#endif
#endif
                if (ret)
                {
                  pManager->curCMD = __ATCNTP_SET;
                }
                else
                {
                  pManager->curCMD = __ATCREG_SET;
                }

#if AT_MANAGER_1
#if __USE_WITH_ESP_IDF__
                log_debug("GSM IP %s", GSMData.ip);
#else
                Serial.printf("GSM IP %s", GSMData.ip); Serial.println();
#endif
#endif //AT_MANAGER_1
#endif
              }
            }
          }
          break;

        case __ATCSQ:
          /*+CSQ: 16,99
          */
          ret = strstr(pManager->payload, "+CSQ");
          if (ret)
          {
            ret = strstr(ret, ":");
            ret += 2; // goto number in csq
            GSMData.rssi = strtol(ret, NULL, 10);
            ret = strstr(ret, ",");
            if (ret)
            {
              GSMData.ber = strtol(++ret, NULL, 10);
            }
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
            log_debug("GSM rssi [%d], ber [%d]", GSMData.rssi, GSMData.ber);
#else
            Serial.printf("GSM rssi [%d], ber [%d]", GSMData.rssi, GSMData.ber); Serial.println();
#endif
#endif
            if (GSMData.rssi == 99)
            {
              pManager->curCMD = __ATCSQ;
            }
            else
            {
              pManager->curCMD = __ATCREG_SET;
            }
          }
          break;

        case __ATCMQTTCONNECT:
          ret = strstr(pManager->payload, "+CMQTTCONNECT:");
          if (ret)
          {
            ret = strstr(ret, ",");
            ret++;
            xStatus = strtol(ret, NULL, 10);
            if (xStatus == 0)
            {
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
              log_info("[%s]Set Connected Flag", __func__);
#else
              Serial.printf("[%s]Set Connected Flag", __func__); Serial.println();
#endif
#endif
              xEventGroupSetBits(pManager->flag, FLAG_MQTT_CONNECTED);

              //              gpio_set_level(MQTT_STATUS_IO, false);

              if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_RECONNECT)
              {
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
                log_debug("[%s]Dont set cmd if RECONNECT flag is set", __func__);
#else
                Serial.printf("[%s]Dont set cmd if RECONNECT flag is set", __func__); Serial.println();
#endif
#endif
                xEventGroupClearBits(pManager->flag, FLAG_MQTT_RECONNECT);
              }
              /*else{
              	pManager->curCMD = __ATCMQTTTOPIC_SET;
                }*/

#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
              log_debug("[%s]Set Cmd %d", __func__, pManager->curCMD);
#else
              Serial.printf("[%s]Set Cmd %d", __func__, pManager->curCMD); Serial.println();
#endif
#endif
            }
            else if (xStatus == 9)
            {
              pManager->curCMD = __ATCMQTTSTART;
            }
            else
            {
              //              gpio_set_level(MQTT_STATUS_IO, true);

#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
              log_debug("[%s]Disconnect CMD %d", __func__, pManager->curCMD);
#else
              Serial.printf("[%s]Disconnect CMD %d", __func__, pManager->curCMD); Serial.println();
#endif
#endif

              pManager->curCMD = __ATCMQTTDISC;
            }
          }
          ret = strstr(pManager->payload, "CMQTTCONNLOST");
          if (ret)
          {

#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
            log_debug("[%s]Lost, reConnect To MQTT", __func__);
#else
            Serial.printf("[%s]Lost, reConnect To MQTT", __func__); Serial.println();
#endif
#endif
            at_state = 0; // reset for state been same.
            pManager->curCMD = __ATCMQTTCONNECT;
            xEventGroupClearBits(pManager->flag, FLAG_MQTT_CONNECTED);
            //            gpio_set_level(MQTT_STATUS_IO, true);
          }
          break;
        case __ATCNTP:
          ret = strstr(pManager->payload, "+CNTP:");
          if (ret)
          {
            pManager->curCMD = __ATCCLk_GET;
          }
          break;

        case __ATCCLk_GET:
          //+CCLK: "22/03/21,16:55:04+22"
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
          log_debug("[%s] Resp: %s", __func__, pManager->payload);
#else
          Serial.printf("[%s] Resp: %s", __func__, pManager->payload); Serial.println();
#endif
#endif

          ret = strstr(pManager->payload, "+CCLK:");
          if (ret)
          {
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
            log_debug("[%s]Check PWON %x:%x", __func__, pManager->flag, FLAG_MQTT_POWERON);
#else
            Serial.printf("[%s]Check PWON %x:%x", __func__, pManager->flag, FLAG_MQTT_POWERON); Serial.println();
#endif
#endif
            // is MQTT client started ?
            if ((xEventGroupGetBits(pManager->flag) & FLAG_MQTT_STARTED))
            {
              if ((xEventGroupGetBits(pManager->flag) & FLAG_MQTT_TO_PUB))
              {
                pManager->curCMD = __ATCMQTTTOPIC_SET;
              }
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
              log_debug("[%s]Set Cmd %d", __func__, pManager->curCMD);
#else
              Serial.printf("[%s]Set Cmd %d", __func__, pManager->curCMD); Serial.println();
#endif
#endif
            }
            else
            {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
              log_debug("[%s]Clear PWON %x:%x", __func__, xEventGroupGetBits(pManager->flag), FLAG_MQTT_POWERON);
#else
              Serial.printf("[%s]Clear PWON %x:%x", __func__, xEventGroupGetBits(pManager->flag), FLAG_MQTT_POWERON); Serial.println();
#endif
#endif
              // vTaskDelay( pdMS_TO_TICKS( 50 ) );
              xEventGroupClearBits(pManager->flag, FLAG_MQTT_POWERON);

              if ((xEventGroupGetBits(pManager->flag) & FLAG_MQTT_TO_PUB))
              {
                pManager->curCMD = __ATCMQTTTOPIC_SET;
              }
              else
              {
                pManager->curCMD = __ATCMQTTSTART;
              }
            }

            ret = strstr(ret, "\"");
            ret++;
            // get year
            timeinfogsm.tm_year = strtol(ret, NULL, 10) + 2000 - 1900;
            ret = strstr(ret, "/");
            if (ret)
            {
              ret++;
              timeinfogsm.tm_mon = strtol(ret, NULL, 10) - 1;
              ret = strstr(ret, "/");
              if (ret)
              {
                ret++;
                timeinfogsm.tm_mday = strtol(ret, NULL, 10);
                // get hour
                ret = strstr(ret, ",");
                if (ret)
                {
                  ret++;
                  timeinfogsm.tm_hour = strtol(ret, NULL, 10);
                  ret = strstr(ret, ":");
                  if (ret)
                  {
                    ret++;
                    timeinfogsm.tm_min = strtol(ret, NULL, 10);
                    ret = strstr(ret, ":");
                    if (ret)
                    {
                      ret++;
                      timeinfogsm.tm_sec = strtol(ret, NULL, 10);

                      xEventGroupSetBits(pManager->flag, FLAG_RTC_GOT_CLOCK);
                    }
                  }
                }
              }
            }
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
            log_debug("[%s] Time from 4G: %04d-%02d-%02d %02d:%02d:%02d\n", __func__,
                      timeinfogsm.tm_year + 1900, timeinfogsm.tm_mon + 1, timeinfogsm.tm_mday,
                      timeinfogsm.tm_hour, timeinfogsm.tm_min, timeinfogsm.tm_sec);
#else
            Serial.printf("[%s] Time from 4G: %04d-%02d-%02d %02d:%02d:%02d\n", __func__,
                          timeinfogsm.tm_year + 1900, timeinfogsm.tm_mon + 1, timeinfogsm.tm_mday,
                          timeinfogsm.tm_hour, timeinfogsm.tm_min, timeinfogsm.tm_sec); Serial.println();
#endif
            char buf[32];
            strftime(buf, sizeof(buf), "%a %m/%d/%Y %r", &timeinfogsm);
#if __USE_WITH_ESP_IDF__
            log_debug("[%s] Timenow = %s", __func__, buf);

#else
            Serial.printf("[%s] Timenow = %s", __func__, buf); Serial.println();
#endif
#endif
          }
          else
          {
            pManager->curCMD = __ATCREG_CHECK;
          }

          break;

        case __ATCREG_CHECK:
          // CREG: 0,1
          if (check_at_ok(pManager->payload) == 1)
          {
            ret = strstr(pManager->payload, "CREG:");
            if (ret)
            {
              ret = strstr(ret, ",");
              if (ret)
              {
                ret++;
                if (strtol(ret, NULL, 10) == 1)
                {
                  pManager->curCMD = __ATCGREG_CHECK;
                }
                else
                {

                  pManager->curCMD = __ATCREG_SET;
                }
              }
            }
          }
          else
          {
            pManager->curCMD = __AT;
          }
          break;

        case __ATCMQTTPAYLOAD_SET:

          if (strstr(pManager->payload, ">"))
          {
            pManager->curCMD = __PAYLOAD;
          }
          break;

        case __ATCMQTTCONNECT_CHECK:
          if (strstr(pManager->payload, "+CMQTTCONNECT:"))
          {
            ret = strstr(pManager->payload, "0");

            if (ret)
            {
              ret = strstr(ret, ",");
              if (ret)
              {
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
                log_debug("[%s]Connected to %s", __func__, ret);
#else
                Serial.printf("[%s]Connected to %s", __func__, ret); Serial.println();
#endif
#endif
                xEventGroupSetBits(pManager->flag, FLAG_MQTT_CONNECTED);
                pManager->curCMD = __ATCMQTTTOPIC_SET;
              }
              else
              {

                pManager->curCMD = __ATCMQTTCONNECT;
              }
            }
          }
          ret = strstr(pManager->payload, "CMQTTCONNLOST");
          if (ret)
          {
            /*ret = strstr( ret , "," );
              if( ret )
              {
            	if(	strtol( ++ret, NULL 10) == 1)
            	{

            	}
              }*/
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
            log_debug("[%s]Lost, reConnect To MQTT", __func__);
#else
            Serial.printf("[%s]Lost, reConnect To MQTT", __func__); Serial.println();
#endif
#endif
            pManager->curCMD = __ATCMQTTDISC;
            xEventGroupClearBits(pManager->flag, FLAG_MQTT_CONNECTED);
            //            gpio_set_level(MQTT_STATUS_IO, true);
          }
          break;

        case __ATCMQTTDISC:
          pManager->curCMD = __ATCMQTTREL;
          //          gpio_set_level(MQTT_STATUS_IO, true);
          break;

        case __ATCMQTTREL:
          pManager->curCMD = __ATCMQTTSTOP;
          break;

        case __ATCMQTTSTART:
          if (check_at_ok(pManager->payload) == 1)
          {
            pManager->curCMD++;
            xEventGroupSetBits(pManager->flag, FLAG_MQTT_STARTED);
          }
          else
          {
          }
          break;
        case __ATCMQTTSTOP:
          pManager->curCMD = __ATCREG_CHECK;
          xEventGroupClearBits(pManager->flag, FLAG_MQTT_STARTED);
          break;

        case __ATCMQTTTOPIC_SET:
          if (strstr(pManager->payload, ">"))
          {
            pManager->curCMD = __TOPIC;
          }
          else if (strstr(pManager->payload, "+CMQTTTOPIC"))
          {
            ret = strstr(pManager->payload, "0,");
            if (ret)
            {
              ret += 2;
              xStatus = strtol(ret, NULL, 10);
              if (xStatus > 0)
              {
                pManager->curCMD = __ATCMQTTDISC;
              }
            }
          }
          else if (strstr(pManager->payload, "ERROR"))
          {
            pManager->curCMD = __ATCMQTTCONNECT_CHECK;
          }
          break;

        case __PAYLOAD:
          if (check_at_ok(pManager->payload) == 1)
          {
            pManager->curCMD = __ATCMQTTPUB;
          }
          break;
        case __TOPIC:
          if (check_at_ok(pManager->payload) == 1)
          {
            pManager->curCMD = __ATCMQTTPAYLOAD_SET;
          }
          else
          {
            pManager->curCMD = __ATCMQTTTOPIC_SET;
          }
          break;
        case __ATCMQTTPUB:
          if (strstr(pManager->payload, "CMQTTCONNLOST"))
          {
            // pManager->curCMD = __ATCMQTTDISC;
            xEventGroupClearBits(pManager->flag, FLAG_MQTT_CONNECTED);
            //            gpio_set_level(MQTT_STATUS_IO, true);
          }
          ret = strstr(pManager->payload, "+CMQTTPUB:");
          if (ret)
          {
            ret = strstr(ret, ",");
            if (ret)
            {
              ret++;
              if (strtol(ret, NULL, 10) == 0)
              {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
                log_info("[%s] Publish OK", __func__);
#else
                Serial.printf("[%s] Publish OK", __func__); Serial.println();
#endif
#endif
                xEventGroupClearBits(pManager->flag, FLAG_MQTT_TO_PUB);

                xEventGroupSetBits(pManager->flag, FLAG_MQTT_PUBLISHED_OK);
              }
              else
              {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
                log_error("[%s] Publish Err %d", __func__, strtol(ret, NULL, 10));
#else
                Serial.printf("[%s] Publish Err %d", __func__, strtol(ret, NULL, 10)); Serial.println();
#endif
#endif
                xEventGroupSetBits(pManager->flag, FLAG_MQTT_PUBLISHED_FAIL);
              }
            }
          }
          break;

        case __AT:
        default:
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
          log_debug("[%s]Response;%s", "default", pManager->payload);
          #else
                Serial.printf("[%s]Response;%s", "default", pManager->payload); Serial.println();
#endif
#endif
          if (check_at_ok(pManager->payload) == 1)
          {
            pManager->curCMD++;
          }
          else
          {
            pManager->curCMD = __AT;
          }
          /* incoming message other than fixed at command */
          if (strstr(pManager->payload, "CMQTTCONNLOST"))
          {
            pManager->curCMD = __ATCMQTTCONNECT_CHECK;
            xEventGroupClearBits(pManager->flag, FLAG_MQTT_CONNECTED);
            //            gpio_set_level(MQTT_STATUS_IO, true);
          }

          break;
      }
      // check if cmd is updated
      if (at_state != pManager->curCMD)
        send_to_at_managerFirst(pManager->curCMD);
    }
  }
}

void thread_sender(void * arg)
{
  uint8_t mac[6] = {0};
  int xStatus, at_state;
  at_manager *pManager = &ATManager;

  while (1)
  {
    xStatus = xQueueReceive(xQueue_at_sender, &at_state, portMAX_DELAY);
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
    log_debug("[%s] state: %d", __func__, at_state);
    #else
                Serial.printf("[%s] state: %d", __func__, at_state); Serial.println();
#endif
#endif
    if (xStatus == pdPASS)
    {
      /*successive command delay for sim7600*/
      vTaskDelay(pdMS_TO_TICKS(100));

      switch (at_state)
      {
        case __ATCMQTTACCQ:



          strcpy(pManager->payload, at_cmd_list[at_state]);

          sprintf(pManager->payload + strlen(pManager->payload), "\"ESP32_%02X%02X\"\r", mac[4], mac[5]);
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
          log_debug("[%s]CLIENT ID: %s", __func__, pManager->payload);
#else
          Serial.printf("[%s]CLIENT ID: %s", __func__, pManager->payload); Serial.println();
#endif
#endif

          send_ATcmd(pManager->payload, strlen(pManager->payload));
          break;
        case __ATCGDCONT_SET:
          strcpy(pManager->payload, at_cmd_list[at_state]);

          sprintf(pManager->payload + strlen(pManager->payload), "\"%s\"\r", __AIRTEL_APN__ );

          send_ATcmd(pManager->payload, strlen(pManager->payload));
          break;
        case __ATCMQTTCONNECT:
          //\"tcp://mqtt.ssexims.in:1883\",60,1,\"byke\",\"Byke@202020\"\r"
          strcpy(pManager->payload, at_cmd_list[at_state]);
#if 0
          sprintf( pManager->payload + strlen(pManager->payload) , "\"%s:%d\",60,1,\"%s\",\"%s\"\r", userData.mqtt_cfg.host,
                   userData.mqtt_cfg.port,
                   userData.mqtt_cfg.username,
                   userData.mqtt_cfg.password ) ;

#endif
#if __STATIC_DEBUG__
          sprintf(pManager->payload + strlen(pManager->payload), "\"%s:%d\",60,1", "tcp://3.111.139.83",
                  1883);
          /*sprintf(pManager->payload + strlen(pManager->payload), "\"%s:%d\",60,1", "tcp://broker.emqx.io",
          		1883);*/

#else
          sprintf(pManager->payload + strlen(pManager->payload), "\"%s:%d\",60,1", userData.mqtt_cfg.host,
                  userData.mqtt_cfg.port);

#endif
#if __STATIC_DEBUG__
          /*sprintf(pManager->payload + strlen(pManager->payload), ",\"%s\"", "123" );
            sprintf(pManager->payload + strlen(pManager->payload), ",\"%s\"", "123" );*/
#else
          if (strlen(userData.mqtt_cfg.username) > 1)
          {
            sprintf(pManager->payload + strlen(pManager->payload), ",\"%s\"", userData.mqtt_cfg.username);
          }

          if (strlen(userData.mqtt_cfg.password) > 1)
          {
            sprintf(pManager->payload + strlen(pManager->payload), ",\"%s\"", userData.mqtt_cfg.password);
          }
#endif
          strcat(pManager->payload, "\r");

#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
          log_debug("[%s] connect to: %s", __func__, pManager->payload);
#else
          Serial.printf("[%s] connect to: %s", __func__, pManager->payload); Serial.println();
#endif
#endif
          send_ATcmd(pManager->payload, strlen(pManager->payload));
          break;
        case __ATCMQTTPAYLOAD_SET:
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
          log_info("[%s]Payload: %s", __func__, GSMData.pub_data.data);
#else
          Serial.printf("[%s]Payload: %s", __func__, GSMData.pub_data.data); Serial.println();
#endif
#endif
          strcpy(pManager->payload, at_cmd_list[at_state]);

          sprintf(pManager->payload + strlen(pManager->payload), "%d\r", (strlen(GSMData.pub_data.data)));

          send_ATcmd(pManager->payload, strlen(pManager->payload));
          break;
        case __ATCMQTTTOPIC_SET:
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
          log_info("[%s] Set MQTT Topic AT", __func__);
#else
          Serial.printf("[%s] Set MQTT Topic AT", __func__); Serial.println();
#endif
#if __USE_WITH_ESP_IDF__
          log_info("[%s]Idx %d: Topic: %s", __func__, at_state, at_cmd_list[at_state]);
#else
          Serial.printf("[%s]Idx %d: Topic: %s", __func__, at_state, at_cmd_list[at_state]); Serial.println();
#endif
#endif
          strcpy(pManager->payload, at_cmd_list[at_state]);


#if !__STATIC_DEBUG__
          if (strlen(GSMData.pub_data.topic) > 0)
#endif
          {

            sprintf(pManager->payload + strlen(pManager->payload), "%d\r", strlen(GSMData.pub_data.topic));

            send_ATcmd(pManager->payload, strlen(pManager->payload));
          } /*else{

				 }*/

          break;
        case __TOPIC:
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
          log_info("[%s]Topic: %s", __func__, GSMData.pub_data.topic);
#else
          Serial.printf("[%s]Topic: %s", __func__, GSMData.pub_data.topic); Serial.println();
#endif
#endif
          if (strlen(GSMData.pub_data.topic) > 2)
            send_ATcmd(GSMData.pub_data.topic, strlen(GSMData.pub_data.topic));
          break;

        case __PAYLOAD:
          strcpy(pManager->payload, GSMData.pub_data.data);
          send_ATcmd(pManager->payload, strlen(pManager->payload));

          break;
        default:
          send_ATcmd((char *)at_cmd_list[at_state], strlen((char *)at_cmd_list[at_state]));
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
          log_debug("[%s]Fixed Cmd: %s:%d", __func__, at_cmd_list[at_state], at_state);
#else
          Serial.printf("[%s]Fixed Cmd: %s:%d", __func__, at_cmd_list[at_state], at_state); Serial.println();
#endif
#endif
      }

      if (esp_timer_is_active(oneshot_timer) == 0)
      {
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
        log_debug("[%s] Start Timeout esp timer", __func__);
#else
        Serial.printf("[%s] Start Timeout esp timer", __func__); Serial.println();
#endif
#endif
        ESP_ERROR_CHECK(esp_timer_start_periodic(oneshot_timer, __CMD_TIMEOUR__ * 1000000));
      }
    }
  }
}

uint32_t get_mqtt_status_at()
{
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
  log_debug("[%s]Flag Status: %X, Mqtt Connected %d [%d]", __func__, xEventGroupGetBits(ATManager.flag), xEventGroupGetBits(ATManager.flag) && FLAG_MQTT_CONNECTED, xEventGroupGetBits(ATManager.flag) & FLAG_MQTT_CONNECTED);
#else
  Serial.printf("[%s]Flag Status: %X, Mqtt Connected %d [%d]", __func__, xEventGroupGetBits(ATManager.flag), xEventGroupGetBits(ATManager.flag) && FLAG_MQTT_CONNECTED, xEventGroupGetBits(ATManager.flag) & FLAG_MQTT_CONNECTED); Serial.println();
#endif
#endif
  return xEventGroupGetBits(ATManager.flag) && FLAG_MQTT_CONNECTED;
}

void send_ATcmd(char *buffer, uint32_t len)
{
  uint32_t lu32written = 0;
  lu32written = Serial1.write((uint8_t *)buffer, len);
#if AT_MANAGER_5
#if __USE_WITH_ESP_IDF__
  log_debug("[%s]written: %d", __func__, lu32written);
#else
  Serial.printf("[%s]written: %d", __func__, lu32written); Serial.println();
#endif
#endif
}
void send_to_at_managerFirst(uint32_t code)
{
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
  log_debug("[%s]queue Count: %d, Code %d", __func__, uxQueueMessagesWaiting(xQueue_at_sender), code);
#else
  Serial.printf("[%s]queue Count: %d, Code %d", __func__, uxQueueMessagesWaiting(xQueue_at_sender), code); Serial.println();
#endif
#endif
  if (xQueueSendToFront(xQueue_at_sender,
                        (void *)&code,
                        (TickType_t)0) != pdPASS)
  {
    xQueueReset(xQueue_at_sender);
#if AT_MANAGER_1
#if __USE_WITH_ESP_IDF__
    /* Failed to post the message, even after 10 ticks. */
    log_debug("Failed to send q UART task");
#else
    Serial.printf("Failed to send q UART task"); Serial.println();
#endif
#endif
  }
}
void send_to_at_manager(uint32_t code)
{
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
  log_debug("[%s]queue Cnt: %d, Code %d", __func__, uxQueueMessagesWaiting(xQueue_at_sender), code);
#else
  Serial.printf("[%s]queue Cnt: %d, Code %d", __func__, uxQueueMessagesWaiting(xQueue_at_sender), code); Serial.println();
#endif
#endif
  if (xQueueSend(xQueue_at_sender,
                 (void *)&code,
                 (TickType_t)0) != pdPASS)
  {
    xQueueReset(xQueue_at_sender);
#if AT_MANAGER_1
#if __USE_WITH_ESP_IDF__
    /* Failed to post the message, even after 10 ticks. */
    log_debug("Failed to send q UART task");
#else
    Serial.printf("Failed to send q UART task"); Serial.println();
#endif
#endif
  }
}

static void oneshot_timer_callback(void *arg)
{
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
  log_debug("[%s]:Timeout******* %d", __func__, ATManager.curCMD);
#else
  Serial.printf("[%s]:Timeout******* %d", __func__, ATManager.curCMD); Serial.println();
#endif
#endif
  send_to_at_managerFirst(ATManager.curCMD);
  ESP_ERROR_CHECK(esp_timer_stop(oneshot_timer));
}

//!< copy data to mqtt buffer
void send_mqtt_data_AT(char *topic, char *source, uint32_t len)
{
  uint32_t length = 0;
  node_t *item;
  at_mqtt_data *temp = NULL;
#if AT_MANAGER_1
#if __USE_WITH_ESP_IDF__
  log_info("[%s] Assemble new packet to send to MQTT", __func__);
#else
  Serial.printf("[%s] Assemble new packet to send to MQTT", __func__); Serial.println();
#endif
#endif

  item = (node_t *)malloc(sizeof(node_t));
  item->memory = (at_mqtt_data *)malloc(sizeof(at_mqtt_data));

  /**/
  temp = (at_mqtt_data *)item->memory;
  // add payload
  temp->data = (char *)malloc(len + 1);

  memset(temp->data, 0, len + 1);
  memcpy(temp->data, source, len);
  // add topic
  temp->topic = (char *)malloc(strlen(topic) + 1);
  memset(temp->topic, 0, strlen(topic) + 1);
  memcpy(temp->topic, topic, strlen(topic));

  // save size
  temp->size = len;

  item->next = NULL;
  xSemaphoreTake(GSMData.sem_mqtt_data, portMAX_DELAY);

  SL_APPEND(head_mqtt_data, item);

  SL_LENGTH(head_mqtt_data, length);

  xSemaphoreGive(GSMData.sem_mqtt_data);
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
  log_info("[%s] List Count %d", __func__, length);
#else
  Serial.printf("[%s] List Count %d", __func__, length); Serial.println();
#endif
#endif
  // anyTime log goes over 30 reset
  if (length > 30)
  {
//    CMD_DELAY_RESET_MCU(); // MCU will reset aFTER 2 S
  }
}

void thread_mqtt_publisher( void *arg)
{
  at_manager *pManager = &ATManager;
  uint32_t state = mqtt_pub_idle;
  uint32_t length = 0;
  uint32_t Counter0 = 0;
  uint32_t reconnect_timer = 0;
  uint32_t reconnect_attempt = 0;
  node_t *item;

  while (1)
  {
    switch (state)
    {
      case mqtt_pub_idle:
        SL_LENGTH(head_mqtt_data, length);

        if (length > 0)
        {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
          log_info("[%s]item available in mqtt data list %d", __func__, length);
#else
          Serial.printf("[%s]item available in mqtt data list %d", __func__, length); Serial.println();
#endif
#endif
          if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_CONNECTED)
          {
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
            log_info("[%s]MQTT is already connected", __func__);
#else
            Serial.printf("[%s]MQTT is already connected", __func__); Serial.println();
#endif
#endif
            state = mqtt_pub_get;
            reconnect_attempt = 0;
          }
          else
          {
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
            log_error("[%s]mqtt is not connected \n\n", __func__);
#else
            Serial.printf("[%s]mqtt is not connected \n\n", __func__); Serial.println();
#endif
#endif
            xEventGroupSetBits(pManager->flag, FLAG_MQTT_RECONNECT);

            if ((xEventGroupGetBits(pManager->flag) & FLAG_MQTT_POWERON) == 0)
            {
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
              log_debug("[%s]Connect To MQTT", __func__);
#else
              Serial.printf("[%s]Connect To MQTT", __func__); Serial.println();
#endif
#endif
              pManager->curCMD = __ATCMQTTCONNECT;
              send_to_at_manager(pManager->curCMD);
            }
            else
            {
              pManager->curCMD = __ATCSQ;
            }
            reconnect_attempt++;
            if (reconnect_attempt > __RECONNECT_COUNT__)
              state = mqtt_flush;
            else
            {
              state = mqtt_wait_reconnect;
              reconnect_timer = 2000;
            }
          }
        }
        break;

      case mqtt_pub_get:
        // get data from node
#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
        log_info("[%s]get mqtt data from list", __func__);
#else
        Serial.printf("[%s]get mqtt data from list", __func__); Serial.println();
#endif
#endif
        xSemaphoreTake(GSMData.sem_mqtt_data, portMAX_DELAY);
        SL_LAST(head_mqtt_data, item);

        memcpy(&GSMData.pub_data, item->memory, sizeof(at_mqtt_data));

        SL_DELETE(head_mqtt_data, item);
        xSemaphoreGive(GSMData.sem_mqtt_data);

#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
        log_info("[%s]free List pointer memory", __func__);
#else
        Serial.printf("[%s]free List pointer memory", __func__); Serial.println();
#endif
#endif

        free(item->memory);
        item->memory = NULL;
        free(item);
        item = NULL;

#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
        log_debug("[%s] Publish Data to Topic", __func__);
#else
        Serial.printf("[%s] Publish Data to Topic", __func__); Serial.println();
#endif
#endif
        pManager->curCMD = __ATCCLk_GET;
        send_to_at_manager(pManager->curCMD);

        xEventGroupSetBits(pManager->flag, FLAG_MQTT_TO_PUB);

        Counter0 = __PUBLISH_TIMEOUT__;

        state = mqtt_wait_publish;
        break;

      case mqtt_pub_free:
        SL_LENGTH(head_mqtt_data, length);

#if AT_MANAGER_3
#if __USE_WITH_ESP_IDF__
        log_info("[%s]item available in list %d", __func__, length);
#else
        Serial.printf("[%s]item available in list %d", __func__, length); Serial.println();
#endif
#endif
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
        log_info("[%s]free mqtt data pointer memory", __func__);
#else
        Serial.printf("[%s]free mqtt data pointer memory", __func__); Serial.println();
#endif
#endif
        if (GSMData.pub_data.data)
          free(GSMData.pub_data.data);

        if (GSMData.pub_data.topic)
          free(GSMData.pub_data.topic);

        state = mqtt_pub_idle;
        break;

      case mqtt_wait_publish:
        if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_PUBLISHED_OK)
        {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
          log_info("[%s]PUBLISH success, RST timer", __func__);
#else
          Serial.printf("[%s]PUBLISH success, RST timer", __func__); Serial.println();
#endif
#endif
          xEventGroupClearBits(pManager->flag, FLAG_MQTT_PUBLISHED_OK);

          if (xTimerReset(xTimersAtExpire, 10) != pdPASS)
          {
            #if __USE_WITH_ESP_IDF__
            log_error("[%s] Unable to reset timer", __func__);
#else
            Serial.printf("[%s] Unable to reset timer", __func__); Serial.println();
#endif
          }


          state = mqtt_pub_free;
          Counter0 = 0;
          reconnect_attempt = 0;
        }
        else if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_PUBLISHED_FAIL)
        {

          xEventGroupClearBits(pManager->flag, FLAG_MQTT_PUBLISHED_FAIL);

          if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_CONNECTED)
          {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
            log_error("[%s]mqtt attempt [%d} \n\n", __func__, reconnect_attempt);
#else
            Serial.printf("[%s]mqtt attempt [%d} \n\n", __func__, reconnect_attempt); Serial.println();
#endif
#endif
            if (reconnect_attempt > __RECONNECT_COUNT__)
            {
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
              log_error("[%s]mqtt publish failed [%d} \n\n", __func__, reconnect_attempt);
#else
              Serial.printf("[%s]mqtt publish failed [%d} \n\n", __func__, reconnect_attempt); Serial.println();
#endif
#endif
              state = mqtt_pub_free;
            }
            else
            {
              pManager->curCMD = __ATCMQTTTOPIC_SET;
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
              log_debug("[%s] Retry to Publish Topic", __func__);
#else
              Serial.printf("[%s] Retry to Publish Topic", __func__); Serial.println();
#endif
#endif
              send_to_at_manager(pManager->curCMD);
              reconnect_attempt++;
              Counter0 = __PUBLISH_TIMEOUT__;
              state = mqtt_wait_publish;
            }
          }
          else
          {
            pManager->curCMD = __ATCMQTTCONNECT;
            send_to_at_manager(pManager->curCMD);
#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
            log_debug("[%s] Retry to Connect server than publish Topic", __func__);
#else
            Serial.printf("[%s] Retry to Connect server than publish Topic", __func__); Serial.println();
#endif
#endif
            state = mqtt_wait_publishErr;
            reconnect_timer = 1000;
          }
        }
        break;
      case mqtt_wait_publishErr:

        vTaskDelay(pdMS_TO_TICKS(reconnect_timer)); // wait for xs for reconnect
        if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_CONNECTED)
        {
          if (reconnect_attempt > __RECONNECT_COUNT__)
          {
            state = mqtt_pub_free;
          }
          else
          {
            pManager->curCMD = __ATCMQTTTOPIC_SET;
            send_to_at_manager(pManager->curCMD); // re publish
            state = mqtt_wait_publish;
          }
        }
        else
        {
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
          log_debug("[%s]Publish err Reconnect", __func__);
#else
          Serial.printf("[%s]Publish err Reconnect", __func__); Serial.println();
#endif
#endif

          if (xEventGroupGetBits(pManager->flag) & FLAG_MQTT_STARTED)
            pManager->curCMD = __ATCMQTTCONNECT;
          else
            pManager->curCMD = __ATCMQTTSTART;

          send_to_at_manager(pManager->curCMD);
          // no need to update state as this is same state to return
        }
        break;

      case mqtt_wait_reconnect:
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
        log_debug("[%s]Wait Reconnect for %d (s)", __func__, reconnect_timer / 1000);
#else
        Serial.printf("[%s]Wait Reconnect for %d (s)", __func__, reconnect_timer / 1000); Serial.println();
#endif
#endif
        vTaskDelay(pdMS_TO_TICKS(reconnect_timer)); // wait for xs for reconnect
        state = mqtt_pub_idle;						// update state
        break;

      case mqtt_timeout:
        state = mqtt_pub_free;
        break;

      case mqtt_flush:
        state = mqtt_pub_idle;

#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
        log_info("[%s]delete all list data", __func__);
#else
        Serial.printf("[%s]delete all list data", __func__); Serial.println();
#endif
#endif
        do
        {
          xSemaphoreTake(GSMData.sem_mqtt_data, portMAX_DELAY);
          SL_LAST(head_mqtt_data, item);

          memcpy(&GSMData.pub_data, item->memory, sizeof(at_mqtt_data));

          SL_DELETE(head_mqtt_data, item);

          xSemaphoreGive(GSMData.sem_mqtt_data);

          free(item->memory);
          item->memory = NULL;
          free(item);
          item = NULL;

          if (GSMData.pub_data.data)
            free(GSMData.pub_data.data);

          if (GSMData.pub_data.topic)
            free(GSMData.pub_data.topic);

          SL_LENGTH(head_mqtt_data, length);

#if AT_MANAGER_2
#if __USE_WITH_ESP_IDF__
          log_info("[%s]item available in list %d", __func__, length);
#else
          Serial.printf("[%s]item available in list %d", __func__, length); Serial.println();
#endif
#endif
        } while (length > 0);
        break;
    } // switch state

    if (Counter0)
    {
      --Counter0;
      if (Counter0 == 0)
        state = mqtt_timeout;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  } // while 1
}

#if __STATIC_DEBUG__
void thread_test_mqtt( void * arg)
{
  uint32_t counter0 = 0;
  vTaskDelay(pdMS_TO_TICKS(1000 * 40));
  while (1)
  {
    do
    {
      counter0++;

      send_mqtt_data_AT((char *)"S17/sen", (char *)mqtt_tmp, strlen(mqtt_tmp));
      vTaskDelay(pdMS_TO_TICKS(2000));

    } while (counter0 < 10);

    vTaskDelay(pdMS_TO_TICKS(1000 * 60));
  }
}
#endif

void init_at_manager()
{
  #if __USE_WITH_ESP_IDF__
  log_info("[%s] Init GSM AT Command Manager", __func__);
#else
  Serial.printf("[%s] Init GSM AT Command Manager", __func__); Serial.println();
#endif

  xQueue_at_sender = xQueueCreate(16, sizeof(uint32_t));
  xQueue_at_receiver = xQueueCreate(8, sizeof(uint32_t));

  GSMData.xQueue_mqtt_pub = xQueueCreate(32, sizeof(uint32_t));
  ATManager.flag = xEventGroupCreate();
  /* Was the event group created successfully? */
  if (ATManager.flag == NULL)
  {
    #if __USE_WITH_ESP_IDF__
    log_error("[%s] Flah not created", __func__);
#else
    Serial.printf("[%s] Flah not created", __func__); Serial.println();
#endif
    while (1)
      ;
  }
#if AT_MANAGER_4
#if __USE_WITH_ESP_IDF__
  log_debug("[%s] Flag memory = 0x%X", __func__, ATManager.flag);
#else
  Serial.printf("[%s] Flag memory = 0x%X", __func__, ATManager.flag); Serial.println();
#endif
#endif
  GSMData.sem_mqtt_data = xSemaphoreCreateMutex();
  ATManager.curCMD = __AT;

  send_to_at_manager(ATManager.curCMD);
  xEventGroupSetBits(ATManager.flag, FLAG_MQTT_POWERON);
  xEventGroupClearBits(ATManager.flag, FLAG_MQTT_CONNECTED);

  const esp_timer_create_args_t oneshot_timer_args = {
    .callback = &oneshot_timer_callback,
    /* argument specified here will be passed to timer callback function */
    // .arg = (void*) periodic_timer,
    .name = "one-shot"
  };

  ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));

  xTaskCreate(thread_mqtt_publisher, "mqtt_pub", 4096, NULL, tskIDLE_PRIORITY + 7, NULL);
#if __STATIC_DEBUG__
  xTaskCreate(thread_test_mqtt, "testMQTT", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
#endif

  xTaskCreate(thread_sender, "atSender", 4096, NULL, tskIDLE_PRIORITY + 4, NULL);

  xTaskCreate(thread_receiver, "atReciever", 8192, NULL, tskIDLE_PRIORITY + 4, NULL);

#if __USE_WITH_ESP_IDF__
  log_info("[%s]Set Rst timer value %d\n", __func__, 1000 * 60 * __RST_ST_CMD__ );
#else
  Serial.printf("[%s]Set Rst timer value %d\n", __func__, 1000 * 60 * __RST_ST_CMD__ ); Serial.println();
#endif

  xTimersAtExpire = xTimerCreate(/* Just a text name, not used by the RTOS
								   kernel. */
                      "atExpiry",
                      /* The timer period in ticks, must be
                        greater than 0. */
                      pdMS_TO_TICKS(1000 * 60 * __RST_ST_CMD__ ),
                      /* The timers will auto-reload themselves
                        when they expire. */
                      pdFALSE,
                      /* The ID is used to store a count of the
                        number of times the timer has expired, which
                        is initialised to 0. */
                      (void *)0,
                      /* Each timer calls the same callback when
                        it expires. */
                      vTimerCallbackExpiredAT);
  if (xTimersAtExpire == NULL)
  {
    /* The timer was not created. */
#if __USE_WITH_ESP_IDF__    
    log_error("[%s] TImer not created", __func__);
#else
    Serial.printf("[%s] TImer not created", __func__); Serial.println();
#endif
  }
  else
  {
    /* Start the timer.  No block time is specified, and
      even if one was it would be ignored because the RTOS
      scheduler has not yet been started. */
    if (xTimerStart(xTimersAtExpire, 0) != pdPASS)
    {
      /* The timer could not be set into the Active
        state. */
#if __USE_WITH_ESP_IDF__        
      log_error("[%s] TImer not started", __func__);
#else
      Serial.printf("[%s] TImer not started", __func__); Serial.println();
#endif
    }
  }
}
