#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>


#define BT_UUID_SHS BT_UUID_DECLARE_128(                           \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0))
#define BT_UUID_DFU_CCCD BT_UUID_DECLARE_16(0x2902)
#define BT_UUID_SHC BT_UUID_DECLARE_128(                           \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1))

#define BT_UUID_AGG_SERVICE BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0xabcdef01, 0x2345, 0x6789, 0x0123, 0x456789abcdef))
#define BT_UUID_AGG_CHAR    BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0xabcdef02, 0x2345, 0x6789, 0x0123, 0x456789abcdef))

static struct bt_conn *connections[2] = {NULL, NULL};
static struct bt_gatt_subscribe_params subscribe_params[2];
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_write_params write_params;
static struct bt_gatt_read_params read_params;

// Track states separately for connection and notification
static bool connected_devices[2] = {false, false};
static bool notifications_enabled[2] = {false, false};
static int active_conn_idx = 0; // Track which device we're currently working with
static int active_discovery_idx = -1; // Track which device we're currently discovering for

// State machine phases
typedef enum {
    PHASE_CONNECTING,    // Connecting to both devices
    PHASE_DISCOVERING,   // Discovering services/characteristics
    PHASE_OPERATIONAL    // Both devices connected and notifications enabled
} system_phase_t;

static system_phase_t current_phase = PHASE_CONNECTING;

// Global variable to track the phone connection and aggregate value:
static struct bt_conn *phone_conn = NULL;
static char agg_value[256] = "No Data";

// Forward declarations
extern const struct bt_gatt_service_static agg_svc;
static void start_scan(void);
static void start_discovery_phase(int idx);
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params);

