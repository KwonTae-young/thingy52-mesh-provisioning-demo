/* Copyright (c) 2010 - 2019, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.F
*
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <string.h>

/* HAL */
#include "boards.h"
#include "simple_hal_thingy.h"
#include "app_timer.h"

/* Core */
#include "nrf_mesh_config_core.h"
#include "nrf_mesh_gatt.h"
#include "nrf_mesh_configure.h"
#include "nrf_mesh.h"
#include "mesh_stack.h"
#include "device_state_manager.h"
#include "access_config.h"
#include "proxy.h"
#include "m_ui.h"

/* Provisioning and configuration */
#include "my_mesh_provisionee.h"
#include "mesh_app_utils.h"

/* Models */
#include "generic_onoff_server.h"
#include "generic_onoff_client.h"

/* Logging and RTT */
#include "log.h"
#include "rtt_input.h"

/* Example specific includes */
#include "app_config.h"
#include "example_common.h"
#include "nrf_mesh_config_examples.h"
#include "light_switch_example_common.h"
#include "app_onoff.h"
#include "ble_softdevice_support.h"
#include "pca20020.h"
#include "drv_ext_light.h"
#include "drv_ext_gpio.h"
#include "nrf_delay.h"
#include "enocean.h"
#include "flash_helper.h"
#include "enocean_switch_example.h"
#include "nrf_mesh_events.h"
/* Configure switch debounce timeout for EnOcean switch here */
#define SWITCH_DEBOUNCE_INTERVAL_US (MS_TO_US(500))
/* Two EnOcean switches can be used in parallel */
#define MAX_ENOCEAN_DEVICES_SUPPORTED (2)
#define ONOFF_SERVER_0_LED          (BSP_LED_0)
#define APP_ONOFF_ELEMENT_INDEX     (0)
#define THINGY_BUTTON 11
#define BUTTON_PIN_CONFIG ((GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos)     | \
                           (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos)    | \
                           (BUTTON_PULL << GPIO_PIN_CNF_PULL_Pos)                 | \
                           (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) | \
                           (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos))
#define APP_UNACK_MSG_REPEAT_COUNT   (2)
#define GPIOTE_IRQ_LEVEL NRF_MESH_IRQ_PRIORITY_LOWEST   
static uint32_t m_last_button_press;
static const nrf_drv_twi_t     m_twi_sensors = NRF_DRV_TWI_INSTANCE(TWI_SENSOR_INSTANCE);
static bool m_device_provisioned;
uint32_t hal_buttons_init(void);
static bool m_on_off_button_flag= 0;
static void app_mesh_core_event_cb (const nrf_mesh_evt_t * p_evt);
static void app_enocean_start(void);

static nrf_mesh_evt_handler_t m_mesh_core_event_handler = { .evt_cb = app_mesh_core_event_cb };
/*************************************************************************************************/
static void app_onoff_server_set_cb(const app_onoff_server_t * p_server, bool onoff);
static void app_onoff_server_get_cb(const app_onoff_server_t * p_server, bool * p_present_onoff);
static generic_onoff_client_t m_client;
/* Generic OnOff server structure definition and initialization */
APP_ONOFF_SERVER_DEF(m_onoff_server_0,
                     APP_CONFIG_FORCE_SEGMENTATION,
                     APP_CONFIG_MIC_SIZE,
                     app_onoff_server_set_cb,
                     app_onoff_server_get_cb)

static void app_gen_onoff_client_publish_interval_cb(access_model_handle_t handle, void * p_self);
static void app_generic_onoff_client_status_cb(const generic_onoff_client_t * p_self,
                                               const access_message_rx_meta_t * p_meta,
                                               const generic_onoff_status_params_t * p_in);
static void app_gen_onoff_client_transaction_status_cb(access_model_handle_t model_handle,
                                                       void * p_args,
                                                       access_reliable_status_t status);

const generic_onoff_client_callbacks_t client_cbs =
{
    .onoff_status_cb = app_generic_onoff_client_status_cb,
    .ack_transaction_status_cb = app_gen_onoff_client_transaction_status_cb,
    .periodic_publish_cb = app_gen_onoff_client_publish_interval_cb
};

