#include <common/bk_include.h>
#include <os/mem.h>
#include "bk_drv_model.h"
#include "bk_wifi.h"
#if CONFIG_FLASH_ORIGIN_API
#include "bk_flash.h"
#include "BkDriverFlash.h"
#else
#include "driver/flash.h"
#include <driver/flash_partition.h>
#endif
#include <driver/uart.h>

#define MAC_ADDR_LEN 6

static UINT32 info_item_len(NET_INFO_ITEM item)
{
	UINT32 len = 0;
	switch (item) {
	case AUTO_CONNECT_ITEM:
	case WIFI_MODE_ITEM:
	case DHCP_MODE_ITEM:
	case RF_CFG_TSSI_ITEM:
	case RF_CFG_DIST_ITEM:
	case RF_CFG_MODE_ITEM:
	case RF_CFG_TSSI_B_ITEM:              
		len = sizeof(ITEM_COMM_ST);
		break;
	case WIFI_MAC_ITEM:
		len = sizeof(ITEM_MAC_ADDR_ST);
		break;
	case CHARGE_CONFIG_ITEM:
		len = sizeof(ITEM_CHARGE_ST);
		break;
	case FAST_CONNECT_ITEM:
		len = sizeof(ITEM_FASTCONNECT_ST);
		break;
	case SSID_KEY_ITEM:
		len = sizeof(ITEM_SSIDKEY_ST);
		break;
	case IP_CONFIG_ITEM:
		len = sizeof(ITEM_IP_CONFIG_ST);
		break;
	default:
		len = sizeof(ITEM_COMM_ST);
		break;
	}
	return len;
}

static UINT32 search_info_tbl(UINT8 *buf, UINT32 *cfg_len)
{
#if CONFIG_FLASH_ORIGIN_API
	UINT32 ret = 0, status;
	DD_HANDLE flash_handle;
#else
	UINT32 ret = 0;
#endif
	TLV_HEADER_ST head;
#if CONFIG_FLASH_ORIGIN_API
	bk_logic_partition_t *pt = bk_flash_get_info(BK_PARTITION_NET_PARAM);
#else
	bk_logic_partition_t *pt = bk_flash_partition_get_info(BK_PARTITION_NET_PARAM);
#endif

	*cfg_len = 0;
#if CONFIG_FLASH_ORIGIN_API
	flash_handle = ddev_open(DD_DEV_TYPE_FLASH, &status, 0);
	ddev_read(flash_handle, (char *)&head, sizeof(TLV_HEADER_ST), pt->partition_start_addr);
#else
	bk_flash_read_bytes(pt->partition_start_addr, (uint8_t *)&head, sizeof(TLV_HEADER_ST));
#endif

	if (INFO_TLV_HEADER == head.type) {
		*cfg_len = head.len + sizeof(TLV_HEADER_ST);
		ret = 1;

		if (buf != NULL) {
#if CONFIG_FLASH_ORIGIN_API
			ddev_read(flash_handle, (char *)buf, *cfg_len, pt->partition_start_addr);
#else
			bk_flash_read_bytes(pt->partition_start_addr, (uint8_t *)buf, *cfg_len);
#endif
		}
	}
#if CONFIG_FLASH_ORIGIN_API
	ddev_close(flash_handle);
#endif
	return ret;
}

static UINT32 search_info_item(NET_INFO_ITEM type, UINT32 start_addr)
{
#if CONFIG_FLASH_ORIGIN_API
	UINT32 status, addr, end_addr;
	DD_HANDLE flash_handle;
#else
	UINT32 addr, end_addr;
#endif
	INFO_ITEM_ST head;
#if CONFIG_FLASH_ORIGIN_API
	flash_handle = ddev_open(DD_DEV_TYPE_FLASH, &status, 0);
	ddev_read(flash_handle, (char *)&head, sizeof(TLV_HEADER_ST), start_addr);
#else
	bk_flash_read_bytes(start_addr, (uint8_t *)&head, sizeof(TLV_HEADER_ST));
#endif
	addr = start_addr + sizeof(TLV_HEADER_ST);
	end_addr = addr + head.len;
	while (addr < end_addr) {
#if CONFIG_FLASH_ORIGIN_API
	ddev_read(flash_handle, (char *)&head, sizeof(INFO_ITEM_ST), addr);
#else
	bk_flash_read_bytes(addr, (uint8_t *)&head, sizeof(INFO_ITEM_ST));
#endif
		if (type != head.type) {
		//	addr += sizeof(INFO_ITEM_ST);
			head.len = info_item_len(type);
			addr += head.len;
		} else
			break;
	}

	if (addr >= end_addr)
		addr = 0;
#if CONFIG_FLASH_ORIGIN_API
	ddev_close(flash_handle);
#endif

	return addr;
}

UINT32 get_info_item(NET_INFO_ITEM item, UINT8 *ptr0, UINT8 *ptr1, UINT8 *ptr2)
{
	return 0;
}

UINT32 save_info_item(NET_INFO_ITEM item, UINT8 *ptr0, UINT8 *ptr1, UINT8 *ptr2)
{
	return 0;
}

UINT32 test_get_whole_tbl(UINT8 *ptr)
{
	UINT32 len;
	return search_info_tbl(ptr, &len);
}

UINT32 save_net_info(NET_INFO_ITEM item, UINT8 *ptr0, UINT8 *ptr1, UINT8 *ptr2)
{
	UINT32 len, offset;

	switch (item) {
	case WIFI_MAC_ITEM:
		len = MAC_ADDR_LEN;
		offset = NET_INFO_MAC_ADDR_OFFSET;
		bk_spec_flash_write_bytes(BK_PARTITION_NET_PARAM, ptr0, len, offset);
		break;

	case FAST_CONNECT_ITEM:
		len = sizeof(struct wlan_fast_connect_info);
		offset = NET_INFO_FAST_CONNECT_OFFSET;
		bk_spec_flash_write_bytes(BK_PARTITION_NET_PARAM, ptr0, len, offset);
		break;

	default:
		break;
	}

	return 0;
}

UINT32 get_net_info(NET_INFO_ITEM item, UINT8 *ptr0, UINT8 *ptr1, UINT8 *ptr2)
{
	UINT32 len, offset, ret = 0;

	switch (item) {
	case WIFI_MAC_ITEM:
		len = 6;
		offset = NET_INFO_MAC_ADDR_OFFSET;
		bk_flash_partition_read(BK_PARTITION_NET_PARAM, ptr0, offset, len);
		ret = BK_OK;
		break;

	case FAST_CONNECT_ITEM:
		len = sizeof(struct wlan_fast_connect_info);
		offset = NET_INFO_FAST_CONNECT_OFFSET;
		if (ptr0 != NULL) {
			bk_flash_partition_read(BK_PARTITION_NET_PARAM, ptr0, offset, len);
			ret = BK_OK;
			}
		break;

	default:
		break;
	}
	
	return ret;
}
