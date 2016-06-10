/***********************************************************************************
Copyright (c) Nordic Semiconductor ASA
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of other
  contributors to this software may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************/

#include <string.h>
#include "bootloader_app.h"
#include "bootloader_util.h"
#include "timeslot.h"
#include "rbc_mesh.h"
#include "dfu_types_mesh.h"
#include "toolchain.h"
#include "bl_if.h"
#include "nrf_flash.h"
#include "mesh_packet.h"
#include "rbc_mesh_common.h"
#include "timer_scheduler.h"
#include "mesh_flash.h"
#include "rand.h"
#include "transport_control.h"
#include "app_error.h"

extern uint32_t rbc_mesh_event_push(rbc_mesh_event_t* p_event);

/*****************************************************************************
* Local defines
*****************************************************************************/
#define IRQ_ENABLED                 (0x01)      /**< Field that identifies if an interrupt is enabled. */
#define MAX_NUMBER_INTERRUPTS       (32)        /**< Maximum number of interrupts available. */

#define DFU_TX_SLOTS                (8)         /**< Number of concurrent transmits available. */
#define DFU_TX_INTERVAL_US          (100000)    /**< Time between transmits on regular interval, and base-interval on exponential. */
#define DFU_TX_START_DELAY_MASK_US  (0xFFFF)    /**< Must be power of two. */
#define DFU_TX_TIMER_MARGIN_US      (1000)      /**< Time margin for a timeout to be considered instant. */

#define TIMER_REQ_TIMEOUT           (100000000) /**< Time to wait before giving up on an ongoing request. */
#define TIMER_START_TIMEOUT         ( 50000000) /**< Time to wait for first data during a transfer. */
#define TIMER_DATA_TIMEOUT          ( 10000000) /**< Time to wait for next data during a transfer. */
/*****************************************************************************
* Local typedefs
*****************************************************************************/

typedef struct
{
    mesh_packet_t* p_packet;
    uint32_t order_time;
    bl_radio_interval_type_t interval_type;
    uint8_t repeats;
    uint8_t tx_count;
} dfu_tx_t;
/*****************************************************************************
* Static globals
*****************************************************************************/
static bl_if_cmd_handler_t          m_cmd_handler = NULL;           /**< Command handler in shared code space */
static timer_event_t                m_timer_evt;                    /**< Timer event for scheduler. */
static timer_event_t                m_tx_timer_evt;                 /**< TX event for scheduler. */
static bool                         m_tx_scheduled;                 /**< Whether the TX event is scheduled. */
static dfu_tx_t                     m_tx_slots[DFU_TX_SLOTS];       /**< TX slots for concurrent transmits. */
static prng_t                       m_prng;                         /**< PRNG for time delays. */
static tc_tx_config_t               m_tx_config;
static fwid_t*                      mp_curr_fwid;
static dfu_transfer_state_t         m_transfer_state;               /**< State of the ongoing dfu transfer. */
/*****************************************************************************
* Static functions
*****************************************************************************/
static uint32_t get_curr_fwid(dfu_type_t type, fwid_union_t* p_fwid)
{
    switch (type)
    {
        case DFU_TYPE_SD:
            p_fwid->sd = mp_curr_fwid->sd;
            break;
        case DFU_TYPE_BOOTLOADER:
            p_fwid->bootloader = mp_curr_fwid->bootloader;
            break;
        case DFU_TYPE_APP:
            p_fwid->app = mp_curr_fwid->app;
            break;
        default:
            return NRF_ERROR_NOT_SUPPORTED;
    }
    return NRF_SUCCESS;
}

static uint32_t next_tx_timeout(dfu_tx_t* p_tx)
{
    if (p_tx->interval_type == BL_RADIO_INTERVAL_TYPE_EXPONENTIAL)
    {
        return (p_tx->order_time + DFU_TX_INTERVAL_US * ((1 << (p_tx->tx_count)) - 1));
    }
    else
    {
        return (p_tx->order_time + DFU_TX_INTERVAL_US * p_tx->tx_count);
    }
}