typedef struct
{
    uint32_t a0_ts;
    uint32_t a1_ts;
    uint32_t b0_ts;
    uint32_t b1_ts;
} app_switch_debounce_state_t;
static app_secmat_flash_t m_app_secmat_flash[MAX_ENOCEAN_DEVICES_SUPPORTED];
static enocean_commissioning_secmat_t m_app_secmat[MAX_ENOCEAN_DEVICES_SUPPORTED];
static uint8_t  m_enocean_dev_cnt;
static app_switch_debounce_state_t m_switch_state[MAX_ENOCEAN_DEVICES_SUPPORTED];
/* Try to store app data. If busy, ask user to manually initiate operation. */
static void app_data_store_try(void)
{
    uint32_t status = app_flash_data_store(APP_DATA_ENTRY_HANDLE, &m_app_secmat_flash[0], sizeof(m_app_secmat_flash));
    if (status == NRF_SUCCESS)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Storing: Enocean security material\n");
    }
    else if (status == NRF_ERROR_NOT_SUPPORTED)
    {
       __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Cannot store: Persistent storage not enabled\n");
    }
    else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Flash busy. Cannot store. Press Button 2 to try again. \n");
    }
}

static uint8_t enocean_device_index_get(enocean_evt_t * p_evt)
{
    uint8_t i;
    for (i = 0; i < m_enocean_dev_cnt; i++)
    {
        if (memcmp(p_evt->p_ble_gap_addr, m_app_secmat[i].p_ble_gap_addr, BLE_GAP_ADDR_LEN) == 0)
        {
            break;
        }
    }

    /* This should never assert. */
    if (i == m_enocean_dev_cnt)
    {
        APP_ERROR_CHECK(false);
    }

    return i;
}

/* Forward the ADV packets to the Enocean module */
static void rx_callback(const nrf_mesh_adv_packet_rx_data_t * p_rx_data)
{
    enocean_packet_process(p_rx_data);
}

/* This example translates the messages from the PTM215B switches to on/off client model messages.
The mapping of the switches and corresponding client model messages is as follows:

Pressing Switch 1 will turn ON LED 1 on the servers with ODD addresses.
Pressing Switch 2 will turn OFF LED 1 on the servers with ODD addresses.
Pressing Switch 3 will turn ON LED 1 on the servers with EVEN addresses.
Pressing Switch 4 will turn OFF LED 1 on the servers with EVEN addresses.
*/
static void app_enocean_cb(enocean_evt_t * p_evt)
{
    if  (p_evt->type == ENOCEAN_EVT_DATA_RECEIVED)
    {
        uint32_t status = NRF_SUCCESS;
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Sw data A0: %d A1: %d B0: %d B1: %d Action: %d\n",
              p_evt->params.data.status.a0,p_evt->params.data.status.a1,p_evt->params.data.status.b0,
              p_evt->params.data.status.b1,p_evt->params.data.status.action);
        //Simplified

        generic_onoff_set_params_t set_params;
        model_transition_t transition_params;
        static uint8_t tid = 0;
        set_params.tid = tid++;
        transition_params.delay_ms = 50;
        transition_params.transition_time_ms = 100;
        if (p_evt->params.data.status.action==1)
              {
                  //There are 4 buttons on the Enocean switch, when release all of them are 0.        
                  if ((p_evt->params.data.status.a1,p_evt->params.data.status.b0==1)||(p_evt->params.data.status.a1,p_evt->params.data.status.b1==1))
                  {
                      set_params.on_off=p_evt->params.data.status.b0;
                      status = generic_onoff_client_set_unack(&m_client,&set_params,
                                                    &transition_params,APP_UNACK_MSG_REPEAT_COUNT);
                  }
                  else
                  {
                      set_params.on_off=p_evt->params.data.status.a0;
                      status = generic_onoff_client_set_unack(&m_client,&set_params,
                                                    &transition_params, APP_UNACK_MSG_REPEAT_COUNT);
                  }

              }
    }
    else if (p_evt->type == ENOCEAN_EVT_SECURITY_MATERIAL_RECEIVED)
    {
        if (m_enocean_dev_cnt < MAX_ENOCEAN_DEVICES_SUPPORTED)
        {
            m_app_secmat_flash[m_enocean_dev_cnt].seq = p_evt->params.secmat.seq;
            memcpy(&m_app_secmat_flash[m_enocean_dev_cnt].ble_gap_addr[0],
                   p_evt->p_ble_gap_addr, BLE_GAP_ADDR_LEN);
            memcpy(&m_app_secmat_flash[m_enocean_dev_cnt].key[0],
                   p_evt->params.secmat.p_key, PTM215B_COMM_PACKET_KEY_SIZE);

            app_data_store_try();

            m_app_secmat[m_enocean_dev_cnt].p_seq =  &m_app_secmat_flash[m_enocean_dev_cnt].seq;
            m_app_secmat[m_enocean_dev_cnt].p_ble_gap_addr = &m_app_secmat_flash[m_enocean_dev_cnt].ble_gap_addr[0];
            m_app_secmat[m_enocean_dev_cnt].p_key = &m_app_secmat_flash[m_enocean_dev_cnt].key[0];
            enocean_secmat_add(&m_app_secmat[m_enocean_dev_cnt]);

            m_enocean_dev_cnt++;

            hal_led_blink_ms( LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_PROV);
        }
        else
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Cannot add new device. Max number of supported devices: %d\n", MAX_ENOCEAN_DEVICES_SUPPORTED);
        }
    }
}

