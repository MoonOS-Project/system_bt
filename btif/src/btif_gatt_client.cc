/******************************************************************************
 *
 *  Copyright (C) 2009-2014 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*******************************************************************************
 *
 *  Filename:      btif_gatt_client.c
 *
 *  Description:   GATT client implementation
 *
 *******************************************************************************/

#define LOG_TAG "bt_btif_gattc"

#include <base/at_exit.h>
#include <base/bind.h>
#include <base/threading/thread.h>
#include <errno.h>
#include <hardware/bluetooth.h>
#include <stdlib.h>
#include <string.h>
#include "device/include/controller.h"

#include "btcore/include/bdaddr.h"
#include "btif_common.h"
#include "btif_util.h"

#if (BLE_INCLUDED == TRUE)

#include <hardware/bt_gatt.h>

#include "bta_api.h"
#include "bta_gatt_api.h"
#include "btif_config.h"
#include "btif_dm.h"
#include "btif_gatt.h"
#include "btif_gatt_util.h"
#include "btif_storage.h"
#include "osi/include/log.h"
#include "vendor_api.h"

using base::Bind;
using base::Owned;
using std::vector;

extern bt_status_t do_in_jni_thread(const base::Closure& task);
extern bt_status_t btif_gattc_test_command_impl(int command,
                                                btgatt_test_params_t* params);
extern const btgatt_callbacks_t* bt_gatt_callbacks;

/*******************************************************************************
 *  Constants & Macros
 *******************************************************************************/

#define CLI_CBACK_IN_JNI(P_CBACK, ...)                                         \
  do {                                                                         \
    if (bt_gatt_callbacks && bt_gatt_callbacks->client->P_CBACK) {             \
      BTIF_TRACE_API("HAL bt_gatt_callbacks->client->%s", #P_CBACK);           \
      do_in_jni_thread(Bind(bt_gatt_callbacks->client->P_CBACK, __VA_ARGS__)); \
    } else {                                                                   \
      ASSERTC(0, "Callback is NULL", 0);                                       \
    }                                                                          \
  } while (0)

#define CHECK_BTGATT_INIT()                                      \
  do {                                                           \
    if (bt_gatt_callbacks == NULL) {                             \
      LOG_WARN(LOG_TAG, "%s: BTGATT not initialized", __func__); \
      return BT_STATUS_NOT_READY;                                \
    } else {                                                     \
      LOG_VERBOSE(LOG_TAG, "%s", __func__);                      \
    }                                                            \
  } while (0)

#define BLE_RESOLVE_ADDR_MSB                                                   \
  0x40                             /* bit7, bit6 is 01 to be resolvable random \
                                      */
#define BLE_RESOLVE_ADDR_MASK 0xc0 /* bit 6, and bit7 */
#define BTM_BLE_IS_RESOLVE_BDA(x) \
  (((x)[0] & BLE_RESOLVE_ADDR_MASK) == BLE_RESOLVE_ADDR_MSB)

namespace {

uint8_t rssi_request_client_if;

void btif_gattc_upstreams_evt(uint16_t event, char* p_param) {
  LOG_VERBOSE(LOG_TAG, "%s: Event %d", __func__, event);

  tBTA_GATTC* p_data = (tBTA_GATTC*)p_param;
  switch (event) {
    case BTA_GATTC_REG_EVT: {
      bt_uuid_t app_uuid;
      bta_to_btif_uuid(&app_uuid, &p_data->reg_oper.app_uuid);
      HAL_CBACK(bt_gatt_callbacks, client->register_client_cb,
                p_data->reg_oper.status, p_data->reg_oper.client_if, &app_uuid);
      break;
    }

    case BTA_GATTC_DEREG_EVT:
      break;

    case BTA_GATTC_EXEC_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->execute_write_cb,
                p_data->exec_cmpl.conn_id, p_data->exec_cmpl.status);
      break;
    }

    case BTA_GATTC_SEARCH_CMPL_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->search_complete_cb,
                p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
      break;
    }

    case BTA_GATTC_NOTIF_EVT: {
      btgatt_notify_params_t data;

      bdcpy(data.bda.address, p_data->notify.bda);
      memcpy(data.value, p_data->notify.value, p_data->notify.len);

      data.handle = p_data->notify.handle;
      data.is_notify = p_data->notify.is_notify;
      data.len = p_data->notify.len;

      HAL_CBACK(bt_gatt_callbacks, client->notify_cb, p_data->notify.conn_id,
                &data);

      if (p_data->notify.is_notify == false)
        BTA_GATTC_SendIndConfirm(p_data->notify.conn_id, p_data->notify.handle);

      break;
    }

    case BTA_GATTC_OPEN_EVT: {
      bt_bdaddr_t bda;
      bdcpy(bda.address, p_data->open.remote_bda);

      HAL_CBACK(bt_gatt_callbacks, client->open_cb, p_data->open.conn_id,
                p_data->open.status, p_data->open.client_if, &bda);

      if (GATT_DEF_BLE_MTU_SIZE != p_data->open.mtu && p_data->open.mtu) {
        HAL_CBACK(bt_gatt_callbacks, client->configure_mtu_cb,
                  p_data->open.conn_id, p_data->open.status, p_data->open.mtu);
      }

      if (p_data->open.status == BTA_GATT_OK)
        btif_gatt_check_encrypted_link(p_data->open.remote_bda,
                                       p_data->open.transport);
      break;
    }

    case BTA_GATTC_CLOSE_EVT: {
      bt_bdaddr_t bda;
      bdcpy(bda.address, p_data->close.remote_bda);
      HAL_CBACK(bt_gatt_callbacks, client->close_cb, p_data->close.conn_id,
                p_data->status, p_data->close.client_if, &bda);
      break;
    }

    case BTA_GATTC_ACL_EVT:
      LOG_DEBUG(LOG_TAG, "BTA_GATTC_ACL_EVT: status = %d", p_data->status);
      /* Ignore for now */
      break;

    case BTA_GATTC_CANCEL_OPEN_EVT:
      break;

    case BTA_GATTC_CFG_MTU_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->configure_mtu_cb,
                p_data->cfg_mtu.conn_id, p_data->cfg_mtu.status,
                p_data->cfg_mtu.mtu);
      break;
    }

    case BTA_GATTC_CONGEST_EVT:
      HAL_CBACK(bt_gatt_callbacks, client->congestion_cb,
                p_data->congest.conn_id, p_data->congest.congested);
      break;

    default:
      LOG_ERROR(LOG_TAG, "%s: Unhandled event (%d)!", __func__, event);
      break;
  }
}

