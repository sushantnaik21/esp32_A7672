
#ifndef __AT_MANAGER_H__
#define __AT_MANAGER_H__

#define __USE_WITH_ESP_IDF__  0

#define __USE_WITH_ARDUINO__       1

#if ( __USE_WITH_ARDUINO__ && __USE_WITH_ESP_IDF__ )
  #error AT_Manager.h Either select one to compile log
#endif

//!< @brief	disable or enable test mode for AT command , test without 485
#define __STATIC_DEBUG__ 		1

#define __AIRTEL_APN__  "airtelgprs.com"

#define __RST_ST_CMD__    5 // reset time in min

void thread_sender();
void init_at_manager();
void thread_receiver();

uint32_t serial2_receive_cb (uint8_t *arg, uint32_t length);

void send_mqtt_data_AT(char *topic, char * source, uint32_t len);

time_t get_current_gsm_ts();

char * get_gsm_ip();

uint32_t get_sim_rssi_at();

uint32_t get_mqtt_status_at();

void thread_sync_time_gsm();
#endif 