static void app_enocean_start(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Starting application \n");

    /* Load app specific data */
    if (app_flash_data_load(APP_DATA_ENTRY_HANDLE, &m_app_secmat_flash[0], sizeof(m_app_secmat_flash)) == NRF_SUCCESS)
    {
        for (uint8_t i = 0; i < MAX_ENOCEAN_DEVICES_SUPPORTED; i++)
        {
            m_app_secmat[i].p_ble_gap_addr = &m_app_secmat_flash[i].ble_gap_addr[0];
            m_app_secmat[i].p_key = &m_app_secmat_flash[i].key[0];
            m_app_secmat[i].p_seq = &m_app_secmat_flash[i].seq;
            m_enocean_dev_cnt++;
            enocean_secmat_add(&m_app_secmat[i]);
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Restored: Enocean security materials\n");
        }
    }
    else
    {
        m_enocean_dev_cnt = 0;
    }

    /* Install rx callback to intercept incoming ADV packets so that they can be passed to the
    EnOcean packet processor */
    nrf_mesh_rx_cb_set(rx_callback);
}
static void app_mesh_core_event_cb(const nrf_mesh_evt_t * p_evt)
{
    /* USER_NOTE: User can insert mesh core event proceesing here */
    switch (p_evt->type)
    {
        /* Start user application specific functions only when flash is stable */
        case NRF_MESH_EVT_FLASH_STABLE:
            __LOG(LOG_SRC_APP, LOG_LEVEL_DBG1, "Mesh evt: FLASH_STABLE \n");
            {
                static bool s_app_started;
                if (!s_app_started)
                {
                    /* Flash operation initiated during initialization has been completed */
                    app_enocean_start();
                    s_app_started = true;
                }
            }
            break;

        default:
            break;
    }
}

/* Callback for updating the hardware state */
static void app_onoff_server_set_cb(const app_onoff_server_t * p_server, bool onoff)
{
    /* Resolve the server instance here if required, this example uses only 1 instance. */

    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Setting GPIO value: %d\n", onoff)
    hal_led_pin_set(onoff);
    
}

/* Callback for reading the hardware state */
static void app_onoff_server_get_cb(const app_onoff_server_t * p_server, bool * p_present_onoff)
{
    /* Resolve the server instance here if required, this example uses only 1 instance. */

    *p_present_onoff = hal_led_pin_get();
}

static void app_model_init(void)
{
    /* Instantiate onoff server on element index APP_ONOFF_ELEMENT_INDEX */
    ERROR_CHECK(app_onoff_init(&m_onoff_server_0, APP_ONOFF_ELEMENT_INDEX));
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "App OnOff Model Handle: %d\n", m_onoff_server_0.server.model_handle);
}