static void interrupts_disable(void)
{
    uint32_t interrupt_setting_mask;
    uint32_t irq;

    interrupt_setting_mask = NVIC->ISER[0];

    /* Loop through and disable all interrupts. */
    for (irq = 0; irq < MAX_NUMBER_INTERRUPTS; irq++)
    {
        if (interrupt_setting_mask & (IRQ_ENABLED << irq))
        {
            NVIC_DisableIRQ((IRQn_Type)irq);
        }
    }
}

static void tx_timeout(uint32_t timestamp, void* p_context)
{
    __LOG("TX Timeout @ %d\n", timestamp);
    uint32_t next_timeout = timestamp + (UINT32_MAX / 2);
    for (uint32_t i = 0; i < DFU_TX_SLOTS; ++i)
    {
        if (m_tx_slots[i].p_packet)
        {
            uint32_t timeout = next_tx_timeout(&m_tx_slots[i]);
            if (TIMER_OLDER_THAN(timeout, (timestamp + DFU_TX_TIMER_MARGIN_US)))
            {
                if (tc_tx(m_tx_slots[i].p_packet, &m_tx_config) == NRF_SUCCESS)
                {
                    __LOG("DFU TX\n");
                    m_tx_slots[i].tx_count++;

                    if (m_tx_slots[i].tx_count == TX_REPEATS_INF &&
                        m_tx_slots[i].repeats  == TX_REPEATS_INF)
                    {
                        m_tx_slots[i].order_time = timeout;
                        m_tx_slots[i].tx_count = 0;
                    }
                    else if (m_tx_slots[i].tx_count >= m_tx_slots[i].repeats)
                    {
                        mesh_packet_ref_count_dec(m_tx_slots[i].p_packet);
                        memset(&m_tx_slots[i], 0, sizeof(dfu_tx_t));
                    }
                    timeout = next_tx_timeout(&m_tx_slots[i]);
                }
            }
            if (TIMER_DIFF(timeout, timestamp) < TIMER_DIFF(next_timeout, timestamp))
            {
                next_timeout = timeout;
            }
        }
    }
    m_tx_timer_evt.timestamp = next_timeout;
    if (timer_sch_schedule(&m_tx_timer_evt) != NRF_SUCCESS)
    {
        m_tx_scheduled = true;
        APP_ERROR_CHECK(NRF_ERROR_NO_MEM);
    }
}

static void abort_timeout(uint32_t timestamp, void* p_context)
{
    NRF_GPIO->OUTCLR = (1 << 1);
    __LOG("ABORT Timeout fired @%d\n", timestamp);
    bl_cmd_t abort_cmd;
    abort_cmd.type = BL_CMD_TYPE_DFU_ABORT;
    m_cmd_handler(&abort_cmd);
}

static void timer_timeout(uint32_t timestamp, void* p_context)
{
    NRF_GPIO->OUTCLR = (1 << 1);
    __LOG("Timeout fired @%d\n", timestamp);
    bl_cmd_t timeout_cmd;
    timeout_cmd.type = BL_CMD_TYPE_TIMEOUT;
    timeout_cmd.params.timeout.timer_index = 0;
    m_cmd_handler(&timeout_cmd);
}

static void flash_op_complete(flash_op_type_t type, void* p_context)
{
    bl_cmd_t end_cmd;
    if (type == FLASH_OP_TYPE_WRITE)
    {
        end_cmd.type = BL_CMD_TYPE_FLASH_WRITE_COMPLETE;
        end_cmd.params.flash.write.p_data = p_context;
    }
    else
    {
        end_cmd.type = BL_CMD_TYPE_FLASH_ERASE_COMPLETE;
        end_cmd.params.flash.erase.p_dest = p_context;
    }
    bootloader_cmd_send(&end_cmd); /* don't care about the return code */
}

