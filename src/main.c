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


static struct bt_conn *default_conn;
static struct bt_gatt_subscribe_params subscribe_params;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_write_params write_params;
static struct bt_gatt_read_params read_params;

static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length)
{
    if (!data) {
		printk("[UNSUBSCRIBED] no data in the notif\n");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}
    printk("Notification received: ");
    for (int i = 0; i < length; i++) {
        printk("%02x ", ((uint8_t *)data)[i]);
    }
    printk("\n");

    return BT_GATT_ITER_CONTINUE;
}


static void start_scan(void);

static uint8_t indicate_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    if (data) {
        printk("Indication received: ");
        for (uint16_t i = 0; i < length; i++) {
            printk("%02x ", ((uint8_t *)data)[i]);
        }
        printk("\n");

        if (length == 3 && ((uint8_t *)data)[0] == 0x20 && ((uint8_t *)data)[1] == 0x01 && ((uint8_t *)data)[2] == 0x01) {
            printk("Expected indication response received.\n");
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

// Subscribe function
static void subscribe_to_characteristic(struct bt_conn *conn, uint16_t value_handle) {
    int err;

    subscribe_params.notify = indicate_func;
    subscribe_params.value_handle = value_handle;
    subscribe_params.ccc_handle = 0; // This will be auto-discovered
    subscribe_params.value = BT_GATT_CCC_INDICATE;
    atomic_set_bit(subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

    err = bt_gatt_subscribe(conn, &subscribe_params);
    if (err) {
        printk("Subscribe failed (err %d)\n", err);
    } else {
        printk("Subscribed successfully\n");

        static uint8_t data[] = {0x20, 0x01};
        write_params.func = write_func;
        write_params.handle = value_handle;
        write_params.offset = 0;
        write_params.data = data;
        write_params.length = sizeof(data);

        err = bt_gatt_write(conn, &write_params);
        if (err) {
            printk("Write request failed (err %d)\n", err);
        } else {
            printk("Write request sent\n");
        }
    }
}

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;

    if (!attr) {
        printk("Discovery complete\n");
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        struct bt_gatt_service_val *service = (struct bt_gatt_service_val *)attr->user_data;
        printk("Service UUID: %s\n", bt_uuid_str(service->uuid));

        if (!bt_uuid_cmp(service->uuid, BT_UUID_SHS)) {
            printk("Service UUID matched\n");

            discover_params.uuid = NULL;
            discover_params.start_handle = attr->handle + 1;
            discover_params.end_handle = service->end_handle;
            discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
            discover_params.func = discover_func;

            err = bt_gatt_discover(conn, &discover_params);
            if (err) {
                printk("Discover failed (err %d)\n", err);
                return BT_GATT_ITER_STOP;
            }
        }
    } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        struct bt_gatt_chrc *chrc = (struct bt_gatt_chrc *)attr->user_data;
        printk("Characteristic UUID: %s\n", bt_uuid_str(chrc->uuid));


        if (!bt_uuid_cmp(chrc->uuid, BT_UUID_SHC)) {
            // printk("Characteristic UUID matched\n");
			// printk("%x \n",chrc->properties);
            // subscribe_params.notify = indicate_func;
		    // subscribe_params.value = BT_GATT_CCC_INDICATE;
		    // subscribe_params.ccc_handle = attr->handle+2;
            // err = bt_gatt_subscribe(conn, &subscribe_params);
            // if (err) {
            //     printk("Subscribe failed (err %d)\n", err);
            // } else {
            //     printk("[SUBSCRIBED]\n");
            // }
			// static uint8_t data = 0x99;
            // write_params.func = write_func;
            // write_params.handle = chrc->value_handle;
            // write_params.offset = 0;
            // write_params.data = &data;
            // write_params.length = sizeof(data);
            // int err = bt_gatt_write(conn, &write_params);
            // if (err) {
            //     printk("Write request failed (err %d)\n", err);
            // } else {
            //     printk("Write request sent\n");
            // }
            //     return BT_GATT_ITER_STOP;
            subscribe_params.notify = notify_func;
            subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
            subscribe_params.ccc_handle = attr->handle+2 ;  // Usually CCCD is located two handles after characteristic
            subscribe_params.value = BT_GATT_CCC_NOTIFY;
            err = bt_gatt_subscribe(conn, &subscribe_params);
        //     err = bt_gatt_subscribe(conn, &subscribe_params);
            if (err && err != -EALREADY) {
                printk("Subscribe failed (err %d)\n", err);
            } else {
                printk("[SUBSCRIBED]\n");
            }

            return BT_GATT_ITER_STOP;
        }

		else if (params->type == BT_GATT_DISCOVER_ATTRIBUTE) {
			printk("Found\n");
		}
    }

    return BT_GATT_ITER_CONTINUE;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    if (conn_err) {
        printk("Failed to connect (err %d)\n", conn_err);
        return;
    }

    default_conn = bt_conn_ref(conn);

    printk("Connected\n");
//     bt_set_bondable(true);


    discover_params.uuid = NULL;
    discover_params.func = discover_func;
    discover_params.start_handle = 0x0001;
    discover_params.end_handle = 0xffff;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(default_conn, &discover_params);
    if (err) {
        printk("Discover failed(err %d)\n", err);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected (reason %d)\n", reason);

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
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

     char addr_str[BT_ADDR_LE_STR_LEN];
    uint8_t a[6];
    bt_addr_to_str(&addr->a, addr_str, sizeof(addr_str));
 
    for (int i = 0; i < 6; i++) {
        a[i] = addr->a.val[i];
    }

    if (default_conn) {
        return;
    }

    bool err_fg = false;
    for (int i = 0; i < BT_ADDR_SIZE; i++) {
        if (a[i] != target_mac[BT_ADDR_SIZE - i - 1]) {
            err_fg = true;
            break;
        }
    }

    if (err_fg) {
        printk("DEVICE NOT FOUND\n");
        return;
    }
    printk("Device found: %s (RSSI %d)\n", addr_str, rssi);

    int err = bt_le_scan_stop();
    if (err) {
        printk("Stop LE scan failed (err %d)\n", err);
    }

    err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                            BT_LE_CONN_PARAM_DEFAULT, &default_conn);
    if (err) {
        printk("Create conn to %s failed (%u)\n", addr_str, err);
        start_scan();
    }
}

static void start_scan(void)
{
    int err;

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

    printk("Scanning successfully started\n");
}

void main(void)
{
    int err;

    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }

    printk("Bluetooth initialized\n");

    start_scan();
}