/* This callback is called periodically if model is configured for periodic publishing */
static void app_gen_onoff_client_publish_interval_cb(access_model_handle_t handle, void * p_self)
{
     __LOG(LOG_SRC_APP, LOG_LEVEL_WARN, "Publish desired message here.\n");
}

/* Acknowledged transaction status callback, if acknowledged transfer fails, application can
* determine suitable course of action (e.g. re-initiate previous transaction) by using this
* callback.
*/
static void app_gen_onoff_client_transaction_status_cb(access_model_handle_t model_handle,
                                                       void * p_args,
                                                       access_reliable_status_t status)
{
    switch(status)
    {
        case ACCESS_RELIABLE_TRANSFER_SUCCESS:
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Acknowledged transfer success.\n");
            break;

        case ACCESS_RELIABLE_TRANSFER_TIMEOUT:
            
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Acknowledged transfer timeout.\n");
            break;

        case ACCESS_RELIABLE_TRANSFER_CANCELLED:
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Acknowledged transfer cancelled.\n");
            break;

        default:
            ERROR_CHECK(NRF_ERROR_INTERNAL);
            break;
    }
}

/* Generic OnOff client model interface: Process the received status message in this callback */
static void app_generic_onoff_client_status_cb(const generic_onoff_client_t * p_self,
                                               const access_message_rx_meta_t * p_meta,
                                               const generic_onoff_status_params_t * p_in)
{
    if (p_in->remaining_time_ms > 0)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "OnOff server: 0x%04x, Present OnOff: %d, Target OnOff: %d, Remaining Time: %d ms\n",
              p_meta->src.value, p_in->present_on_off, p_in->target_on_off, p_in->remaining_time_ms);
    }
    else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "OnOff server: 0x%04x, Present OnOff: %d\n",
              p_meta->src.value, p_in->present_on_off);
    }
}
/*************************************************************************************************/

static void node_reset(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- Node reset  -----\n");
    hal_led_blink_ms(LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_RESET);
    /* This function may return if there are ongoing flash operations. */
    mesh_stack_device_reset();
}

static void config_server_evt_cb(const config_server_evt_t * p_evt)
{
    if (p_evt->type == CONFIG_SERVER_EVT_NODE_RESET)
    {
        node_reset();
    }
}

static void button_event_handler(uint32_t button_number)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Button %u pressed\n", button_number);
    uint32_t status = NRF_SUCCESS;
    generic_onoff_set_params_t set_params;
    model_transition_t transition_params;
    static uint8_t tid = 0;
    set_params.tid = tid++;
    transition_params.delay_ms = 50;
    transition_params.transition_time_ms = 100;
    switch (button_number)
    {
        /* Pressing SW1 on the Development Kit will result in LED state to toggle and trigger
        the STATUS message to inform client about the state change. This is a demonstration of
        state change publication due to local event. */
        case 0:
        case 1:
        {
             m_on_off_button_flag=!m_on_off_button_flag; 
             set_params.on_off=m_on_off_button_flag;
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Sending msg: ONOFF SET %d\n", set_params.on_off);
            status = generic_onoff_client_set_unack(&m_client, &set_params,
                                                    &transition_params, APP_UNACK_MSG_REPEAT_COUNT);
        }
        default:
            break;
    }
}

static void app_rtt_input_handler(int key)
{
    if (key >= '0' && key <= '4')
    {
        uint32_t button_number = key - '0';
        button_event_handler(button_number);
    }
}

static void device_identification_start_cb(uint8_t attention_duration_s)
{
    hal_led_pin_set(false);
    hal_led_blink_ms(LED_BLINK_ATTENTION_INTERVAL_MS,
                     LED_BLINK_ATTENTION_COUNT(3));

}

static void provisioning_aborted_cb(void)
{
    hal_led_blink_stop();
}

static void provisioning_complete_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Successfully provisioned\n");

#if MESH_FEATURE_GATT_ENABLED
    /* Restores the application parameters after switching from the Provisioning
     * service to the Proxy  */
    gap_params_init();
    conn_params_init();
#endif

    dsm_local_unicast_address_t node_address;
    dsm_local_unicast_addresses_get(&node_address);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Node Address: 0x%04x \n", node_address.address_start);

    hal_led_blink_stop();
    hal_led_pin_set(0);
    hal_led_blink_ms(LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_PROV);
}