static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length)
{
    if (!data) {
        printk("[UNSUBSCRIBED] no data in the notif\n");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }
    
    int conn_idx = -1;
    for (int i = 0; i < 2; i++) {
        if (connections[i] == conn) {
            conn_idx = i;
            break;
        }
    }
    
    printk("Notification received from device %d: ", conn_idx);
    for (int i = 0; i < length; i++) {
        printk("%02x ", ((uint8_t *)data)[i]);
    }
    printk("\n");

    // Convert received data to a hex string and update agg_value:
    {
        int offset = snprintf(agg_value, sizeof(agg_value), "Device %d: ", conn_idx);
        for (int i = 0; i < length && offset < sizeof(agg_value) - 3; i++) {
            offset += snprintf(agg_value + offset, sizeof(agg_value) - offset, "%02x ", ((uint8_t *)data)[i]);
        }
        // If a phone is connected as a peripheral, send this updated data via notification:
        if (phone_conn) {
            int notify_err = bt_gatt_notify(phone_conn, agg_svc.attrs + 2, agg_value, strlen(agg_value));
            if (notify_err) {
                printk("Failed to notify phone (err %d)\n", notify_err);
            }
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t indicate_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    if (data) {
        int conn_idx = -1;
        for (int i = 0; i < 2; i++) {
            if (connections[i] == conn) {
                conn_idx = i;
                break;
            }
        }
        
        printk("Indication received from device %d: ", conn_idx);
        for (uint16_t i = 0; i < length; i++) {
            printk("%02x ", ((uint8_t *)data)[i]);
        }
        printk("\n");

        if (length == 3 && ((uint8_t *)data)[0] == 0x20 && ((uint8_t *)data)[1] == 0x01 && ((uint8_t *)data)[2] == 0x01) {
            printk("Expected indication response received from device %d.\n", conn_idx);
        }
    } else {
        printk("Indication confirmation received.\n");
    }
    return BT_GATT_ITER_CONTINUE;
}

// Write callback function
static void write_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params) {
    if (err) {
        printk("Write failed (err %d)\n", err);
    } else {
        printk("Write successful\n");
    }
}

// Function to check if we're ready to move to the next phase
static void check_phase_transition(void) {
    if (current_phase == PHASE_CONNECTING && connected_devices[0] && connected_devices[1]) {
        // Both devices connected, move to discovery phase
        printk("Both devices connected, starting discovery phase\n");
        current_phase = PHASE_DISCOVERING;
        for(int i = 0; i < 2; i++) {
            start_discovery_phase(i);
        }
    } else if (current_phase == PHASE_DISCOVERING && notifications_enabled[0] && notifications_enabled[1]) {
        // Both devices have notifications enabled, move to operational phase
        printk("Notifications enabled for both devices, entering operational phase\n");
        current_phase = PHASE_OPERATIONAL;
    }
}

// Function to start discovery for the first device
static void start_discovery_phase(int idx) {
    active_discovery_idx = idx;
    printk("Starting discovery for device %d\n", active_discovery_idx);
    
    discover_params.uuid = NULL;
    discover_params.func = discover_func;
    discover_params.start_handle = 0x0001;
    discover_params.end_handle = 0xffff;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(connections[active_discovery_idx], &discover_params);
    if (err) {
        printk("Discover failed for device %d (err %d)\n", active_discovery_idx, err);
        // Try the next device
        // active_discovery_idx = 1;
        // err = bt_gatt_discover(connections[active_discovery_idx], &discover_params);
        // if (err) {
        //     printk("Discover failed for both devices\n");
        // }
    }
    else {
        notifications_enabled[active_discovery_idx] = true;
        printk("Notifications enabled for device %d\n", active_discovery_idx);
    }
}

// Move to the next device for discovery
static void discover_next_device(void) {
    if (active_discovery_idx == 0) {
        active_discovery_idx = 1;
        printk("Starting discovery for device %d\n", active_discovery_idx);
        
        discover_params.uuid = NULL;
        discover_params.func = discover_func;
        discover_params.start_handle = 0x0001;
        discover_params.end_handle = 0xffff;
        discover_params.type = BT_GATT_DISCOVER_PRIMARY;

        int err = bt_gatt_discover(connections[active_discovery_idx], &discover_params);
        if (err) {
            printk("Discover failed for device %d (err %d)\n", active_discovery_idx, err);
        }
    } else {
        // Both devices have been processed, check if we can move to operational phase
        notifications_enabled[active_discovery_idx] = true;
        check_phase_transition();
    }
}

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;
    int conn_idx = active_discovery_idx;
    char uuid_str[37]; // Buffer for UUID string
    
    if (!attr) {
        printk("Discovery complete for device %d\n", conn_idx);
        // Move to the next device for discovery
        // discover_next_device();
        return BT_GATT_ITER_STOP;
    }
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        struct bt_gatt_service_val *service = (struct bt_gatt_service_val *)attr->user_data;
        bt_uuid_to_str(service->uuid, uuid_str, sizeof(uuid_str));
        printk("Device %d - Service UUID: %s\n", conn_idx, uuid_str);

        if (!bt_uuid_cmp(service->uuid, BT_UUID_SHS)) {
            printk("Device %d - Service UUID matched\n", conn_idx);

            params->uuid = NULL;
            params->start_handle = attr->handle + 1;
            params->end_handle = service->end_handle;
            params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

            err = bt_gatt_discover(conn, params);
            if (err) {
                printk("Device %d - Discover failed (err %d)\n", conn_idx, err);
                // discover_next_device();
                return BT_GATT_ITER_STOP;
            }
        }
    } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        struct bt_gatt_chrc *chrc = (struct bt_gatt_chrc *)attr->user_data;
        bt_uuid_to_str(chrc->uuid, uuid_str, sizeof(uuid_str));
        printk("Device %d - Characteristic UUID: %s\n", conn_idx, uuid_str);

        if (!bt_uuid_cmp(chrc->uuid, BT_UUID_SHC)) {
            subscribe_params[conn_idx].notify = notify_func;
            subscribe_params[conn_idx].value_handle = bt_gatt_attr_value_handle(attr);
            subscribe_params[conn_idx].ccc_handle = attr->handle+2;  // Usually CCCD is located two handles after characteristic
            subscribe_params[conn_idx].value = BT_GATT_CCC_NOTIFY;
            err = bt_gatt_subscribe(conn, &subscribe_params[conn_idx]);
            if (err && err != -EALREADY) {
                printk("Device %d - Subscribe failed (err %d)\n", conn_idx, err);
            } else {
                printk("Device %d - [SUBSCRIBED]\n", conn_idx);
            }
            params->type = BT_GATT_DISCOVER_PRIMARY;
            
            // Move to the next device
            // discover_next_device();
            return BT_GATT_ITER_STOP;
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_func_1(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;
    int conn_idx = active_discovery_idx;
    char uuid_str[37]; // Buffer for UUID string
    
    if (!attr) {
        printk("Discovery complete for device %d\n", conn_idx);
        // Move to the next device for discovery
        // discover_next_device();
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        struct bt_gatt_service_val *service = (struct bt_gatt_service_val *)attr->user_data;
        bt_uuid_to_str(service->uuid, uuid_str, sizeof(uuid_str));
        printk("Device %d - Service UUID: %s\n", conn_idx, uuid_str);

        if (!bt_uuid_cmp(service->uuid, BT_UUID_SHS)) {
            printk("Device %d - Service UUID matched\n", conn_idx);

            params->uuid = NULL;
            params->start_handle = attr->handle + 1;
            params->end_handle = service->end_handle;
            params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

            err = bt_gatt_discover(conn, params);
            if (err) {
                printk("Device %d - Discover failed (err %d)\n", conn_idx, err);
                // discover_next_device();
                return BT_GATT_ITER_STOP;
            }
        }
    } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        struct bt_gatt_chrc *chrc = (struct bt_gatt_chrc *)attr->user_data;
        bt_uuid_to_str(chrc->uuid, uuid_str, sizeof(uuid_str));
        printk("Device %d - Characteristic UUID: %s\n", conn_idx, uuid_str);

        if (!bt_uuid_cmp(chrc->uuid, BT_UUID_SHC)) {
            subscribe_params[conn_idx].notify = notify_func;
            subscribe_params[conn_idx].value_handle = bt_gatt_attr_value_handle(attr);
            subscribe_params[conn_idx].ccc_handle = attr->handle+2;  // Usually CCCD is located two handles after characteristic
            subscribe_params[conn_idx].value = BT_GATT_CCC_NOTIFY;
            err = bt_gatt_subscribe(conn, &subscribe_params[conn_idx]);
            if (err && err != -EALREADY) {
                printk("Device %d - Subscribe failed (err %d)\n", conn_idx, err);
            } else {
                printk("Device %d - [SUBSCRIBED]\n", conn_idx);
            }
            
            // Move to the next device
            // discover_next_device();
            return BT_GATT_ITER_STOP;
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    if (conn_err) {
        printk("Failed to connect (err %d)\n", conn_err);
        
        // Try the other device if we're in connecting phase
        if (current_phase == PHASE_CONNECTING) {
            if (active_conn_idx == 0) {
                // Failed to connect to device 0, try device 1
                active_conn_idx = 1;
                start_scan();
            } else {
                // Failed to connect to device 1, try device 0 again
                active_conn_idx = 0;
                start_scan();
            }
        }
        return;
    }

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));
    printk("Connected to %s\n", addr_str);
    
    // Check the role to differentiate a phone (incoming peripheral connection)
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_PERIPHERAL) {
        phone_conn = bt_conn_ref(conn);
        printk("Phone connected: %s\n", addr_str);
        return;
    }

    // For central-initiated connections (to your two devices):
    connections[active_conn_idx] = bt_conn_ref(conn);
    connected_devices[active_conn_idx] = true;
    printk("Saved as connection %d\n", active_conn_idx);
    
    if (current_phase == PHASE_CONNECTING) {
        // If we've connected to device 0, start connecting to device 1
        if (active_conn_idx == 0 && !connected_devices[1]) {
            active_conn_idx = 1;
            start_scan();
        } 
        // If both devices are connected, check for phase transition
        else {
            check_phase_transition();
        }
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));
    printk("Disconnected from %s (reason %d)\n", addr_str, reason);

    // If this is the phone connection, clear it:
    if (conn == phone_conn) {
        bt_conn_unref(phone_conn);
        phone_conn = NULL;
        printk("Phone disconnected\n");
        return;
    }

    // Find which connection was disconnected
    for (int i = 0; i < 2; i++) {
        if (connections[i] == conn) {
            bt_conn_unref(connections[i]);
            connections[i] = NULL;
            connected_devices[i] = false;
            notifications_enabled[i] = false;
            printk("Connection %d removed\n", i);
            
            // Reset to connecting phase if any device disconnects
            current_phase = PHASE_CONNECTING;
            active_conn_idx = i;  // Try to reconnect to the same device
            start_scan();
            break;
        }
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad)
{
    uint8_t target_mac[6];
    target_mac[0] = 237;
    target_mac[1] = 10;
    target_mac[2] = 57;
    target_mac[3] = 240;
    target_mac[4] = 14;
    target_mac[5] = 28;

    uint8_t target_mac_1[6];
    target_mac_1[0] = 195;
    target_mac_1[1] = 165;
    target_mac_1[2] = 216;
    target_mac_1[3] = 38;
    target_mac_1[4] = 247;
    target_mac_1[5] = 197;

    char addr_str[BT_ADDR_LE_STR_LEN];
    uint8_t a[6];
    bt_addr_to_str(&addr->a, addr_str, sizeof(addr_str));
 
    for (int i = 0; i < 6; i++) {
        a[i] = addr->a.val[i];
    }

    // Check if this is the device we're currently looking for
    bool is_target = false;
    
    if (active_conn_idx == 0) {
        bool matches = true;
        for (int i = 0; i < BT_ADDR_SIZE; i++) {
            if (a[i] != target_mac[BT_ADDR_SIZE - i - 1]) {
                matches = false;
                break;
            }
        }
        is_target = matches;
    } else {
        bool matches = true;
        for (int i = 0; i < BT_ADDR_SIZE; i++) {
            if (a[i] != target_mac_1[BT_ADDR_SIZE - i - 1]) {
                matches = false;
                break;
            }
        }
        is_target = matches;
    }
    
    if (!is_target || connected_devices[active_conn_idx]) {
        return;
    }

    printk("Target device %d found: %s (RSSI %d)\n", active_conn_idx, addr_str, rssi);

    int err = bt_le_scan_stop();
    if (err) {
        printk("Stop LE scan failed (err %d)\n", err);
    }

    // Clean up any stale connection handles
    if (connections[active_conn_idx] != NULL) {
        printk("Cleaning up stale connection handle for device %d\n", active_conn_idx);
        bt_conn_unref(connections[active_conn_idx]);
        connections[active_conn_idx] = NULL;
    }

    // Try to connect
    struct bt_conn *conn = NULL;
    err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                            BT_LE_CONN_PARAM_DEFAULT, &conn);
    if (err) {
        printk("Create conn to %s failed (%u)\n", addr_str, err);
        
        // Try the other device if both aren't connected
        if (active_conn_idx == 0 && !connected_devices[1]) {
            active_conn_idx = 1;
        } else if (active_conn_idx == 1 && !connected_devices[0]) {
            active_conn_idx = 0;
        }
        
        start_scan();
    } else if (conn) {
        // Connection initiated
        bt_conn_unref(conn);
    }
}

