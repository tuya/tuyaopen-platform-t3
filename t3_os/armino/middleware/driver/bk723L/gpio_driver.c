// Copyright 2020-2021 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <common/bk_include.h>
#include "bk_intc.h"
#include "bk_icu.h"
#include <os/mem.h>
#include "gpio_hal.h"
#include "gpio_map.h"
#include "gpio_driver.h"
#include "gpio_driver_base.h"
#include "icu_driver.h"
#include "amp_lock_api.h"
#include <driver/gpio.h>

extern gpio_driver_t s_gpio;

#define GPIO_REG_LOCK_WAIT_TIME_MS  6
#define GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, mode_max) do {\
				if ((mode) >= (mode_max)) {\
					return BK_ERR_GPIO_SET_INVALID_FUNC_MODE;\
				}\
			} while(0)

bk_err_t gpio_dev_map(gpio_id_t gpio_id, gpio_dev_t dev)
{
	uint32_t ret_val = 1;

	ret_val = amp_res_acquire(AMP_RES_ID_GPIO, GPIO_REG_LOCK_WAIT_TIME_MS);
	GPIO_LOGD("amp_res_acquire:ret=%d\r\n", ret_val);
	if(ret_val != BK_OK)
		return ret_val;

	/* Restore a configuration that is not a secondary function to its initial state. */
	gpio_hal_output_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_input_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_pull_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_disable_interrupt(&s_gpio.hal, gpio_id);
	gpio_hal_func_map(&s_gpio.hal, gpio_id, dev);

	ret_val = amp_res_release(AMP_RES_ID_GPIO);
	GPIO_LOGD("amp res release:ret=%d\r\n", ret_val);
	if(ret_val != BK_OK)
		return ret_val;

	return BK_OK;
}

bk_err_t gpio_dev_unmap(gpio_id_t gpio_id)
{
	uint32_t ret_val = 1;

	ret_val = amp_res_acquire(AMP_RES_ID_GPIO, GPIO_REG_LOCK_WAIT_TIME_MS);
	GPIO_LOGD("amp_res_acquire:ret=%d\r\n", ret_val);
	if(ret_val != BK_OK)
		return ret_val;

	/* Restore a configuration that is not a secondary function to its initial state. */
	gpio_hal_output_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_input_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_pull_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_disable_interrupt(&s_gpio.hal, gpio_id);
	gpio_hal_func_unmap(&s_gpio.hal, gpio_id);

	ret_val = amp_res_release(AMP_RES_ID_GPIO);
	GPIO_LOGD("amp res release:ret=%d\r\n", ret_val);
	if(ret_val != BK_OK)
		return ret_val;

	return BK_OK;
}

/* Here doesn't check the GPIO id is whether used by another CPU-CORE, but checked current CPU-CORE */
bk_err_t gpio_dev_unprotect_map(gpio_id_t gpio_id, gpio_dev_t dev)
{
	GPIO_LOGI("%s:id=%d, dev=%d\r\n", __func__, gpio_id, dev);

	/* Restore a configuration that is not a secondary function to its initial state. */
	gpio_hal_output_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_input_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_pull_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_disable_interrupt(&s_gpio.hal, gpio_id);
	gpio_hal_func_map(&s_gpio.hal, gpio_id, dev);

	return BK_OK;
}

/* Here doesn't check the GPIO id is whether used by another CPU-CORE */
bk_err_t gpio_dev_unprotect_unmap(gpio_id_t gpio_id)
{
	/* Restore a configuration that is not a secondary function to its initial state. */
	gpio_hal_output_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_input_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_pull_enable(&s_gpio.hal, gpio_id, 0);
	gpio_hal_disable_interrupt(&s_gpio.hal, gpio_id);
	gpio_hal_func_unmap(&s_gpio.hal, gpio_id);

	return BK_OK;
}

bk_err_t gpio_i2c1_sel(gpio_i2c1_map_mode_t mode)
{
	GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, GPIO_I2C1_MAP_MODE_MAX);

	GPIO_MAP_TABLE(GPIO_I2C1_USED_GPIO_NUM, GPIO_I2C1_MAP_MODE_MAX, i2c1_gpio_map) = GPIO_I2C1_MAP_TABLE;

	gpio_hal_devs_map(&s_gpio.hal, i2c1_gpio_map[mode].gpio_bits, i2c1_gpio_map[mode].devs, GPIO_I2C1_USED_GPIO_NUM);

	return BK_OK;
}