static void models_init_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Initializing and adding models\n");
    app_model_init();

    m_client.settings.p_callbacks = &client_cbs;
    m_client.settings.timeout = 0;
    m_client.settings.force_segmented = APP_CONFIG_FORCE_SEGMENTATION;
    m_client.settings.transmic_size = APP_CONFIG_MIC_SIZE;
    ERROR_CHECK(generic_onoff_client_init(&m_client,  APP_ONOFF_ELEMENT_INDEX+1));
}
static void board_init(void)
{
    uint32_t            err_code;
    drv_ext_gpio_init_t ext_gpio_init;


    static const nrf_drv_twi_config_t twi_config =
    {
        .scl                = TWI_SCL,
        .sda                = TWI_SDA,
        .frequency          = NRF_TWI_FREQ_400K,
        .interrupt_priority = APP_IRQ_PRIORITY_LOW
    };

    static const drv_sx1509_cfg_t sx1509_cfg =
    {
        .twi_addr       = SX1509_ADDR,
        .p_twi_instance = &m_twi_sensors,
        .p_twi_cfg      = &twi_config
    };

    ext_gpio_init.p_cfg = &sx1509_cfg;
    
    err_code = support_func_configure_io_startup(&ext_gpio_init);
    APP_ERROR_CHECK(err_code);
}
uint32_t hal_buttons_init(void)
{
   
    NRF_GPIO->PIN_CNF[THINGY_BUTTON] = BUTTON_PIN_CONFIG;
   
    NRF_GPIOTE->INTENSET = GPIOTE_INTENSET_PORT_Msk;
    NRF_GPIOTE->EVENTS_PORT  = 0;

    NVIC_SetPriority(GPIOTE_IRQn, GPIOTE_IRQ_LEVEL);
    NVIC_EnableIRQ(GPIOTE_IRQn);
    return NRF_SUCCESS;

}
void GPIOTE_IRQHandler(void)
{
    NRF_GPIOTE->EVENTS_PORT = 0;
   
        /* Check that the event was generated by a button press, and reject if it's too soon (debounce).
         * NOTE: There is a bug with this at the wrap-around for the RTC0 where the button could be
         * pressed before HAL_BUTTON_PRESS_FREQUENCY has passed a single time. It doesn't matter practically.
         */
 
   if( TIMER_DIFF(m_last_button_press, NRF_RTC0->COUNTER) > HAL_BUTTON_PRESS_FREQUENCY)
    {
      __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Buttonpressed\n");
        m_last_button_press = NRF_RTC0->COUNTER;
        button_event_handler(0);
    }
    
}

uint32_t m_my_ui_init( void)
{
    uint32_t                        err_code;
    static drv_sx1509_cfg_t         sx1509_cfg;
    drv_ext_light_init_t            led_init;
    //lint --e{651} Potentially confusing initializer
    static const drv_ext_light_conf_t led_conf[DRV_EXT_LIGHT_NUM] = DRV_EXT_LIGHT_CFG;

    static const nrf_drv_twi_config_t twi_config =
    {
        .scl                = TWI_SCL,
        .sda                = TWI_SDA,
        .frequency          = NRF_TWI_FREQ_100K,
        .interrupt_priority = APP_IRQ_PRIORITY_LOW
    };
    sx1509_cfg.twi_addr       = SX1509_ADDR;
    sx1509_cfg.p_twi_instance = &m_twi_sensors;
    sx1509_cfg.p_twi_cfg      = &twi_config;



    led_init.p_light_conf        = led_conf;
    led_init.num_lights          = DRV_EXT_LIGHT_NUM;
    led_init.clkx_div            = DRV_EXT_LIGHT_CLKX_DIV_8;
    led_init.p_twi_conf          = &sx1509_cfg;
    led_init.resync_pin          = SX_RESET;

    err_code = drv_ext_light_init(&led_init, false);
    APP_ERROR_CHECK(err_code);
    ERROR_CHECK(drv_ext_light_off(DRV_EXT_RGB_LED_SENSE));
    ERROR_CHECK(drv_ext_light_off(DRV_EXT_RGB_LED_LIGHTWELL));
    nrf_gpio_cfg_output(MOS_1);
    nrf_gpio_cfg_output(MOS_2);
    nrf_gpio_cfg_output(MOS_3);
    nrf_gpio_cfg_output(MOS_4);
    nrf_gpio_pin_clear(MOS_1);
    nrf_gpio_pin_clear(MOS_2);
    nrf_gpio_pin_clear(MOS_3);
    nrf_gpio_pin_clear(MOS_4);
    
 


    return NRF_SUCCESS;
}
static void provisioning_blink_output_cb(uint8_t * number)