void bta_gattc_cback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
  bt_status_t status =
      btif_transfer_context(btif_gattc_upstreams_evt, (uint16_t)event,
                            (char*)p_data, sizeof(tBTA_GATTC), NULL);
  ASSERTC(status == BT_STATUS_SUCCESS, "Context transfer failed!", status);
}


void btm_read_rssi_cb(tBTM_RSSI_RESULTS* p_result) {
  if (!p_result) return;

  bt_bdaddr_t* addr = new bt_bdaddr_t;
  bdcpy(addr->address, p_result->rem_bda);
  CLI_CBACK_IN_JNI(read_remote_rssi_cb, rssi_request_client_if,
                   base::Owned(addr), p_result->rssi, p_result->status);
}

/*******************************************************************************
 *  Client API Functions
 *******************************************************************************/

void btif_gattc_register_app_impl(tBT_UUID uuid) {
  BTA_GATTC_AppRegister(&uuid, bta_gattc_cback);
}

bt_status_t btif_gattc_register_app(bt_uuid_t* uuid) {
  CHECK_BTGATT_INIT();

  tBT_UUID bt_uuid;
  btif_to_bta_uuid(&bt_uuid, uuid);
  return do_in_jni_thread(Bind(&btif_gattc_register_app_impl, bt_uuid));
}

void btif_gattc_unregister_app_impl(int client_if) {
  BTA_GATTC_AppDeregister(client_if);
}

bt_status_t btif_gattc_unregister_app(int client_if) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&btif_gattc_unregister_app_impl, client_if));
}