bk_err_t gpio_i2s_sel(gpio_i2s_map_mode_t mode)
{
	GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, GPIO_I2S_MAP_MODE_MAX);

	GPIO_MAP_TABLE(GPIO_I2S_USED_GPIO_NUM, GPIO_I2S_MAP_MODE_MAX, i2s_gpio_map) = GPIO_I2S_MAP_TABLE;

	gpio_hal_devs_map(&s_gpio.hal, i2s_gpio_map[mode].gpio_bits, i2s_gpio_map[mode].devs, GPIO_I2S_USED_GPIO_NUM);

	return BK_OK;
}

bk_err_t gpio_spi_sel(gpio_spi1_map_mode_t mode)
{
	GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, GPIO_SPI_MAP_MODE_MAX);

	GPIO_MAP_TABLE(GPIO_SPI0_USED_GPIO_NUM, GPIO_SPI_MAP_MODE_MAX, spi_gpio_map) = GPIO_SPI0_MAP_TABLE;

	gpio_hal_devs_map(&s_gpio.hal, spi_gpio_map[mode].gpio_bits, spi_gpio_map[mode].devs, GPIO_SPI0_USED_GPIO_NUM);

	return BK_OK;
}

bk_err_t gpio_sdio_sel(gpio_sdio_map_mode_t mode)
{
	GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, GPIO_SDIO_MAP_MODE_MAX);

	GPIO_MAP_TABLE(GPIO_SDIO_USED_GPIO_NUM, GPIO_SDIO_MAP_MODE_MAX, sdio_gpio_map) = GPIO_SDIO_MAP_TABLE;

	gpio_hal_devs_map(&s_gpio.hal, sdio_gpio_map[mode].gpio_bits, sdio_gpio_map[mode].devs, GPIO_SDIO_USED_GPIO_NUM);

	return BK_OK;
}

bk_err_t gpio_sdio_one_line_sel(gpio_sdio_map_mode_t mode)
{
	GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, GPIO_SDIO_MAP_MODE_MAX);

	GPIO_MAP_TABLE(GPIO_SDIO_ONE_LINE_USED_GPIO_NUM, GPIO_SDIO_MAP_MODE_MAX, sdio_gpio_map) = GPIO_SDIO_ONE_LINE_MAP_TABLE;

	gpio_hal_devs_map(&s_gpio.hal, sdio_gpio_map[mode].gpio_bits, sdio_gpio_map[mode].devs, GPIO_SDIO_ONE_LINE_USED_GPIO_NUM);

	return BK_OK;
}


bk_err_t gpio_jtag_sel(gpio_jtag_map_group_t group_id)
{
	gpio_dev_unprotect_unmap(GPIO_20);
	gpio_dev_unprotect_unmap(GPIO_21);

#if CONFIG_SPE
	gpio_dev_unprotect_unmap(GPIO_0);
	gpio_dev_unprotect_unmap(GPIO_1);
#endif

	if (group_id == GPIO_JTAG_MAP_GROUP0) {
		gpio_dev_unprotect_map(GPIO_20, GPIO_DEV_JTAG_TCK);
		gpio_dev_unprotect_map(GPIO_21, GPIO_DEV_JTAG_TMS);
	} else if (group_id == GPIO_JTAG_MAP_GROUP1) {
		gpio_dev_unprotect_map(GPIO_0, GPIO_DEV_JTAG_TCK);
		gpio_dev_unprotect_map(GPIO_1, GPIO_DEV_JTAG_TMS);
	} else {
		GPIO_LOGI("Unsupported group id(%d).\r\n", group_id);
		return BK_FAIL;
	}

	return BK_OK;
}

bk_err_t gpio_scr_sel(gpio_scr_map_group_t mode)
{
	GPIO_RETURN_ON_INVALID_PERIAL_MODE(mode, GPIO_SCR_MAP_GROUP_MAX);

	if (mode == GPIO_SCR_MAP_GROUP0) {
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_0, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_1, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_2, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_3, 3);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_0, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_1, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_2, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_3, 1);
	} else if (mode == GPIO_SCR_MAP_GROUP1) {
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_30, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_31, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_32, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_43, 3);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_30, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_31, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_32, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_43, 1);
	} else if (mode == GPIO_SCR_MAP_GROUP2) {
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_40, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_41, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_42, 3);
		gpio_ll_set_gpio_perial_mode(s_gpio.hal.hw, GPIO_43, 3);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_40, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_41, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_42, 1);
		gpio_hal_sencond_function_enable(&s_gpio.hal, GPIO_43, 1);
	}

	return BK_OK;
}