{   uint32_t err_code;
    ERROR_CHECK(drv_ext_light_off(1));
     __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Blink OOB %u\n", number[15]);
     //The OOB data only use last byte to set the number of blink
     //Start the blinking timer
    hal_led_blink_stop();
    hal_led_pin_set(0);
    nrf_delay_ms(300);
    hal_led_blink_ms(500,number[15]);
    APP_ERROR_CHECK(err_code);
}
static void mesh_init(void)
{
    mesh_stack_init_params_t init_params =
    {
        .core.irq_priority       = NRF_MESH_IRQ_PRIORITY_LOWEST,
        .core.lfclksrc           = DEV_BOARD_LF_CLK_CFG,
        .core.p_uuid             = NULL,
        .models.models_init_cb   = models_init_cb,
        .models.config_server_cb = config_server_evt_cb
    };
    ERROR_CHECK(mesh_stack_init(&init_params, &m_device_provisioned));
    /* Register event handler to receive NRF_MESH_EVT_FLASH_STABLE. Application functionality will
    be started after this event */
    nrf_mesh_evt_handler_add(&m_mesh_core_event_handler);
}

static void initialize(void)
{
    __LOG_INIT(LOG_SRC_APP | LOG_SRC_FRIEND, LOG_LEVEL_DBG1, LOG_CALLBACK_DEFAULT);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- Thingy Provisioning Demo -----\n");

    ERROR_CHECK(app_timer_init());
    hal_leds_init();
    ble_stack_init();
#if MESH_FEATURE_GATT_ENABLED
    gap_params_init();
    conn_params_init();
#endif
    board_init();
    mesh_init();
    m_my_ui_init();
    app_flash_init();
    enocean_translator_init(app_enocean_cb);

    
}

static void start(void)
{
    rtt_input_enable(app_rtt_input_handler, RTT_INPUT_POLL_PERIOD_MS);
    nrf_gpio_cfg_input(THINGY_BUTTON,GPIO_PIN_CNF_PULL_Pullup);
    if (!m_device_provisioned)
    {
        static const uint8_t static_auth_data[NRF_MESH_KEY_SIZE] = STATIC_AUTH_DATA;
        mesh_provisionee_start_params_t prov_start_params =
        {
            .p_static_data    = static_auth_data,
            .prov_complete_cb = provisioning_complete_cb,
            .prov_device_identification_start_cb = device_identification_start_cb,
            .prov_device_identification_stop_cb = NULL,
            .auth_output_cb = provisioning_blink_output_cb, 
            .prov_abort_cb = provisioning_aborted_cb,
            .p_device_uri = EX_URI_LS_SERVER
        };
        ERROR_CHECK(mesh_provisionee_prov_start(&prov_start_params));
        led_breath_red();

    }
    else
    {  
        //erase bond information if button pressed when reset
        if (nrf_gpio_pin_read(THINGY_BUTTON)==0)
        {
 
#if MESH_FEATURE_GATT_PROXY_ENABLED
            (void) proxy_stop();
#endif
            mesh_stack_config_clear();
            nrf_delay_ms(500);
            node_reset();
        }
      hal_led_blink_ms(100,2);
    }
   
    mesh_app_uuid_print(nrf_mesh_configure_device_uuid_get());

    ERROR_CHECK(mesh_stack_start());

   /* hal_led_pin_set(0);
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_START);*/
}



int main(void)
{
    initialize();
    start();
   hal_buttons_init();
    for (;;)
    {
        (void)sd_app_evt_wait();
    }
}