void btif_gattc_open_impl(int client_if, BD_ADDR address, bool is_direct,
                          int transport_p) {
  // Ensure device is in inquiry database
  int addr_type = 0;
  int device_type = 0;
  tBTA_GATT_TRANSPORT transport = (tBTA_GATT_TRANSPORT)BTA_GATT_TRANSPORT_LE;

  if (btif_get_address_type(address, &addr_type) &&
      btif_get_device_type(address, &device_type) &&
      device_type != BT_DEVICE_TYPE_BREDR) {
    BTA_DmAddBleDevice(address, addr_type, device_type);
  }

  // Check for background connections
  if (!is_direct) {
    // Check for privacy 1.0 and 1.1 controller and do not start background
    // connection if RPA offloading is not supported, since it will not
    // connect after change of random address
    if (!controller_get_interface()->supports_ble_privacy() &&
        (addr_type == BLE_ADDR_RANDOM) && BTM_BLE_IS_RESOLVE_BDA(address)) {
      tBTM_BLE_VSC_CB vnd_capabilities;
      BTM_BleGetVendorCapabilities(&vnd_capabilities);
      if (!vnd_capabilities.rpa_offloading) {
        HAL_CBACK(bt_gatt_callbacks, client->open_cb, 0, BT_STATUS_UNSUPPORTED,
                  client_if, (bt_bdaddr_t*)&address);
        return;
      }
    }
    BTA_DmBleSetBgConnType(BTM_BLE_CONN_AUTO, NULL);
  }

  // Determine transport
  if (transport_p != GATT_TRANSPORT_AUTO) {
    transport = transport_p;
  } else {
    switch (device_type) {
      case BT_DEVICE_TYPE_BREDR:
        transport = BTA_GATT_TRANSPORT_BR_EDR;
        break;

      case BT_DEVICE_TYPE_BLE:
        transport = BTA_GATT_TRANSPORT_LE;
        break;

      case BT_DEVICE_TYPE_DUMO:
        if (transport == GATT_TRANSPORT_LE)
          transport = BTA_GATT_TRANSPORT_LE;
        else
          transport = BTA_GATT_TRANSPORT_BR_EDR;
        break;
    }
  }

  // Connect!
  BTIF_TRACE_DEBUG("%s Transport=%d, device type=%d", __func__, transport,
                   device_type);
  BTA_GATTC_Open(client_if, address, is_direct, transport);
}

bt_status_t btif_gattc_open(int client_if, const bt_bdaddr_t* bd_addr,
                            bool is_direct, int transport) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t* address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(&btif_gattc_open_impl, client_if,
                               base::Owned(address), is_direct, transport));
}

void btif_gattc_close_impl(int client_if, BD_ADDR address, int conn_id) {
  // Disconnect established connections
  if (conn_id != 0)
    BTA_GATTC_Close(conn_id);
  else
    BTA_GATTC_CancelOpen(client_if, address, true);

  // Cancel pending background connections (remove from whitelist)
  BTA_GATTC_CancelOpen(client_if, address, false);
}

bt_status_t btif_gattc_close(int client_if, const bt_bdaddr_t* bd_addr,
                             int conn_id) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t* address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(&btif_gattc_close_impl, client_if, base::Owned(address), conn_id));
}

void btif_gattc_listen_cb(int client_if, uint8_t status)
{
  HAL_CBACK(bt_gatt_callbacks, client->listen_cb, status, client_if);
}

bt_status_t btif_gattc_listen(int client_if, bool start) {
  CHECK_BTGATT_INIT();
#if (defined(BLE_PERIPHERAL_MODE_SUPPORT) && \
     (BLE_PERIPHERAL_MODE_SUPPORT == true))
  return do_in_jni_thread(Bind(&BTA_GATTC_Listen, start, base::Bind(&btif_gattc_listen_cb, client_if)));
#else
  return do_in_jni_thread(Bind(&BTA_GATTC_Broadcast, start, base::Bind(&btif_gattc_listen_cb, client_if)));
#endif
}

bt_status_t btif_gattc_refresh(int client_if, const bt_bdaddr_t* bd_addr) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t* address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(&BTA_GATTC_Refresh, base::Owned(address)));
}