/*****************************************************************************
* Interface functions
*****************************************************************************/
uint32_t bootloader_start(dfu_type_t type, fwid_union_t* p_fwid)
{
    if (NRF_UICR->BOOTLOADERADDR != 0xFFFFFFFF)
    {
        interrupts_disable();

#ifdef SOFTDEVICE_PRESENT
        sd_power_reset_reason_clr(0x0F000F);
        sd_power_gpregret_set(RBC_MESH_GPREGRET_CODE_FORCED_REBOOT);
        sd_nvic_SystemReset();
#else
        NRF_POWER->RESETREAS = 0x0F000F; /* erase reset-reason to avoid wrongful state-readout on reboot */
        NRF_POWER->GPREGRET = RBC_MESH_GPREGRET_CODE_FORCED_REBOOT;
        NVIC_SystemReset();
#endif
        return NRF_SUCCESS; /* unreachable */
    }
    else
    {
        /* the UICR->BOOTLOADERADDR isn't set, and we have no way to find the bootloader-address. */
        return NRF_ERROR_FORBIDDEN;
    }
}

uint32_t bootloader_init(void)
{
    m_cmd_handler = *((bl_if_cmd_handler_t*) (0x20000000 + ((uint32_t) (NRF_FICR->SIZERAMBLOCKS * NRF_FICR->NUMRAMBLOCK) - 4)));
    if (m_cmd_handler == NULL ||
        (uint32_t) m_cmd_handler >= (NRF_FICR->CODESIZE * NRF_FICR->CODEPAGESIZE) ||
        (uint32_t) m_cmd_handler < NRF_UICR->BOOTLOADERADDR)
    {
        __LOG(RTT_CTRL_TEXT_RED "ERROR, command handler @0x%x\n" RTT_CTRL_TEXT_WHITE, m_cmd_handler);
        m_cmd_handler = NULL;
        return NRF_ERROR_NOT_SUPPORTED;
    }

    rand_prng_seed(&m_prng);

    m_timer_evt.cb           = timer_timeout;
    m_timer_evt.interval     = 0;
    m_timer_evt.p_context    = NULL;
    m_timer_evt.p_next       = NULL;
    m_tx_timer_evt.cb        = tx_timeout;
    m_tx_timer_evt.interval  = 0;
    m_tx_timer_evt.p_context = NULL;
    m_tx_timer_evt.p_next    = NULL;
    m_tx_scheduled           = true;

    m_tx_config.access_address = RBC_MESH_ACCESS_ADDRESS_BLE_ADV;
    m_tx_config.first_channel = 37;
    m_tx_config.channel_map = (1 << 0) | (1 << 1) | (1 << 2); /* 37, 38, 39 */

    mesh_flash_init(flash_op_complete);

    bl_cmd_t init_cmd =
    {
        .type = BL_CMD_TYPE_INIT,
        .params.init =
        {
            .bl_if_version = BL_IF_VERSION,
            .event_callback = bootloader_event_handler,
            .timer_count = 1,
            .tx_slots = DFU_TX_SLOTS
        }
    };

    uint32_t error_code = m_cmd_handler(&init_cmd);

    if (error_code != NRF_SUCCESS)
    {
        return error_code;
    }

    bl_cmd_t fwid_cmd;
    fwid_cmd.type = BL_CMD_TYPE_INFO_GET;
    fwid_cmd.params.info.get.type = BL_INFO_TYPE_VERSION;
    fwid_cmd.params.info.get.p_entry = NULL;
    error_code = m_cmd_handler(&fwid_cmd);
    if (error_code != NRF_SUCCESS)
    {
        /* no version info */
        return error_code;
    }

    mp_curr_fwid = &fwid_cmd.params.info.get.p_entry->version;
    return error_code;
}