static void start_scan(void)
{
    // Skip scanning if in wrong phase
    if (current_phase != PHASE_CONNECTING) {
        return;
    }
    
    int err;
    // bt_le_scan_stop(); // Stop any active scan
    
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    err = bt_le_scan_start(&scan_param, device_found);
    if (err) {
        printk("Scanning failed to start (err %d)\n", err);
        return;
    }

    printk("Scanning for device %d started\n", active_conn_idx);
}

// Define a read callback for the aggregated characteristic:
static ssize_t read_agg(struct bt_conn *conn, const struct bt_gatt_attr *attr,
    void *buf, uint16_t len, uint16_t offset)
{
    const char *value = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}

// Define a CCC configuration changed callback:
static void agg_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    printk("Aggregated characteristic CCC changed: 0x%04x\n", value);
}

// Define the aggregated service using the service definition macro:
BT_GATT_SERVICE_DEFINE(agg_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_AGG_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_AGG_CHAR,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_agg, NULL, agg_value),
    BT_GATT_CCC(agg_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

void main(void)
{
    int err;

    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }

    printk("Bluetooth initialized\n");

    // Start advertising so that a phone can connect to our GATT server:
    const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, (sizeof(CONFIG_BT_DEVICE_NAME) - 1)),
    };
    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
    } else {
        printk("Advertising started\n");
    }

    // Initialize phase to connecting
    current_phase = PHASE_CONNECTING;
    active_conn_idx = 0;
    start_scan();
}