bt_status_t btif_gattc_search_service(int conn_id, bt_uuid_t* filter_uuid) {
  CHECK_BTGATT_INIT();

  if (filter_uuid) {
    tBT_UUID* uuid = new tBT_UUID;
    btif_to_bta_uuid(uuid, filter_uuid);
    return do_in_jni_thread(
        Bind(&BTA_GATTC_ServiceSearchRequest, conn_id, base::Owned(uuid)));
  } else {
    return do_in_jni_thread(
        Bind(&BTA_GATTC_ServiceSearchRequest, conn_id, nullptr));
  }
}

void btif_gattc_get_gatt_db_impl(int conn_id) {
  btgatt_db_element_t* db = NULL;
  int count = 0;
  BTA_GATTC_GetGattDb(conn_id, 0x0000, 0xFFFF, &db, &count);

  HAL_CBACK(bt_gatt_callbacks, client->get_gatt_db_cb, conn_id, db, count);
  osi_free(db);
}

bt_status_t btif_gattc_get_gatt_db(int conn_id) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&btif_gattc_get_gatt_db_impl, conn_id));
}

void read_char_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                  uint16_t len, uint8_t* value, void* data) {
  btgatt_read_params_t* params = new btgatt_read_params_t;
  params->value_type = 0x00 /* GATTC_READ_VALUE_TYPE_VALUE */;
  params->status = status;
  params->handle = handle;
  params->value.len = len;
  assert(len <= BTGATT_MAX_ATTR_LEN);
  if (len > 0) memcpy(params->value.value, value, len);

  CLI_CBACK_IN_JNI(read_characteristic_cb, conn_id, status,
                   base::Owned(params));
}

bt_status_t btif_gattc_read_char(int conn_id, uint16_t handle, int auth_req) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&BTA_GATTC_ReadCharacteristic, conn_id, handle,
                               auth_req, read_char_cb, nullptr));
}

void read_desc_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                  uint16_t len, uint8_t* value, void* data) {
  btgatt_read_params_t* params = new btgatt_read_params_t;
  params->value_type = 0x00 /* GATTC_READ_VALUE_TYPE_VALUE */;
  params->status = status;
  params->handle = handle;
  params->value.len = len;
  assert(len <= BTGATT_MAX_ATTR_LEN);
  if (len > 0) memcpy(params->value.value, value, len);

  CLI_CBACK_IN_JNI(read_descriptor_cb, conn_id, status, base::Owned(params));
}

bt_status_t btif_gattc_read_char_descr(int conn_id, uint16_t handle,
                                       int auth_req) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&BTA_GATTC_ReadCharDescr, conn_id, handle,
                               auth_req, read_desc_cb, nullptr));
}

void write_char_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                   void* data) {
  CLI_CBACK_IN_JNI(write_characteristic_cb, conn_id, status, handle);
}

bt_status_t btif_gattc_write_char(int conn_id, uint16_t handle, int write_type,
                                  int auth_req, vector<uint8_t> value) {
  CHECK_BTGATT_INIT();

  if (value.size() > BTGATT_MAX_ATTR_LEN) value.resize(BTGATT_MAX_ATTR_LEN);

  return do_in_jni_thread(Bind(&BTA_GATTC_WriteCharValue, conn_id, handle,
                               write_type, std::move(value), auth_req,
                               write_char_cb, nullptr));
}

void write_descr_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                    void* data) {
  CLI_CBACK_IN_JNI(write_descriptor_cb, conn_id, status, handle);
}

bt_status_t btif_gattc_write_char_descr(int conn_id, uint16_t handle,
                                        int auth_req, vector<uint8_t> value) {
  CHECK_BTGATT_INIT();

  if (value.size() > BTGATT_MAX_ATTR_LEN) value.resize(BTGATT_MAX_ATTR_LEN);

  return do_in_jni_thread(Bind(&BTA_GATTC_WriteCharDescr, conn_id, handle,
                               std::move(value), auth_req, write_descr_cb,
                               nullptr));
}

bt_status_t btif_gattc_execute_write(int conn_id, int execute) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(&BTA_GATTC_ExecuteWrite, conn_id, (uint8_t)execute));
}