//TODO DUPLICATE?
uint32_t bootloader_enable(void)
{
    if (m_cmd_handler == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    bl_cmd_t enable_cmd =
    {
        .type = BL_CMD_TYPE_ENABLE,
        .params = {{0}}
    };

    return m_cmd_handler(&enable_cmd);
}

uint32_t bootloader_event_handler(bl_evt_t* p_evt)
{
    switch (p_evt->type)
    {
        case BL_EVT_TYPE_ECHO:
            __LOG("Echo: %s\n", p_evt->params.echo.str);
            break;
        case BL_EVT_TYPE_DFU_ABORT:
            {
                __LOG("Abort event. Reason: 0x%x\n", p_evt->params.dfu.abort.reason);
                rbc_mesh_event_t evt;
                evt.type = RBC_MESH_EVENT_TYPE_DFU_END;
                evt.params.dfu.end.dfu_type = m_transfer_state.type;
                evt.params.dfu.end.role = m_transfer_state.role;
                evt.params.dfu.end.fwid = m_transfer_state.fwid;
                evt.params.dfu.end.end_reason = p_evt->params.dfu.abort.reason;
                memset(&m_transfer_state, 0, sizeof(dfu_transfer_state_t));
                rbc_mesh_event_push(&evt);
            }
            break;

        case BL_EVT_TYPE_DFU_NEW_FW:
            {
                rbc_mesh_event_t evt;
                evt.type = RBC_MESH_EVENT_TYPE_DFU_NEW_FW_AVAILABLE;
                evt.params.dfu.new_fw.dfu_type = p_evt->params.dfu.new_fw.fw_type;
                evt.params.dfu.new_fw.new_fwid = p_evt->params.dfu.new_fw.fwid;
                if (get_curr_fwid(
                            p_evt->params.dfu.new_fw.fw_type,
                            &evt.params.dfu.new_fw.current_fwid) == NRF_SUCCESS)
                {
                    rbc_mesh_event_push(&evt);
                }
            }
            break;

        case BL_EVT_TYPE_DFU_REQ:
            {
                /* Forward to application */
                rbc_mesh_event_t evt;
                switch (p_evt->params.dfu.req.role)
                {
                    case DFU_ROLE_RELAY:
                        evt.type = RBC_MESH_EVENT_TYPE_DFU_RELAY_REQ;
                        evt.params.dfu.relay_req.dfu_type = p_evt->params.dfu.req.dfu_type;
                        evt.params.dfu.relay_req.fwid = p_evt->params.dfu.req.fwid;
                        evt.params.dfu.relay_req.authority = p_evt->params.dfu.req.dfu_type;
                        break;
                    case DFU_ROLE_SOURCE:
                        evt.type = RBC_MESH_EVENT_TYPE_DFU_SOURCE_REQ;
                        evt.params.dfu.source_req.dfu_type = p_evt->params.dfu.req.dfu_type;
                        break;
                    default:
                        return NRF_ERROR_NOT_SUPPORTED;
                }
                rbc_mesh_event_push(&evt);
            }
            break;

        case BL_EVT_TYPE_DFU_START:
            {
                rbc_mesh_event_t evt;
                evt.type = RBC_MESH_EVENT_TYPE_DFU_START;
                evt.params.dfu.start.dfu_type = p_evt->params.dfu.start.dfu_type;
                evt.params.dfu.start.fwid = p_evt->params.dfu.start.fwid;
                evt.params.dfu.start.role = p_evt->params.dfu.start.role;
                rbc_mesh_event_push(&evt);
                m_timer_evt.cb = abort_timeout;
                return timer_sch_reschedule(&m_timer_evt, timer_now() + TIMER_START_TIMEOUT);
            }


        case BL_EVT_TYPE_DFU_DATA_SEGMENT_RX:
            m_timer_evt.cb = abort_timeout;
            return timer_sch_reschedule(&m_timer_evt, timer_now() + TIMER_DATA_TIMEOUT);

        case BL_EVT_TYPE_DFU_END:
            {
                rbc_mesh_event_t evt;
                evt.type = RBC_MESH_EVENT_TYPE_DFU_END;
                evt.params.dfu.end.dfu_type = p_evt->params.dfu.end.dfu_type;
                evt.params.dfu.end.fwid = p_evt->params.dfu.end.fwid;
                evt.params.dfu.end.end_reason = DFU_END_SUCCESS;
                evt.params.dfu.end.role = p_evt->params.dfu.end.role;
                rbc_mesh_event_push(&evt);
                m_timer_evt.cb = abort_timeout;
                return timer_sch_reschedule(&m_timer_evt, timer_now() + TIMER_START_TIMEOUT);
            }


        case BL_EVT_TYPE_BANK_AVAILABLE:
            {
                rbc_mesh_event_t evt;
                evt.type = RBC_MESH_EVENT_TYPE_DFU_BANK_AVAILABLE;
                evt.params.dfu.bank.dfu_type     = p_evt->params.bank_available.bank_dfu_type;
                evt.params.dfu.bank.fwid         = p_evt->params.bank_available.bank_fwid;
                evt.params.dfu.bank.is_signed    = p_evt->params.bank_available.is_signed;
                evt.params.dfu.bank.p_start_addr = p_evt->params.bank_available.p_bank_addr;
                rbc_mesh_event_push(&evt);
            }
            break;

        case BL_EVT_TYPE_FLASH_ERASE:
            __LOG("Erase flash at: 0x%x (length %d)\n", p_evt->params.flash.erase.start_addr, p_evt->params.flash.erase.length);

            if (p_evt->params.flash.erase.start_addr & (NRF_FICR->CODEPAGESIZE - 1))
            {
                return NRF_ERROR_INVALID_ADDR;
            }
            if (p_evt->params.flash.erase.length & (NRF_FICR->CODEPAGESIZE - 1))
            {
                return NRF_ERROR_INVALID_LENGTH;
            }

            return mesh_flash_op_push(FLASH_OP_TYPE_ERASE, &p_evt->params.flash);

        case BL_EVT_TYPE_FLASH_WRITE:
            __LOG("Write flash at: 0x%x (length %d)\n", p_evt->params.flash.write.start_addr, p_evt->params.flash.write.length);

            if (p_evt->params.flash.write.start_addr & 0x03)
            {
                return NRF_ERROR_INVALID_ADDR;
            }
            if (p_evt->params.flash.write.length & 0x03)
            {
                return NRF_ERROR_INVALID_LENGTH;
            }
            return mesh_flash_op_push(FLASH_OP_TYPE_WRITE, &p_evt->params.flash);

        case BL_EVT_TYPE_TX_RADIO:
            __LOG("RADIO TX! SLOT %d, count %d, interval: %s, handle: %x\n",
                p_evt->params.tx.radio.tx_slot,
                p_evt->params.tx.radio.tx_count,
                p_evt->params.tx.radio.interval_type == BL_RADIO_INTERVAL_TYPE_EXPONENTIAL ? "exponential" : "periodic",
                p_evt->params.tx.radio.p_dfu_packet->packet_type
            );

            if (m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet)
            {
                mesh_packet_ref_count_dec(m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet);
                m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet = NULL;
            }
            if (mesh_packet_acquire(&m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet))
            {
                uint32_t time_now = timer_now();
                /* build packet */
                mesh_packet_set_local_addr(m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet);
                m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->header.type = BLE_PACKET_TYPE_ADV_NONCONN_IND;
                m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->header.length = DFU_PACKET_OVERHEAD + p_evt->params.tx.radio.length;
                ((ble_ad_t*) m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->payload)->adv_data_type = MESH_ADV_DATA_TYPE;
                ((ble_ad_t*) m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->payload)->data[0] = (MESH_UUID & 0xFF);
                ((ble_ad_t*) m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->payload)->data[1] = (MESH_UUID >> 8) & 0xFF;
                ((ble_ad_t*) m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->payload)->adv_data_length = DFU_PACKET_ADV_OVERHEAD + p_evt->params.tx.radio.length;
                memcpy(&m_tx_slots[p_evt->params.tx.radio.tx_slot].p_packet->payload[4], p_evt->params.tx.radio.p_dfu_packet, p_evt->params.tx.radio.length);

                /* fill other fields in the TX slot. */
                m_tx_slots[p_evt->params.tx.radio.tx_slot].interval_type = p_evt->params.tx.radio.interval_type;
                m_tx_slots[p_evt->params.tx.radio.tx_slot].repeats = p_evt->params.tx.radio.tx_count;
                m_tx_slots[p_evt->params.tx.radio.tx_slot].tx_count = 0;
                m_tx_slots[p_evt->params.tx.radio.tx_slot].order_time = time_now + DFU_TX_TIMER_MARGIN_US + (rand_prng_get(&m_prng) & (DFU_TX_START_DELAY_MASK_US));

                if (!m_tx_scheduled || TIMER_DIFF(m_tx_slots[p_evt->params.tx.radio.tx_slot].order_time, time_now) < TIMER_DIFF(m_tx_timer_evt.timestamp, time_now))
                {
                    m_tx_scheduled = true;
                    timer_sch_reschedule(&m_tx_timer_evt, m_tx_slots[p_evt->params.tx.radio.tx_slot].order_time);
                }
            }
            else
            {
                return NRF_ERROR_NO_MEM;
            }
            break;
        case BL_EVT_TYPE_TX_SERIAL:
            __LOG("SERIAL TX!\n");
            break;

        case BL_EVT_TYPE_TX_ABORT:
            if (p_evt->params.tx.abort.tx_slot >= DFU_TX_SLOTS)
            {
                return NRF_ERROR_INVALID_PARAM;
            }
            if (m_tx_slots[p_evt->params.tx.abort.tx_slot].p_packet == NULL)
            {
                return NRF_ERROR_INVALID_STATE;
            }
            else
            {
                /* set to NULL before deleting reference to avoid freak orphan
                   packet */
                mesh_packet_t* p_packet = m_tx_slots[p_evt->params.tx.abort.tx_slot].p_packet;
                m_tx_slots[p_evt->params.tx.abort.tx_slot].p_packet = NULL;
                mesh_packet_ref_count_dec(p_packet);
                return NRF_SUCCESS;
            }

        case BL_EVT_TYPE_TIMER_SET:
            NRF_GPIO->OUTSET = (1 << 1);
            __LOG("TIMER event: @%d us\n", p_evt->params.timer.set.delay_us);
            m_timer_evt.cb = timer_timeout;
            return timer_sch_reschedule(&m_timer_evt, timer_now() + p_evt->params.timer.set.delay_us);

        case BL_EVT_TYPE_TIMER_ABORT:
            __LOG("TIMER abort: %d\n", p_evt->params.timer.abort.index);
            return timer_sch_abort(&m_timer_evt);

        case BL_EVT_TYPE_ERROR:
            app_error_handler(p_evt->params.error.error_code,
                              p_evt->params.error.line,
                              (uint8_t*) p_evt->params.error.p_file);
        default:
            __LOG("Got unsupported event: 0x%x\n", p_evt->type);
            return NRF_ERROR_NOT_SUPPORTED;
    }
    return NRF_SUCCESS;
}

uint32_t bootloader_rx(mesh_adv_data_t* p_adv)
{
    if (m_cmd_handler == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (p_adv->handle <= RBC_MESH_APP_MAX_HANDLE)
    {
        return NRF_ERROR_INVALID_ADDR;
    }

    dfu_packet_t* p_dfu = (dfu_packet_t*) (&p_adv->handle);
    bl_cmd_t rx_cmd =
    {
        .type = BL_CMD_TYPE_RX,
        .params.rx.p_dfu_packet = p_dfu,
        .params.rx.length = p_adv->adv_data_length - 3
    };

    return m_cmd_handler(&rx_cmd);
}

uint32_t bootloader_dfu_abort(void)
{
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t bootloader_dfu_finalize(void)
{
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t bootloader_cmd_send(bl_cmd_t* p_cmd)
{
    if (m_cmd_handler == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    return m_cmd_handler(p_cmd);
}