void btif_gattc_reg_for_notification_impl(tBTA_GATTC_IF client_if,
                                          const BD_ADDR bda, uint16_t handle) {
  tBTA_GATT_STATUS status = BTA_GATTC_RegisterForNotifications(
      client_if, const_cast<uint8_t*>(bda), handle);

  // TODO(jpawlowski): conn_id is currently unused
  HAL_CBACK(bt_gatt_callbacks, client->register_for_notification_cb,
            /* conn_id */ 0, 1, status, handle);
}

bt_status_t btif_gattc_reg_for_notification(int client_if,
                                            const bt_bdaddr_t* bd_addr,
                                            uint16_t handle) {
  CHECK_BTGATT_INIT();

  uint8_t* address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_reg_for_notification_impl), client_if,
           base::Owned(address), handle));
}

void btif_gattc_dereg_for_notification_impl(tBTA_GATTC_IF client_if,
                                            const BD_ADDR bda,
                                            uint16_t handle) {
  tBTA_GATT_STATUS status = BTA_GATTC_DeregisterForNotifications(
      client_if, const_cast<uint8_t*>(bda), handle);

  // TODO(jpawlowski): conn_id is currently unused
  HAL_CBACK(bt_gatt_callbacks, client->register_for_notification_cb,
            /* conn_id */ 0, 0, status, handle);
}

bt_status_t btif_gattc_dereg_for_notification(int client_if,
                                              const bt_bdaddr_t* bd_addr,
                                              uint16_t handle) {
  CHECK_BTGATT_INIT();

  uint8_t* address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_dereg_for_notification_impl),
           client_if, base::Owned(address), handle));
}

bt_status_t btif_gattc_read_remote_rssi(int client_if,
                                        const bt_bdaddr_t* bd_addr) {
  CHECK_BTGATT_INIT();
  rssi_request_client_if = client_if;
  // Closure will own this value and free it.
  uint8_t* address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(base::IgnoreResult(&BTM_ReadRSSI),
                               base::Owned(address),
                               (tBTM_CMPL_CB*)btm_read_rssi_cb));
}

bt_status_t btif_gattc_configure_mtu(int conn_id, int mtu) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&BTA_GATTC_ConfigureMTU), conn_id, mtu));
}

void btif_gattc_conn_parameter_update_impl(bt_bdaddr_t addr, int min_interval,
                                           int max_interval, int latency,
                                           int timeout) {
  if (BTA_DmGetConnectionState(addr.address))
    BTA_DmBleUpdateConnectionParams(addr.address, min_interval, max_interval,
                                    latency, timeout);
  else
    BTA_DmSetBlePrefConnParams(addr.address, min_interval, max_interval,
                               latency, timeout);
}

bt_status_t btif_gattc_conn_parameter_update(const bt_bdaddr_t* bd_addr,
                                             int min_interval, int max_interval,
                                             int latency, int timeout) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_conn_parameter_update_impl), *bd_addr,
           min_interval, max_interval, latency, timeout));
}

int btif_gattc_get_device_type(const bt_bdaddr_t* bd_addr) {
  int device_type = 0;
  char bd_addr_str[18] = {0};

  bdaddr_to_string(bd_addr, bd_addr_str, sizeof(bd_addr_str));
  if (btif_config_get_int(bd_addr_str, "DevType", &device_type))
    return device_type;
  return 0;
}

bt_status_t btif_gattc_test_command(int command, btgatt_test_params_t* params) {
  return btif_gattc_test_command_impl(command, params);
}

}  // namespace

const btgatt_client_interface_t btgattClientInterface = {
    btif_gattc_register_app,
    btif_gattc_unregister_app,
    btif_gattc_open,
    btif_gattc_close,
    btif_gattc_listen,
    btif_gattc_refresh,
    btif_gattc_search_service,
    btif_gattc_read_char,
    btif_gattc_write_char,
    btif_gattc_read_char_descr,
    btif_gattc_write_char_descr,
    btif_gattc_execute_write,
    btif_gattc_reg_for_notification,
    btif_gattc_dereg_for_notification,
    btif_gattc_read_remote_rssi,
    btif_gattc_get_device_type,
    btif_gattc_configure_mtu,
    btif_gattc_conn_parameter_update,
    btif_gattc_test_command,
    btif_gattc_get_gatt_db};

#endif
