
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "cli.h"
#include <os/os.h>
#include <common/bk_compiler.h>
#include "shell_drv.h"
#include <components/ate.h>
#include <modules/pm.h>
#include <driver/gpio.h>
#include "bk_wdt.h"
#if CONFIG_AT
#include "atsvr_port.h"
#endif
#include <driver/media_co_list.h>

#define DEV_UART        1
#define DEV_MAILBOX     2

#if CONFIG_SYS_PRINT_DEV_UART
#define LOG_DEV    DEV_UART
#define CMD_DEV    DEV_UART
#elif CONFIG_SYS_PRINT_DEV_MAILBOX
#define LOG_DEV    DEV_MAILBOX
#define CMD_DEV    DEV_MAILBOX
#endif

// #if (CMD_DEV != DEV_MAILBOX)
#if (CONFIG_SYS_CPU0)
#if CONFIG_MAILBOX
#ifndef CONFIG_FREERTOS_SMP
#define FWD_CMD_TO_MBOX
#define RECV_CMD_LOG_FROM_MBOX
#endif
#endif
#endif

#if defined(FWD_CMD_TO_MBOX)
#define MBOX_RSP_BLK_ID			0
#define MBOX_IND_BLK_ID			1

#define MBOX_FWD_PEND_NUM		2    /* (1 Rsp + 1 Ind) */
#else
#define MBOX_FWD_PEND_NUM		0
#endif

#define SHELL_WAIT_OUT_TIME		(2000) 		// 2s.

#define SHELL_TASK_WAIT_TIME	(100)		// 100ms
#define SHELL_TASK_WAKE_CYCLE	(20)		// 2s

#define SHELL_EVENT_TX_REQ  	0x01
#define SHELL_EVENT_RX_IND  	0x02
#define SHELL_EVENT_WAKEUP      0x04
#define SHELL_EVENT_TX_SND      0x08

// Modified by TUYA Start
#define SHELL_LOG_BUF1_LEN      256     // 136
#define SHELL_LOG_BUF2_LEN      100     // 80
#define SHELL_LOG_BUF3_LEN      40
// Modified by TUYA End

#if (LOG_DEV == DEV_MAILBOX)
#define SHELL_LOG_BUF1_NUM      2
#define SHELL_LOG_BUF2_NUM      4
#define SHELL_LOG_BUF3_NUM      8
#endif   //  (LOG_DEV == DEV_MAILBOX)

#if (LOG_DEV == DEV_UART)
#if CONFIG_RELEASE_VERSION
#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
#define SHELL_LOG_BUF1_NUM      1
#define SHELL_LOG_BUF2_NUM      1
#define SHELL_LOG_BUF3_NUM      1
#else
#define SHELL_LOG_BUF1_NUM      4
#define SHELL_LOG_BUF2_NUM      16
#define SHELL_LOG_BUF3_NUM      32
#endif
#else
#if !CONFIG_UART_RING_BUFF
#define SHELL_LOG_BUF1_NUM      8
#define SHELL_LOG_BUF2_NUM      40
#define SHELL_LOG_BUF3_NUM      60
#else
#define SHELL_LOG_BUF1_NUM      8
#define SHELL_LOG_BUF2_NUM      30
#define SHELL_LOG_BUF3_NUM      50
#endif
#endif
#endif   // (LOG_DEV == DEV_UART)

#define SHELL_LOG_BUF_NUM       (SHELL_LOG_BUF1_NUM + SHELL_LOG_BUF2_NUM + SHELL_LOG_BUF3_NUM)
#define SHELL_LOG_PEND_NUM      (SHELL_LOG_BUF_NUM * 2 + 4 + MBOX_FWD_PEND_NUM)
/* the worst case may be (one log + one hint) in pending queue, so twice the log_num for pending queue.*/
/* 1: RSP, 1: reserved(queue empty), 1: cmd ovf, 1: ind). */
/* MBOX_FWD_PEND_NUM (1 Rsp + 1 Ind) every slave core. */
#define SHELL_LOG_BUSY_NUM      (8)

#if (CMD_DEV == DEV_MAILBOX)
#define SHELL_RX_BUF_LEN		140
#endif
#if (CMD_DEV == DEV_UART)
#define SHELL_RX_BUF_LEN		4
#endif

#define SHELL_ASSERT_BUF_LEN	140
#define SHELL_CMD_BUF_LEN		200
#define SHELL_RSP_BUF_LEN		140
#define SHELL_IND_BUF_LEN		132

#define SHELL_RSP_QUEUE_ID	    (7)
#define SHELL_FWD_QUEUE_ID      (8)
#define SHELL_ROM_QUEUE_ID		(9)
#define SHELL_IND_QUEUE_ID		(10)

#define MAX_TRACE_ARGS      10
#define MOD_NAME_LEN        4

#define HEX_SYNC_CHAR       0xFE
#define HEX_MOD_CHAR        0xFF
#define HEX_ESC_CHAR        0xFD

#define TBL_SIZE(tbl)		(sizeof(tbl) / sizeof(tbl[0]))

typedef struct
{
	beken_semaphore_t   event_semaphore;  // will release from ISR.
	u32       event_flag;
} os_ext_event_t;

/* for async log with alloc buf */
typedef struct
{
	struct co_list_hdr hdr;
	uint16 buf_len;
	uint16 buf_type;
} print_node_t;

enum
{
	CMD_TYPE_TEXT = 0,
	CMD_TYPE_HEX,
	CMD_TYPE_BKREG,   /* patch for BK_REG tool cmd. */
	CMD_TYPE_INVALID,
};

/* patch for BK_REG tool. */
enum
{
	BKREG_WAIT_01 = 0,
	BKREG_WAIT_E0,
	BKREG_WAIT_FC,
};

typedef struct
{
	u8     rsp_buff[SHELL_RSP_BUF_LEN];
	beken_semaphore_t   rsp_buf_semaphore;

	u8     rx_buff[SHELL_RX_BUF_LEN];

	u8     cur_cmd_type;
	u8     cmd_buff[SHELL_CMD_BUF_LEN];
	u16    cmd_data_len;
	u8     cmd_ovf_hint;

	/* patch for BK_REG tool. */
	/* added one state machine for BK_REG tool cmd. */
	u8     bkreg_state;
	u8     bkreg_left_byte;
	/* patch end. */

	u8     assert_buff[SHELL_ASSERT_BUF_LEN];

	u8     log_level;
	u8     echo_enable;
	u8     log_flush;

	/* patch for AT cmd handling. */
	u8     cmd_ind_buff[SHELL_IND_BUF_LEN];
	beken_semaphore_t   ind_buf_semaphore;

#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
	/* for async log with alloc buf */
	struct co_list pending_list;
#endif
} cmd_line_t;

#define GET_BLOCK_ID(blocktag)          ((blocktag) & 0xFF)
#define GET_QUEUE_ID(blocktag)          (((blocktag) & 0x0F00) >> 8)
#define MAKE_BLOCK_TAG(blk_id, q_id)    (((blk_id) & 0xFF) | (((q_id) & 0x0F) << 8) )

typedef struct
{
	u16     blk_tag;        /* bit0~bit7: blk_id,    bit8~bit11: queue_id; */
	u16     packet_len;
} tx_packet_t;

typedef struct
{
	tx_packet_t     packet_list[SHELL_LOG_PEND_NUM];
	u16     list_out_idx;
	u16     list_in_idx;
} pending_queue_t;

typedef struct
{
	u16     blk_list[SHELL_LOG_BUSY_NUM];
	u16     list_out_idx;
	u16     list_in_idx;
	u16     free_cnt;
} busy_queue_t;

typedef struct
{
	u8   *  const  log_buf;
	u16   * const  blk_list;
	const u16      blk_num;
	const u16      blk_len;
	u16     list_out_idx;
	u16     list_in_idx;
	u16     free_blk_num;
	u32     ovf_cnt;
} free_queue_t;

#if (CONFIG_CACHE_ENABLE) && (CONFIG_LV_USE_DEMO_METER)
#define  SHELL_DECLARE_MEMORY_ATTR __attribute__((section(".sram_cache")))
#elif CONFIG_SOC_BK7258 && CONFIG_SYS_PRINT_DEV_UART
#define  SHELL_DECLARE_MEMORY_ATTR __attribute__((section(".dtcm_section")))
#else
#define  SHELL_DECLARE_MEMORY_ATTR 
#endif

static SHELL_DECLARE_MEMORY_ATTR u8    shell_log_buff1[SHELL_LOG_BUF1_NUM * SHELL_LOG_BUF1_LEN];
static SHELL_DECLARE_MEMORY_ATTR u8    shell_log_buff2[SHELL_LOG_BUF2_NUM * SHELL_LOG_BUF2_LEN];
static SHELL_DECLARE_MEMORY_ATTR u8    shell_log_buff3[SHELL_LOG_BUF3_NUM * SHELL_LOG_BUF3_LEN];
static SHELL_DECLARE_MEMORY_ATTR u16   buff1_free_list[SHELL_LOG_BUF1_NUM];
static SHELL_DECLARE_MEMORY_ATTR u16   buff2_free_list[SHELL_LOG_BUF2_NUM];
static SHELL_DECLARE_MEMORY_ATTR u16   buff3_free_list[SHELL_LOG_BUF3_NUM];

#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
#define SHELL_LOG_BUF_TYPE_ALLOC       (1)
#define SHELL_LOG_BUF_TYPE_STATIC_1    (2)
#define SHELL_LOG_BUF_TYPE_STATIC_2    (3)
#define SHELL_LOG_BUF_TYPE_STATIC_3    (4)

#define SHELL_LOG_BUF_EX1_DATA_LEN     (4)
#define SHELL_LOG_BUF_EX1_LEN          (sizeof(print_node_t) + SHELL_LOG_BUF_EX1_DATA_LEN + 4)
#define SHELL_LOG_BUF_EX1_NUM          (500)
#define MIN_ALLOC_BUF_EX1_NUM          (4)  // should less than SHELL_LOG_BUF_EX1_NUM
static SHELL_DECLARE_MEMORY_ATTR u32   shell_log_buff_ex1[(SHELL_LOG_BUF_EX1_NUM * SHELL_LOG_BUF_EX1_LEN)/sizeof(u32)];
struct co_list buff_ex1_free_list;

#define SHELL_LOG_BUF_EX2_DATA_LEN     (100)
#define SHELL_LOG_BUF_EX2_LEN          (sizeof(print_node_t) + SHELL_LOG_BUF_EX2_DATA_LEN + 4)
#define SHELL_LOG_BUF_EX2_NUM          (40)
#define MIN_ALLOC_BUF_EX2_NUM          (4)  // should less than SHELL_LOG_BUF_EX2_NUM
static SHELL_DECLARE_MEMORY_ATTR u32   shell_log_buff_ex2[(SHELL_LOG_BUF_EX2_NUM * SHELL_LOG_BUF_EX2_LEN)/sizeof(u32)];
struct co_list buff_ex2_free_list;

#define SHELL_LOG_BUF_EX3_DATA_LEN     (200)
#define SHELL_LOG_BUF_EX3_LEN          (sizeof(print_node_t) + SHELL_LOG_BUF_EX3_DATA_LEN + 4)
#define SHELL_LOG_BUF_EX3_NUM          (6)
#define MIN_ALLOC_BUF_EX3_NUM          (2)  // should less than SHELL_LOG_BUF_EX3_NUM
static SHELL_DECLARE_MEMORY_ATTR u32   shell_log_buff_ex3[(SHELL_LOG_BUF_EX3_NUM * SHELL_LOG_BUF_EX3_LEN)/sizeof(u32)];
struct co_list buff_ex3_free_list;

#if( (((SHELL_LOG_BUF_EX1_DATA_LEN < SHELL_LOG_BUF_EX2_DATA_LEN) \
    && (SHELL_LOG_BUF_EX2_DATA_LEN < SHELL_LOG_BUF_EX3_DATA_LEN)) == 0) )
#error("SHELL TASK STATIC BUF LEN ERROR!!!")
#endif
#endif // (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)

/*    queue sort ascending in blk_len.    */
static free_queue_t       free_queue[3] =
	{
		{.log_buf = shell_log_buff3, .blk_list = buff3_free_list, .blk_num = SHELL_LOG_BUF3_NUM, \
			.blk_len = SHELL_LOG_BUF3_LEN, .list_out_idx = 0, .list_in_idx = 0, \
			.free_blk_num = SHELL_LOG_BUF3_NUM, .ovf_cnt = 0},

		{.log_buf = shell_log_buff2, .blk_list = buff2_free_list, .blk_num = SHELL_LOG_BUF2_NUM, \
			.blk_len = SHELL_LOG_BUF2_LEN, .list_out_idx = 0, .list_in_idx = 0, \
			.free_blk_num = SHELL_LOG_BUF2_NUM, .ovf_cnt = 0},

		{.log_buf = shell_log_buff1, .blk_list = buff1_free_list, .blk_num = SHELL_LOG_BUF1_NUM, \
			.blk_len = SHELL_LOG_BUF1_LEN, .list_out_idx = 0, .list_in_idx = 0, \
			.free_blk_num = SHELL_LOG_BUF1_NUM, .ovf_cnt = 0},
	};

#if (CONFIG_CACHE_ENABLE) && (CONFIG_LV_USE_DEMO_METER)
static __attribute__((section(".sram_cache")))  busy_queue_t       log_busy_queue;
static __attribute__((section(".sram_cache")))  pending_queue_t    pending_queue;
#else
static busy_queue_t       log_busy_queue;
static pending_queue_t    pending_queue;
#endif

#if (CONFIG_CACHE_ENABLE) && (CONFIG_LV_USE_DEMO_METER)
static __attribute__((section(".sram_cache")))  cmd_line_t  cmd_line_buf;
#else
static cmd_line_t  cmd_line_buf;
#endif

#if (LOG_DEV == DEV_UART)
static shell_dev_t * log_dev = &shell_uart;
#endif
#if (LOG_DEV == DEV_MAILBOX)
static shell_dev_t * log_dev = &shell_dev_mb;
#endif

#if (CMD_DEV == DEV_UART)
static shell_dev_t * cmd_dev = &shell_uart;
#endif
#if (CMD_DEV == DEV_MAILBOX)
static shell_dev_t * cmd_dev = &shell_dev_mb;
#endif

#if (CONFIG_SYS_CPU0)
static const char	 shell_fault_str[] = "\r\n!!some LOGs discarded!!\r\n";
static const u16     shell_fault_str_len = sizeof(shell_fault_str) - 1;
#endif
#if (!CONFIG_SYS_CPU0)
static const char	 shell_fault_str[] = "\r\n!!CPUx:some LOGs discarded!!\r\n";
static const u16     shell_fault_str_len = sizeof(shell_fault_str) - 1;
#endif

#if defined(FWD_CMD_TO_MBOX) || defined(RECV_CMD_LOG_FROM_MBOX)

typedef struct
{
	log_cmd_t	rsp_buf;
	log_cmd_t   ind_buf;
} fwd_slave_data_t;

static fwd_slave_data_t  ipc_fwd_data;

static shell_dev_ipc_t * ipc_dev = &shell_dev_ipc;

static int result_fwd(int blk_id);
static u32 shell_ipc_rx_indication(u16 cmd, log_cmd_t *data, u16 cpu_id);

#endif
static void shell_cmd_free_buf_ex(print_node_t *node);

static const char	 shell_cmd_ovf_str[] = "\r\n!!some CMDs lost!!\r\n";
static const u16     shell_cmd_ovf_str_len = sizeof(shell_cmd_ovf_str) - 1;
static const char  * shell_prompt_str[2] = {"\r\n$", "\r\n#"};

static u8     shell_init_ok = bFALSE;
static u8     fault_hint_print = 0;
static u32    shell_log_overflow = 0;
static u32    shell_log_count = 0;
static u8     prompt_str_idx = 0;

static u32    shell_pm_wake_time = SHELL_TASK_WAKE_CYCLE;   // wait cycles before enter sleep.
static u8     shell_pm_wake_flag = 1;

#if (CONFIG_CPU_CNT > 1) && (LOG_DEV != DEV_MAILBOX)
/* patch for multi-cpu dump. */
static u8     shell_log_owner_cpu = 0;
static u8     shell_log_req_cpu = 0;
#endif

#ifdef CONFIG_FREERTOS_SMP
#include "spinlock.h"
static volatile spinlock_t shell_spin_lock = SPIN_LOCK_INIT;
#endif // CONFIG_FREERTOS_SMP

static inline uint32_t shell_task_enter_critical()
{
	uint32_t flags = rtos_disable_int();

#ifdef CONFIG_FREERTOS_SMP
	spin_lock(&shell_spin_lock);
#endif // CONFIG_FREERTOS_SMP

	return flags;
}

static inline void shell_task_exit_critical(uint32_t flags)
{
#ifdef CONFIG_FREERTOS_SMP
	spin_unlock(&shell_spin_lock);
#endif // CONFIG_FREERTOS_SMP

	rtos_enable_int(flags);
}

#if 1

static os_ext_event_t   shell_task_event;

static bool_t create_shell_event(void)
{
	shell_task_event.event_flag = 0;
	rtos_init_semaphore(&shell_task_event.event_semaphore, 1);

	return bTRUE;
}

/* this API may be called from ISR. */
bool_t set_shell_event(u32 event_flag)
{
	u32  int_mask;

	int_mask = shell_task_enter_critical();

	shell_task_event.event_flag |= event_flag;

	shell_task_exit_critical(int_mask);

	rtos_set_semaphore(&shell_task_event.event_semaphore);

	return bTRUE;
}

u32 wait_any_event(u32 timeout)
{
	u32  int_mask;
	u32  event_flag;

	int  result;

	while(bTRUE)
	{
		int_mask = shell_task_enter_critical();

		event_flag = shell_task_event.event_flag;
		shell_task_event.event_flag = 0;

		shell_task_exit_critical(int_mask);

		if((event_flag != 0) || (timeout == 0))
		{
			return event_flag;
		}
		else
		{
			result = rtos_get_semaphore(&shell_task_event.event_semaphore, timeout);

			if(result == kTimeoutErr)
				return 0;
		}
	}

}

#else

static beken_semaphore_t   shell_semaphore;  // will release from ISR.

static bool_t create_shell_event(void)
{
	rtos_init_semaphore(&shell_semaphore, 1);

	return bTRUE;
}

static bool_t set_shell_event(u32 event_flag)
{
	(void)event_flag;

	rtos_set_semaphore(&shell_semaphore);

	return bTRUE;
}

static u32 wait_any_event(u32 timeout)
{
	int result;

	result = rtos_get_semaphore(&shell_semaphore, timeout);

	if(result == kTimeoutErr)
		return 0;

	return SHELL_EVENT_RX_IND;
}
#endif

static void tx_req_process(void);

static u8 * alloc_log_blk(u16 log_len, u16 *blk_tag)
{
	u16    free_blk_id;
	u8     queue_id;
	u8     ovf_cnt_saved = 0;
	free_queue_t * free_q;
	u8     *       blk_buf = NULL;

	u32  int_mask = shell_task_enter_critical();

	for(queue_id = 0; queue_id < TBL_SIZE(free_queue); queue_id++)
	{
		free_q = &free_queue[queue_id];

		/*    queue ascending in blk_len.    */
		if(free_q->blk_len < log_len)
			continue;

		if(free_q->free_blk_num > 0)
		{
			free_blk_id = free_q->blk_list[free_q->list_out_idx];

			// free_q->list_out_idx = (free_q->list_out_idx + 1) % free_q->blk_num;
			if((free_q->list_out_idx + 1) < free_q->blk_num)
				free_q->list_out_idx++;
			else
				free_q->list_out_idx = 0;

			free_q->free_blk_num--;

			blk_buf = &free_q->log_buf[free_blk_id * free_q->blk_len];
			*blk_tag = MAKE_BLOCK_TAG(free_blk_id, queue_id);

			break;
		}
		else
		{
			if(ovf_cnt_saved == 0)
			{
				free_q->ovf_cnt++;
				ovf_cnt_saved = 1;
			}
		}
	}

	if(blk_buf == NULL)
	{
		shell_log_overflow++;
	}
	else
	{
		fault_hint_print = 0;
		shell_log_count++;
	}

	shell_task_exit_critical(int_mask);

	return blk_buf;
}

static bool_t free_log_blk(u16 block_tag)
{
	u8      queue_id = GET_QUEUE_ID(block_tag);
	u16     blk_id = GET_BLOCK_ID(block_tag);
	free_queue_t *free_q;

	if(queue_id >= TBL_SIZE(free_queue))
		return bFALSE;

	free_q = &free_queue[queue_id];

	if(blk_id >= free_q->blk_num)
		return bFALSE;

	//disable_interrupt(); // called from tx-complete only, don't lock interrupt.

	free_q->blk_list[free_q->list_in_idx] = blk_id;

	//free_q->list_in_idx = (free_q->list_in_idx + 1) % free_q->blk_num;
	if((free_q->list_in_idx + 1) < free_q->blk_num)
		free_q->list_in_idx++;
	else
		free_q->list_in_idx = 0;

	free_q->free_blk_num++;

	//enable_interrupt(); // called from tx-complete only, don't lock interrupt.

	return bTRUE;
}

/* call this in interrupt !* DISABLED *! context. */
static void push_pending_queue(u16 blk_tag, u16 data_len)
{
	//get_shell_mutex();

	pending_queue.packet_list[pending_queue.list_in_idx].blk_tag = blk_tag;
	pending_queue.packet_list[pending_queue.list_in_idx].packet_len = data_len;

	//pending_queue.list_in_idx = (pending_queue.list_in_idx + 1) % SHELL_LOG_PEND_NUM;
	if((pending_queue.list_in_idx + 1) < SHELL_LOG_PEND_NUM)
		pending_queue.list_in_idx++;
	else
		pending_queue.list_in_idx = 0;

	//release_shell_mutex();

	return;
}

/* call this in interrupt !* DISABLED *! context. */
static void pull_pending_queue(u16 *blk_tag, u16 *data_len)
{
	*blk_tag     = pending_queue.packet_list[pending_queue.list_out_idx].blk_tag;
	*data_len   = pending_queue.packet_list[pending_queue.list_out_idx].packet_len;

	//pending_queue.list_out_idx = (pending_queue.list_out_idx + 1) % SHELL_LOG_PEND_NUM;
	if((pending_queue.list_out_idx + 1) < SHELL_LOG_PEND_NUM)
		pending_queue.list_out_idx++;
	else
		pending_queue.list_out_idx = 0;

	return;
}

int shell_assert_out(bool bContinue, char * format, ...);

/* call from cmd TX ISR. */
static int cmd_tx_complete(u8 *pbuf, u16 buf_tag)
{
	u8      queue_id = GET_QUEUE_ID(buf_tag);
	u16     blk_id = GET_BLOCK_ID(buf_tag);

	/* rsp ok ?? */
	if( queue_id == SHELL_RSP_QUEUE_ID )    /* rsp. */
	{
		/* it is called from cmd_dev tx ISR. */

		if ( (pbuf != cmd_line_buf.rsp_buff) || (blk_id != 0) )
		{
			/* something wrong!!! */
			shell_assert_out(bTRUE, "FAULT: in rsp.\r\n");
		}

		/* rsp compelete, rsp_buff can be used for next cmd/response. */
		rtos_set_semaphore(&cmd_line_buf.rsp_buf_semaphore);

		return 1;
	}

	if( queue_id == SHELL_IND_QUEUE_ID )    /* cmd_ind. */
	{
		/* it is called from cmd_dev tx ISR. */

		if ( (pbuf != cmd_line_buf.cmd_ind_buff) || (blk_id != 0) )
		{
			/* something wrong!!! */
			shell_assert_out(bTRUE, "FAULT: indication.\r\n");
		}

		/* indication tx compelete, cmd_ind_buff can be used for next cmd_indication. */
		rtos_set_semaphore(&cmd_line_buf.ind_buf_semaphore);

		return 1;
	}

	if( queue_id == SHELL_ROM_QUEUE_ID )    /* fault hints buffer, point to flash. */
	{
		/* it is called from cmd_dev tx ISR. */

		if (blk_id == 1)
		{
			if(pbuf != (u8 *)shell_cmd_ovf_str)
			{
				/* something wrong!!! */
				shell_assert_out(bTRUE, "FATAL:t-%x,p-%x\r\n", buf_tag, pbuf);
			}

			cmd_line_buf.cmd_ovf_hint = 0;

			return 1;
		}
	}

	if( queue_id == SHELL_FWD_QUEUE_ID )    /* slave buffer. */
	{
		#if defined(FWD_CMD_TO_MBOX)
		result_fwd(blk_id);
		return 1;
		#endif
	}

	return 0;
}
/* call from TX ISR. */
static void shell_cmd_tx_complete(u8 *pbuf, u16 buf_tag)
{
	u32  int_mask = shell_task_enter_critical();
	int  tx_handled = cmd_tx_complete(pbuf, buf_tag);

	if(tx_handled == 0)  /* not handled. */
	{
		/*        FAULT !!!!      */
		shell_assert_out(bTRUE, "FATAL:%x,\r\n", buf_tag);
	}

	shell_task_exit_critical(int_mask);
}

/* call from log TX ISR. */
static int log_tx_complete(u8 *pbuf, u16 buf_tag)
{
	u16     block_tag;
	u8      queue_id = GET_QUEUE_ID(buf_tag);
	u16     blk_id = GET_BLOCK_ID(buf_tag);
	free_queue_t *free_q;

	if( queue_id == SHELL_ROM_QUEUE_ID )    /* fault hints buffer, point to flash. */
	{
		/* it is called from log_dev tx ISR. */

		if (blk_id == 0)
		{
			if(pbuf != (u8 *)shell_fault_str)
			{
				/* something wrong!!! */
				shell_assert_out(bTRUE, "FATAL:t-%x,p-%x\r\n", buf_tag, pbuf);
			}

			return 1;
		}
	}

	if (queue_id < TBL_SIZE(free_queue))   /* from log busy queue. */
	{
		/* it is called from log_dev tx ISR. */

		free_q = &free_queue[queue_id];

		block_tag = log_busy_queue.blk_list[log_busy_queue.list_out_idx];

		if( ( buf_tag != block_tag ) || (blk_id >= free_q->blk_num) ||
			( (&free_q->log_buf[blk_id * free_q->blk_len]) != pbuf) )
		{
			/* something wrong!!! */
			/*        FAULT !!!!      */
			shell_assert_out(bTRUE, "FATAL:%x,%x\r\n", buf_tag, block_tag);

			return -1;
		}

		/* de-queue from busy queue. */
		//log_busy_queue.list_out_idx = (log_busy_queue.list_out_idx + 1) % SHELL_LOG_BUSY_NUM;
		if((log_busy_queue.list_out_idx + 1) < SHELL_LOG_BUSY_NUM)
			log_busy_queue.list_out_idx++;
		else
			log_busy_queue.list_out_idx = 0;

		log_busy_queue.free_cnt++;

		/* free buffer to queue. */
		free_log_blk(block_tag);

		return 1;
	}

	return 0;
}

/* call from TX ISR. */
static void shell_log_tx_complete(u8 *pbuf, u16 buf_tag)
{
	u32  int_mask = shell_task_enter_critical();

	int log_tx_req = log_tx_complete(pbuf, buf_tag);

	if(log_tx_req == 1)
	{
		//set_shell_event(SHELL_EVENT_TX_REQ);  // notify shell task to process the log tx.
		tx_req_process();
	}
	else if(log_tx_req == 0)  /* not handled. */
	{
		/*        FAULT !!!!      */
		shell_assert_out(bTRUE, "FATAL:%x,\r\n", buf_tag);
	}

	shell_task_exit_critical(int_mask);
}

/* call from TX ISR. */
static void shell_tx_complete(u8 *pbuf, u16 buf_tag)
{
	u32  int_mask = shell_task_enter_critical();

	int tx_req = 0;

	tx_req = cmd_tx_complete(pbuf, buf_tag);

	if(tx_req == 0) /* not a cmd tx event, maybe it is a log tx event. */
	{
		tx_req = log_tx_complete(pbuf, buf_tag);
	}

	if(tx_req == 1)
	{
		//set_shell_event(SHELL_EVENT_TX_REQ);  // notify shell task to process the log tx.
		tx_req_process();
	}
	else if(tx_req == 0)  /* not handled. */
	{
		/*        FAULT !!!!      */
		shell_assert_out(bTRUE, "FATAL:%x,\r\n", buf_tag);
	}

	shell_task_exit_critical(int_mask);
}

/* call from RX ISR. */
static void shell_rx_indicate(void)
{
	set_shell_event(SHELL_EVENT_RX_IND);

	return;
}

static u16 append_link_data_byte(u8 * link_buf, u16 buf_len, u8 * data_ptr, u16 data_len)
{
	u16   cnt = 0, i;

	for(i = 0; i < data_len; i++)
	{
		if( (*data_ptr == HEX_SYNC_CHAR) ||
			(*data_ptr == HEX_ESC_CHAR) )
		{
			if(cnt < (buf_len - 1))
			{
				link_buf[cnt] = HEX_ESC_CHAR;
				cnt++;
				link_buf[cnt] = (*data_ptr) ^ HEX_MOD_CHAR;
				cnt++;
			}
		}
		else
		{
			if(cnt < buf_len)
			{
				link_buf[cnt] = (*data_ptr);
				cnt++;
			}
		}

		data_ptr++;
	}

	return cnt;
}

static bool_t echo_out(u8 * echo_str, u16 len)
{
	u16	 wr_cnt;

	if(len == 0)
		return bTRUE;

	wr_cnt = cmd_dev->dev_drv->write_echo(cmd_dev, echo_str, len);

	return (wr_cnt == len);
}

static void cmd_info_out(u8 * msg_buf, u16 msg_len, u16 blk_tag)
{
	u32  int_mask = shell_task_enter_critical();

	if(log_dev != cmd_dev)
	{
		/* dedicated device for cmd, don't enqueue the msg to pending queue. */
		/* send to cmd dev directly. */
		/* should have a count semaphore for write_asyn calls for rsp/ind/cmd_hint & slave rsp/ind. *
		 * otherwise there will be coupled with driver, drv tx_queue_len MUST be >= 5. */
		cmd_dev->dev_drv->write_async(cmd_dev, msg_buf, msg_len, blk_tag);

	}
	else
	{
		/* shared device for cmd & log, push the rsp msg to pending queue. */
		push_pending_queue(blk_tag, msg_len);

		//set_shell_event(SHELL_EVENT_TX_REQ);  // notify shell task to process the log tx. can't be called in int-disabled context.
		tx_req_process();
	}

	shell_task_exit_critical(int_mask);
}

/*    NOTICE:  this can only be called by shell task internally (cmd handler). */
/*             it is not a re-enterance function becaue of using rsp_buff. */
static bool_t cmd_rsp_out(u8 * rsp_msg, u16 msg_len)
{
	u16    rsp_blk_tag = MAKE_BLOCK_TAG(0, SHELL_RSP_QUEUE_ID);

	if(rsp_msg != cmd_line_buf.rsp_buff)
	{
		if(msg_len > sizeof(cmd_line_buf.rsp_buff))
		{
			msg_len = sizeof(cmd_line_buf.rsp_buff);;
		}

		memcpy(cmd_line_buf.rsp_buff, rsp_msg, msg_len);
	}

	cmd_info_out(cmd_line_buf.rsp_buff, msg_len, rsp_blk_tag);

	return bTRUE;
}

/* it is not a re-enterance function, should sync using ind_buf_semaphore. */
static bool_t cmd_ind_out(u8 * ind_msg, u16 msg_len)
{
	u16    ind_blk_tag = MAKE_BLOCK_TAG(0, SHELL_IND_QUEUE_ID);

	if(ind_msg != cmd_line_buf.cmd_ind_buff)
	{
		if(msg_len > sizeof(cmd_line_buf.cmd_ind_buff))
		{
			msg_len = sizeof(cmd_line_buf.cmd_ind_buff);;
		}

		memcpy(cmd_line_buf.cmd_ind_buff, ind_msg, msg_len);
	}

	cmd_info_out(cmd_line_buf.cmd_ind_buff, msg_len, ind_blk_tag);

	return bTRUE;
}

static bool_t cmd_hint_out(void)
{
	u16    hint_blk_tag = MAKE_BLOCK_TAG(1, SHELL_ROM_QUEUE_ID);

	cmd_info_out((u8 *)shell_cmd_ovf_str, shell_cmd_ovf_str_len, hint_blk_tag);

	return bTRUE;
}

static bool_t log_hint_out(void)
{
	if(fault_hint_print)	/* sent one hint since last allocation fail.*/
		return bTRUE;

	u16    hint_blk_tag = MAKE_BLOCK_TAG(0, SHELL_ROM_QUEUE_ID);

	u32  int_mask = shell_task_enter_critical();

	push_pending_queue(hint_blk_tag, shell_fault_str_len);

	//set_shell_event(SHELL_EVENT_TX_REQ);  // notify shell task to process the log tx.
	tx_req_process();

	fault_hint_print = 1;

	shell_task_exit_critical(int_mask);

	return bTRUE;
}

/* call this in interrupt !* DISABLED *! context. */
static void tx_req_process(void)
{
	u8		*packet_buf = NULL;
	u16		block_tag;
	u16		log_len;
	u16		tx_ready;
	u16		blk_id;
	u8		queue_id;
	free_queue_t *free_q;

	/* maybe tx_req is from tx_complete_callback, check if there any log in queue. */
	if(pending_queue.list_out_idx == pending_queue.list_in_idx)  /* queue empty! */
		return;

	if(log_busy_queue.free_cnt == 0)
		return;

	tx_ready = 0;

	log_dev->dev_drv->io_ctrl(log_dev, SHELL_IO_CTRL_GET_STATUS, &tx_ready);

	if(tx_ready == 0)
		return;

	/**    ====     POP from pending queue     ====    **/
	pull_pending_queue(&block_tag, &log_len);

	queue_id = GET_QUEUE_ID(block_tag);
	blk_id = GET_BLOCK_ID(block_tag);

	if (queue_id < TBL_SIZE(free_queue))
	{
		free_q = &free_queue[queue_id];

		if(blk_id < free_q->blk_num)
		{
			packet_buf = &free_q->log_buf[blk_id * free_q->blk_len];
		}
	}
	else if(queue_id == SHELL_RSP_QUEUE_ID)
	{
		packet_buf = cmd_line_buf.rsp_buff;

		if((log_dev != cmd_dev) || (blk_id != 0))
		{
			shell_assert_out(bTRUE, "xFATAL: in Tx_req\r\n");
			/*		  FAULT !!!!	  */
			/* if log_dev is not the same with cmd_dev,
			 * rsp will not be pushed into pending queue.
			 */
		}
	}
	else if(queue_id == SHELL_IND_QUEUE_ID)
	{
		packet_buf = cmd_line_buf.cmd_ind_buff;

		if((log_dev != cmd_dev) || (blk_id != 0))
		{
			shell_assert_out(bTRUE, "xFATAL: in Tx_req\r\n");
			/*		  FAULT !!!!	  */
			/* if log_dev is not the same with cmd_dev,
			 * indication will not be pushed into pending queue.
			 */
		}
	}
	else if(queue_id == SHELL_ROM_QUEUE_ID)
	{
		if(blk_id == 0)
		{
			packet_buf = (u8 *)shell_fault_str;
		}
		else if(blk_id == 1)
		{
			packet_buf = (u8 *)shell_cmd_ovf_str;
			if(log_dev != cmd_dev)
			{
				shell_assert_out(bTRUE, "xFATAL: in Tx_req id=%x\r\n", blk_id);
				/*		  FAULT !!!!	  */
				/* if log_dev is not the same with cmd_dev,
				 * cmd_hint will not be pushed into pending queue.
				 */
			}
		}
		else
		{
				/*		  FAULT !!!!	  */
				shell_assert_out(bTRUE, "xFATAL: in Tx_req id=%x\r\n", blk_id);
		}
	}
	#if defined(FWD_CMD_TO_MBOX)
	else if(queue_id == SHELL_FWD_QUEUE_ID)
	{
		if(blk_id == MBOX_RSP_BLK_ID)
		{
			packet_buf = (u8 *)ipc_fwd_data.rsp_buf.buf;
		}
		else if(blk_id == MBOX_IND_BLK_ID)
		{
			packet_buf = (u8 *)ipc_fwd_data.ind_buf.buf;
			if(log_dev != cmd_dev)
			{
				shell_assert_out(bTRUE, "xFATAL: in Tx_req id=%x\r\n", blk_id);
				/*		  FAULT !!!!	  */
				/* if log_dev is not the same with cmd_dev,
				 * fwd_data will not be pushed into pending queue.
				 */
			}
		}
		else
		{
				/*		  FAULT !!!!	  */
				shell_assert_out(bTRUE, "xFATAL: in Tx_req id=%x\r\n", blk_id);
		}
	}
	#endif
	else
	{
		/*		  FAULT !!!!	  */
		shell_assert_out(bTRUE, "xFATAL: in Tx_req %x.\r\n", block_tag);
	}

	if(packet_buf == NULL)
		return;

	/* rom & rsp buff not enter busy-queue. */
	if(queue_id < TBL_SIZE(free_queue))
	{
		log_busy_queue.free_cnt--;
		log_busy_queue.blk_list[log_busy_queue.list_in_idx] = block_tag;
		//log_busy_queue.list_in_idx = (log_busy_queue.list_in_idx + 1) % SHELL_LOG_BUSY_NUM;
		if((log_busy_queue.list_in_idx + 1) < SHELL_LOG_BUSY_NUM)
			log_busy_queue.list_in_idx++;
		else
			log_busy_queue.list_in_idx = 0;
	}

	log_dev->dev_drv->write_async(log_dev, packet_buf, log_len, block_tag); /* send to log dev driver. */
	/* if driver return 0, should free log-block or not de-queue pending queue and try again. */
	/* if return 1, push log-block into busy queue is OK. */

	return;
}

int shell_trace_out( u32 trace_id, ... );
int shell_spy_out( u16 spy_id, u8 * data_buf, u16 data_len);

static void rx_ind_process(void)
{
	u16   read_cnt, buf_len, echo_len;
	u16   i = 0;
	u8    cmd_rx_done = bFALSE, need_backspace = bFALSE;

	if(cmd_dev->dev_type == SHELL_DEV_MAILBOX)
	{
		buf_len = SHELL_RX_BUF_LEN;
	}
	else /* if(cmd_dev->dev_type == SHELL_DEV_UART) */
	{
		buf_len = 1;  /* for UART device, read one by one. */
	}

	while(bTRUE)
	{
		u8  * rx_temp_buff = &cmd_line_buf.rx_buff[0];

		read_cnt = cmd_dev->dev_drv->read(cmd_dev, rx_temp_buff, buf_len);

		echo_len = 0;

		for(i = 0; i < read_cnt; i++)
		{
			if(cmd_line_buf.cur_cmd_type == CMD_TYPE_INVALID)
			{
				echo_len++;

				if((rx_temp_buff[i] >= 0x20) && (rx_temp_buff[i] < 0x7f))
				{
					cmd_line_buf.cur_cmd_type = CMD_TYPE_TEXT;

					cmd_line_buf.cmd_data_len = 0;
					cmd_line_buf.cmd_buff[cmd_line_buf.cmd_data_len] = rx_temp_buff[i];
					cmd_line_buf.cmd_data_len++;

					continue;
				}

				/* patch for BK_REG tool. */
				if(cmd_line_buf.bkreg_state == BKREG_WAIT_01)
				{
					if(rx_temp_buff[i] == 0x01)
						cmd_line_buf.bkreg_state = BKREG_WAIT_E0;
				}
				else if(cmd_line_buf.bkreg_state == BKREG_WAIT_E0)
				{
					if(rx_temp_buff[i] == 0xE0)
						cmd_line_buf.bkreg_state = BKREG_WAIT_FC;
					else if(rx_temp_buff[i] != 0x01)
						cmd_line_buf.bkreg_state = BKREG_WAIT_01;
				}
				else if(cmd_line_buf.bkreg_state == BKREG_WAIT_FC)
				{
					if(rx_temp_buff[i] == 0xFC)
					{
						cmd_line_buf.cur_cmd_type = CMD_TYPE_BKREG;

						cmd_line_buf.cmd_buff[0] = 0x01;
						cmd_line_buf.cmd_buff[1] = 0xE0;
						cmd_line_buf.cmd_buff[2] = 0xFC;

						cmd_line_buf.cmd_data_len = 3;

						echo_len = 0;   // cann't echo anything.

						continue;
					}
					else if(rx_temp_buff[i] != 0x01)
						cmd_line_buf.bkreg_state = BKREG_WAIT_01;
					else
						cmd_line_buf.bkreg_state = BKREG_WAIT_E0;
				}

			}

			if(cmd_line_buf.cur_cmd_type == CMD_TYPE_TEXT)
			{
				echo_len++;
				if(rx_temp_buff[i] == '\b')
				{
					if(cmd_line_buf.cmd_data_len > 0)
					{
						cmd_line_buf.cmd_data_len--;

						if(cmd_line_buf.cmd_data_len == 0)
							need_backspace = bTRUE;
					}
				}
				else if((rx_temp_buff[i] == '\n') || (rx_temp_buff[i] == '\r'))
				{
					if(cmd_line_buf.cmd_data_len < sizeof(cmd_line_buf.cmd_buff))
					{
						cmd_line_buf.cmd_buff[cmd_line_buf.cmd_data_len] = 0;
					}
					else
					{
						cmd_line_buf.cmd_buff[cmd_line_buf.cmd_data_len - 1] = 0;  // in case cmd_data_len overflow.
					}

					cmd_rx_done = bTRUE;
					break;
				}
				else if((rx_temp_buff[i] >= 0x20))
				{
					if(cmd_line_buf.cmd_data_len < sizeof(cmd_line_buf.cmd_buff))
					{
						cmd_line_buf.cmd_buff[cmd_line_buf.cmd_data_len] = rx_temp_buff[i];
						cmd_line_buf.cmd_data_len++;
					}
				}

			}

			/* patch for BK_REG tool. */
			if(cmd_line_buf.cur_cmd_type == CMD_TYPE_BKREG)
			{
				echo_len = 0;   // cann't echo anything.

				/* p[0] = 0x1, p[1]=0xe0, p[2]=0xfc, p[3]=len. */
				if(cmd_line_buf.cmd_data_len == 3)
				{
					cmd_line_buf.bkreg_left_byte = rx_temp_buff[i] + 1;  // +1, because will -1 in next process.

					if((cmd_line_buf.bkreg_left_byte + 3) >= sizeof(cmd_line_buf.cmd_buff))  // 3 bytes of header + 1 byte of len.
					{
						cmd_line_buf.cmd_data_len = 0;

						cmd_rx_done = bTRUE;
						break;
					}
				}

				if(cmd_line_buf.cmd_data_len < sizeof(cmd_line_buf.cmd_buff))
				{
					cmd_line_buf.cmd_buff[cmd_line_buf.cmd_data_len] = rx_temp_buff[i];
					cmd_line_buf.cmd_data_len++;
				}

				cmd_line_buf.bkreg_left_byte--;

				if(cmd_line_buf.bkreg_left_byte == 0)
				{
					cmd_rx_done = bTRUE;
					break;
				}
			}
		}

		if( cmd_rx_done )
		{
			/* patch for BK_REG tool. */
			if(cmd_line_buf.cur_cmd_type == CMD_TYPE_BKREG)
			{
				break;  // cann't echo anything.
			}

			if(cmd_line_buf.echo_enable)
			{
				echo_out(&rx_temp_buff[0], echo_len);
				echo_out((u8 *)"\r\n", 2);
			}

			break;
		}
		else
		{
			/* patch for BK_REG tool. */
			if( (cmd_line_buf.cur_cmd_type == CMD_TYPE_BKREG) ||
				((cmd_line_buf.cur_cmd_type == CMD_TYPE_INVALID) && (cmd_line_buf.bkreg_state != BKREG_WAIT_01)) )
			{
				 // cann't echo anything.
			}
			else if(cmd_line_buf.echo_enable)
			{
				if(echo_len > 0)
				{
					if( (rx_temp_buff[echo_len - 1] == '\b') ||
						(rx_temp_buff[echo_len - 1] == 0x7f) ) /* DEL */
					{
						echo_len--;
						if((cmd_line_buf.cmd_data_len > 0) || need_backspace)
							echo_out((u8 *)"\b \b", 3);
					}

					u8    cr_lf = 0;

					if(echo_len == 1)
					{
						if( (rx_temp_buff[echo_len - 1] == '\r') ||
							(rx_temp_buff[echo_len - 1] == '\n') )
						{
							cr_lf = 1;
						}
					}
					else if(echo_len == 2)
					{
						if( (memcmp(rx_temp_buff, "\r\n", 2) == 0) ||
							(memcmp(rx_temp_buff, "\n\r", 2) == 0) )
						{
							cr_lf = 1;
						}
					}

					if(cr_lf != 0)
					{
						echo_out((u8 *)shell_prompt_str[prompt_str_idx], 3);
						echo_len = 0;
					}
				}
				echo_out(rx_temp_buff, echo_len);
			}
		}

		if(read_cnt < buf_len) /* all data are read out. */
			break;
	}

	if(read_cnt < buf_len) /* all data are read out. */
	{
	}
	else  /* cmd pends in buffer, handle it in new loop cycle. */
	{
		set_shell_event(SHELL_EVENT_RX_IND);
	}

	/* can re-use *buf_len*. */
	if( cmd_rx_done )
	{
		if(cmd_line_buf.cur_cmd_type == CMD_TYPE_TEXT)
		{
			if(cmd_line_buf.cmd_ovf_hint == 0)
			{
				u16		rx_ovf = 0;
				cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_GET_RX_STATUS, &rx_ovf);

				if(rx_ovf != 0)
				{
					cmd_hint_out();
					cmd_line_buf.cmd_ovf_hint = 1;
				}
			}

			rtos_get_semaphore(&cmd_line_buf.rsp_buf_semaphore, SHELL_WAIT_OUT_TIME);

			cmd_line_buf.rsp_buff[0] = 0;
			/* handle command. */
			if( cmd_line_buf.cmd_data_len > 0 )
			#if defined(CONFIG_AT) && defined(CONFIG_SYS_CPU0)
			#if (CONFIG_UART_PRINT_PORT == AT_UART_PORT_CFG)
				atsvr_msg_get_input((char *)cmd_line_buf.cmd_buff, cmd_line_buf.cmd_data_len, (char *)cmd_line_buf.rsp_buff, SHELL_RSP_BUF_LEN - 4);
			#else
				handle_shell_input( (char *)cmd_line_buf.cmd_buff, cmd_line_buf.cmd_data_len, (char *)cmd_line_buf.rsp_buff, SHELL_RSP_BUF_LEN - 4 );
			#endif
			#else
				handle_shell_input( (char *)cmd_line_buf.cmd_buff, cmd_line_buf.cmd_data_len, (char *)cmd_line_buf.rsp_buff, SHELL_RSP_BUF_LEN - 4 );
			#endif
			cmd_line_buf.rsp_buff[SHELL_RSP_BUF_LEN - 4] = 0;

			buf_len = strlen((char *)cmd_line_buf.rsp_buff);
			if(buf_len > (SHELL_RSP_BUF_LEN - 4))
				buf_len = (SHELL_RSP_BUF_LEN - 4);
			buf_len += sprintf((char *)&cmd_line_buf.rsp_buff[buf_len], shell_prompt_str[prompt_str_idx]);

			cmd_rsp_out(cmd_line_buf.rsp_buff, buf_len);
		}

		/* patch for BK_REG tool. */
		if(cmd_line_buf.cur_cmd_type == CMD_TYPE_BKREG)
		{
			if(cmd_line_buf.cmd_data_len > 3)
			{
#if CONFIG_BKREG
				extern int bkreg_run_command(const char *cmd, int flag);

				bkreg_run_command((const char *)&cmd_line_buf.cmd_buff[0], (int)cmd_line_buf.cmd_data_len);
#endif // CONFIG_BKREG
			}
		}

		cmd_line_buf.cur_cmd_type = CMD_TYPE_INVALID;  /* reset cmd line to interpret new cmd. */
		cmd_line_buf.cmd_data_len = 0;
		cmd_line_buf.bkreg_state = BKREG_WAIT_01;	/* reset state machine. */
	}

	return;
}

extern gpio_id_t bk_uart_get_rx_gpio(uart_id_t id);

static void shell_rx_wakeup(int gpio_id);

static bk_err_t shell_enter_deep_sleep(uint64_t sleep_time, void *args)
{
	u32		flush_log = cmd_line_buf.log_flush;

	uart_wait_tx_over();

	log_dev->dev_drv->io_ctrl(log_dev, SHELL_IO_CTRL_TX_SUSPEND, (void *)flush_log);
	cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_RX_SUSPEND, NULL);

	return BK_OK;
}

static void shell_power_save_enter(void)
{
	u32		flush_log = cmd_line_buf.log_flush;

	log_dev->dev_drv->io_ctrl(log_dev, SHELL_IO_CTRL_TX_SUSPEND, (void *)flush_log);
	cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_RX_SUSPEND, NULL);

	if(cmd_dev->dev_type == SHELL_DEV_UART)
	{
		u8   uart_port = UART_ID_MAX;

		cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_GET_UART_PORT, &uart_port);

		bk_uart_pm_backup(uart_port);

		u32  gpio_id = bk_uart_get_rx_gpio(uart_port);

		bk_gpio_register_isr(gpio_id, (gpio_isr_t)shell_rx_wakeup);
	}
}

static void shell_power_save_exit(void)
{
	log_dev->dev_drv->io_ctrl(log_dev, SHELL_IO_CTRL_TX_RESUME, NULL);
	cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_RX_RESUME, NULL);
}

static void wakeup_process(void)
{
	bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_LOG, 0, 0);
	shell_pm_wake_flag = 1;
	shell_pm_wake_time = SHELL_TASK_WAKE_CYCLE;
}

static void shell_rx_wakeup(int gpio_id)
{
	wakeup_process();
	set_shell_event(SHELL_EVENT_WAKEUP);

	shell_log_raw_data((const u8*)"wakeup\r\n", sizeof("wakeup\r\n") - 1);

	if(cmd_dev->dev_type == SHELL_DEV_UART)
	{
		bk_gpio_register_isr(gpio_id, NULL);
	}
}

static void shell_task_init(void)
{
	u16		i;

	for(i = 0; i < SHELL_LOG_BUF1_NUM; i++)
	{
		buff1_free_list[i] = i;
	}
	for(i = 0; i < SHELL_LOG_BUF2_NUM; i++)
	{
		buff2_free_list[i] = i;
	}
	for(i = 0; i < SHELL_LOG_BUF3_NUM; i++)
	{
		buff3_free_list[i] = i;
	}

	memset(&log_busy_queue, 0, sizeof(log_busy_queue));
	memset(&pending_queue, 0, sizeof(pending_queue));

	log_busy_queue.free_cnt = SHELL_LOG_BUSY_NUM;

	cmd_line_buf.cur_cmd_type = CMD_TYPE_INVALID;
	cmd_line_buf.cmd_data_len = 0;
	cmd_line_buf.bkreg_state = BKREG_WAIT_01;
	/* Modified by TUYA Start */
	cmd_line_buf.log_level = BK_LOG_LEVEL;
	/* Modified by TUYA End */
	cmd_line_buf.echo_enable = bTRUE;
	cmd_line_buf.log_flush = 1;
	cmd_line_buf.cmd_ovf_hint = 0;

	rtos_init_semaphore_ex(&cmd_line_buf.rsp_buf_semaphore, 1, 1);  // one buffer for cmd_rsp.
	rtos_init_semaphore_ex(&cmd_line_buf.ind_buf_semaphore, 1, 1);  // one buffer for cmd_ind.

#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
	co_list_init(&cmd_line_buf.pending_list);

	co_list_init(&buff_ex1_free_list);
	u8 *buff_ex = (u8 *)&shell_log_buff_ex1[0];
	for(i = 0; i < SHELL_LOG_BUF_EX1_NUM; i++)
	{
		print_node_t * node = (print_node_t *)&buff_ex[i * SHELL_LOG_BUF_EX1_LEN];
		co_list_push_back(&buff_ex1_free_list, &node->hdr);
	}

	co_list_init(&buff_ex2_free_list);
	buff_ex = (u8 *)&shell_log_buff_ex2[0];
	for(i = 0; i < SHELL_LOG_BUF_EX2_NUM; i++)
	{
		print_node_t * node = (print_node_t *)&buff_ex[i * SHELL_LOG_BUF_EX2_LEN];
		co_list_push_back(&buff_ex2_free_list, &node->hdr);
	}

	co_list_init(&buff_ex3_free_list);
	buff_ex = (u8 *)&shell_log_buff_ex3[0];
	for(i = 0; i < SHELL_LOG_BUF_EX3_NUM; i++)
	{
		print_node_t * node = (print_node_t *)&buff_ex[i * SHELL_LOG_BUF_EX3_LEN];
		co_list_push_back(&buff_ex3_free_list, &node->hdr);
	}
#endif // (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)

	create_shell_event();

	if(log_dev != cmd_dev)
	{
		cmd_dev->dev_drv->init(cmd_dev);
		cmd_dev->dev_drv->open(cmd_dev, shell_cmd_tx_complete, shell_rx_indicate); // rx cmd, tx rsp.

		log_dev->dev_drv->init(log_dev);
		log_dev->dev_drv->open(log_dev, shell_log_tx_complete, NULL);  // tx log.
	}
	else
	{
		cmd_dev->dev_drv->init(cmd_dev);
		cmd_dev->dev_drv->open(cmd_dev, shell_tx_complete, shell_rx_indicate); // rx cmd, tx (rsp & log).
	}

	#if defined(FWD_CMD_TO_MBOX) || defined(RECV_CMD_LOG_FROM_MBOX)
	ipc_dev->dev_drv->init(ipc_dev);
	ipc_dev->dev_drv->open(ipc_dev, (shell_ipc_rx_t)shell_ipc_rx_indication);   /* register rx-callback to copy log data to buffer. */
	#endif

	shell_init_ok = bTRUE;

	{
		pm_cb_conf_t enter_config;
		enter_config.cb = (pm_cb)shell_power_save_enter;
		enter_config.args = NULL;

		pm_cb_conf_t exit_config;
		exit_config.cb = (pm_cb)shell_power_save_exit;
		exit_config.args = NULL;

		#if 0
		bk_pm_sleep_register_cb(PM_MODE_LOW_VOLTAGE, PM_DEV_ID_UART1, &enter_config, &exit_config);

		u8   uart_port = UART_ID_MAX;

		cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_GET_UART_PORT, &uart_port);

		shell_rx_wakeup(bk_uart_get_rx_gpio(uart_port));
		#endif

		uart_id_t uart_port = UART_ID_MAX;
		cmd_dev->dev_drv->io_ctrl(cmd_dev, SHELL_IO_CTRL_GET_UART_PORT, &uart_port);

		u8 pm_uart_port = uart_id_to_pm_uart_id(uart_port);
		bk_pm_sleep_register_cb(PM_MODE_LOW_VOLTAGE, pm_uart_port, &enter_config, &exit_config);

		shell_rx_wakeup(bk_uart_get_rx_gpio(uart_port));

		enter_config.cb = (pm_cb)shell_enter_deep_sleep;
		exit_config.args = (void *)PM_CB_PRIORITY_1;

		bk_pm_sleep_register_cb(PM_MODE_DEEP_SLEEP, pm_uart_port, &enter_config, &exit_config);
	}

	if(ate_is_enabled())
		prompt_str_idx = 1;
	else
		prompt_str_idx = 0;

}

void shell_task( void *para )
{
	u32    Events;
	u32    timeout = SHELL_TASK_WAIT_TIME;

	shell_task_init();

	echo_out((u8 *)shell_prompt_str[prompt_str_idx], 3);

	while(bTRUE)
	{
		Events = wait_any_event(timeout);  // WAIT_EVENT;

#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
		if(Events == 0)
		{
			if(!co_list_is_empty(&cmd_line_buf.pending_list))
			{
				Events |= SHELL_EVENT_TX_SND;
			}
		}
#endif

		if(Events & SHELL_EVENT_TX_REQ)
		{
			echo_out((u8 *)"Unsolicited", sizeof("Unsolicited") - 1);
			echo_out((u8 *)shell_prompt_str[prompt_str_idx], 3);
		}

		if(Events & SHELL_EVENT_RX_IND)
		{
			wakeup_process();
			rx_ind_process();
		}

		if(Events & SHELL_EVENT_WAKEUP)
		{
			// TODO
		}

#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
		if(Events & SHELL_EVENT_TX_SND)
		{
			while (!co_list_is_empty(&cmd_line_buf.pending_list))
			{
				print_node_t *node = (print_node_t *)co_list_pick(&cmd_line_buf.pending_list);
				if(node)
				{
					u8 *ptr =(u8*)(node + 1);
					int data_len = node->buf_len, buf_len = SHELL_IND_BUF_LEN, cpy_len;

					u32  int_mask = shell_task_enter_critical();
					co_list_pop_front(&cmd_line_buf.pending_list);
					shell_task_exit_critical(int_mask);

					while (data_len > 0)
					{
						rtos_get_semaphore(&cmd_line_buf.ind_buf_semaphore, SHELL_WAIT_OUT_TIME);
						cpy_len = (data_len > buf_len)? buf_len : data_len;
						cmd_ind_out(ptr, cpy_len);
						data_len -= cpy_len;
						ptr += cpy_len;
					}
					if(node->buf_type == SHELL_LOG_BUF_TYPE_ALLOC)
						os_free(node);
					else
					{
						shell_cmd_free_buf_ex(node);
					}
				}
			}
		}
#endif

		if(Events == 0)
		{
			if(shell_pm_wake_time > 0)
				shell_pm_wake_time--;

			if(shell_pm_wake_time == 0)
			{
				if(shell_pm_wake_flag != 0)
				{
					shell_pm_wake_flag = 0;
					bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_LOG, 1, 0);
					//shell_log_raw_data((const u8*)"sleep\r\n", sizeof("sleep\r\n") - 1);
				}
			}
		}

#if ((CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0) == 0)
		if(shell_pm_wake_flag)
		{
			timeout = SHELL_TASK_WAIT_TIME;
		}
		else
		{
			timeout = BEKEN_WAIT_FOREVER;
		}
#endif
	}
}

int shell_get_cpu_id(void)
{
#ifdef CONFIG_FREERTOS_SMP
	return rtos_get_core_id();
#elif (CONFIG_CPU_CNT > 1)

	if(log_dev->dev_type == SHELL_DEV_MAILBOX)
		return SELF_CPU;

#endif

	return -1;
}

static int shell_cpu_check_valid(void)
{
#if (CONFIG_CPU_CNT > 1) && (LOG_DEV != DEV_MAILBOX)

	if(shell_log_owner_cpu == 0)
		return 1;

	u8    req_cpu = 0;
#ifdef CONFIG_FREERTOS_SMP
	req_cpu = 1 << rtos_get_core_id();
#else
	req_cpu = shell_log_req_cpu;
#endif

	if(shell_log_owner_cpu == req_cpu)
		return 1;

	return 0;

#else

	return 1;

#endif
}

int shell_level_check_valid(int level)
{
	if( !shell_cpu_check_valid() )
		return 0;

	if(level > cmd_line_buf.log_level)
		return 0;

	return 1;
}

int shell_log_out_sync(int level, char *prefix, const char *format, va_list ap)
{
	u32         int_mask;
	char       *pbuf;
	u16         data_len = 0, buf_len;

	if( !shell_level_check_valid(level) )
		return 0;

	pbuf = (char *)&cmd_line_buf.assert_buff[0];
	buf_len = sizeof(cmd_line_buf.assert_buff);

	int_mask = shell_task_enter_critical();

// Modified by TUYA Start
#ifdef prefix_debug
	if(prefix != NULL)
	{
		strncpy(pbuf, prefix, buf_len);
		pbuf[buf_len - 1] = 0;
		data_len = strlen(pbuf);
	}
	else
	{
		data_len = 0;
	}
#else
	data_len = 0;
#endif
// Modified by TUYA End

	data_len += vsnprintf( &pbuf[data_len], buf_len - data_len, format, ap );

	if(data_len >= buf_len)
		data_len = buf_len - 1;

	if ( (data_len != 0) && (pbuf[data_len - 1] == '\n') )
	{
		if ((data_len == 1) || (pbuf[data_len - 2] != '\r'))
		{
			pbuf[data_len] = '\n';      /* '\n\0' replaced with '\r\n', may not end with '\0'. */
			pbuf[data_len - 1] = '\r';
			data_len++;
		}
	}

	log_dev->dev_drv->write_sync(log_dev, (u8 *)pbuf, data_len);

	shell_task_exit_critical(int_mask);

	return 1;
}

int shell_log_raw_data(const u8 *data, u16 data_len)
{
	u8   *packet_buf;
	u16   free_blk_tag;

	if (!shell_init_ok)
	{
		return 0; // bFALSE;
	}

	if( !shell_cpu_check_valid() )
		return 0;

	if (NULL == data || 0 == data_len)
	{
		return 0; // bFALSE;
	}

	packet_buf = alloc_log_blk(data_len, &free_blk_tag);

	if (NULL == packet_buf)
	{
		log_hint_out();
		return 0; // bFALSE;
	}

	memcpy(packet_buf, data, data_len);

	u32 int_mask = shell_task_enter_critical();

	// push to pending queue.
	push_pending_queue(free_blk_tag, data_len);

	// notify shell task to process the log tx.
	tx_req_process();

	shell_task_exit_critical(int_mask);

	return 1; // bTRUE;
}

// Modified by TUYA Start
extern void shell_cmd_ind_out_ex(char *prefix, const char *format, va_list ap,int data_len_tmp);
// Modified by TUYA End
void shell_log_out_port(int level, char *prefix, const char *format, va_list ap)
{
	u8   * packet_buf;
	u16    free_blk_tag;
	u16    log_len = 0, buf_len;

	if( !shell_init_ok )
	{
		/* Modified by TUYA Start */
		cmd_line_buf.log_level = BK_LOG_LEVEL;	// if not intialized, set log_level temporarily here. !!!patch!!!
		/* Modified by TUYA End */
		shell_log_out_sync(level, prefix, format, ap);

		return ;
	}

	if( !shell_level_check_valid(level) )
	{
		return ;
	}

	buf_len = vsnprintf( NULL, 0, format, ap ) + 1;  /* for '\0' */

// Modified by TUYA Start
#ifdef prefix_debug
	if(prefix != NULL)
		buf_len += strlen(prefix);
#endif
// Modified by TUYA End

	#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
	//packet_buf = alloc_log_blk(buf_len, &free_blk_tag);
	packet_buf = NULL;
	#else
	packet_buf = alloc_log_blk(buf_len, &free_blk_tag);
	#endif

	if(packet_buf == NULL)
	{
		#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
// Modified by TUYA Start
		//log_hint_out();
		shell_cmd_ind_out_ex(prefix, format, ap, buf_len);
// Modified by TUYA End
		#else
		log_hint_out();
		#endif
		return ;
	}

	log_len = 0;

// Modified by TUYA Start
#ifdef prefix_debug
	if(prefix != NULL)
	{
		strcpy((char *)&packet_buf[0], prefix);
		log_len = strlen((char *)packet_buf);
	}
#endif
// Modified by TUYA End

	log_len += vsnprintf( (char *)&packet_buf[log_len], buf_len - log_len, format, ap );

	if(log_len >= buf_len)
		log_len = buf_len - 1;

	if ( (log_len != 0) && (packet_buf[log_len - 1] == '\n') )
	{
		if ((log_len == 1) || (packet_buf[log_len - 2] != '\r'))
		{
			packet_buf[log_len] = '\n';     /* '\n\0' replaced with '\r\n', may not end with '\0'. */
			packet_buf[log_len - 1] = '\r';
			log_len++;
		}
	}

	u32  int_mask = shell_task_enter_critical();

	// push to pending queue.
	push_pending_queue(free_blk_tag, log_len);

	//set_shell_event(SHELL_EVENT_TX_REQ);  // notify shell task to process the log tx.
	tx_req_process();

	shell_task_exit_critical(int_mask);

	return ;
}

int shell_assert_out(bool bContinue, char * format, ...)
{
	u32         int_mask;
	char       *pbuf;
	u16         data_len, buf_len;
	va_list     arg_list;

	if( !shell_cpu_check_valid() )
		return 0;

	pbuf = (char *)&cmd_line_buf.assert_buff[0];
	buf_len = sizeof(cmd_line_buf.assert_buff);

	/* just disabled interrupts even when dump out in SMP. */
	/* because other core's dump has been blocked by shell_cpu_check_valid(). */
	/* can't try to get SPINLOCK, because may be it is called from HardFault handler, */
	/* and meanwhile the spinlock is held by other cores. */
	
	// int_mask = shell_task_enter_critical();
	int_mask = rtos_disable_int();

	va_start( arg_list, format );

	data_len = vsnprintf( pbuf, buf_len, format, arg_list );

	va_end( arg_list );

	if(data_len >= buf_len)
		data_len = buf_len - 1;

	log_dev->dev_drv->write_sync(log_dev, (u8 *)pbuf, data_len);

	if( bContinue )
	{
		// shell_task_exit_critical(int_mask);
		rtos_enable_int(int_mask);
	}
	else
	{
		while(bTRUE)
		{
		}
	}

	return 1;//bTRUE;;

}

int shell_assert_raw(bool bContinue, char * data_buff, u16 data_len)
{
	u32         int_mask;

	if( !shell_cpu_check_valid() )
		return 0;

	/* just disabled interrupts even when dump out in SMP. */
	/* because other core's dump has been blocked by shell_cpu_check_valid(). */
	/* can't try to get SPINLOCK, because may be it is called from HardFault handler, */
	/* and meanwhile the spinlock is held by other cores. */
	
	// int_mask = shell_task_enter_critical();
	int_mask = rtos_disable_int();

	log_dev->dev_drv->write_sync(log_dev, (u8 *)data_buff, data_len);

	if( bContinue )
	{
		// shell_task_exit_critical(int_mask);
		rtos_enable_int(int_mask);
	}
	else
	{
		while(1)
		{
		}
	}

	return 1;//bTRUE;;

}

#if defined(FWD_CMD_TO_MBOX) || defined(RECV_CMD_LOG_FROM_MBOX)
static int cmd_rsp_fwd(u8 * rsp_msg, u16 msg_len)
{
	u16    rsp_blk_tag = MAKE_BLOCK_TAG(MBOX_RSP_BLK_ID, SHELL_FWD_QUEUE_ID);

	cmd_info_out(rsp_msg, msg_len, rsp_blk_tag);

	return 1;
}

static int cmd_ind_fwd(u8 * ind_msg, u16 msg_len)
{
	u16    ind_blk_tag = MAKE_BLOCK_TAG(MBOX_IND_BLK_ID, SHELL_FWD_QUEUE_ID);

	cmd_info_out(ind_msg, msg_len, ind_blk_tag);

	return 1;
}

static int result_fwd(int blk_id)
{
	log_cmd_t   * log_cmd;

	if(blk_id == MBOX_RSP_BLK_ID)
	{
		log_cmd = &ipc_fwd_data.rsp_buf;
	}
	else if(blk_id == MBOX_IND_BLK_ID)
	{
		log_cmd = &ipc_fwd_data.ind_buf;
	}
	else
	{
		return 0;
	}

	log_cmd->hdr.data = 0;
	log_cmd->hdr.cmd = MB_CMD_LOG_OUT_OK;

	return ipc_dev->dev_drv->write_cmd(ipc_dev, (mb_chnl_cmd_t *)log_cmd);
}

static u32 shell_ipc_rx_indication(u16 cmd, log_cmd_t *log_cmd, u16 cpu_id)
{
	u32   result = ACK_STATE_FAIL;
	u8  * data = log_cmd->buf;
	u16   data_len = log_cmd->len;

	if(shell_log_owner_cpu != 0)
	{
		if(shell_log_owner_cpu == (0x01 << cpu_id))
			shell_log_req_cpu = shell_log_owner_cpu;
		else
			return result;
	}

	if(cmd == MB_CMD_LOG_OUT)
	{
		u8      queue_id = GET_QUEUE_ID(log_cmd->tag);

		if(queue_id == SHELL_RSP_QUEUE_ID)
		{
			memcpy(&ipc_fwd_data.rsp_buf, log_cmd, sizeof(ipc_fwd_data.rsp_buf));
			cmd_rsp_fwd(data, data_len);

			result = ACK_STATE_PENDING;
		}
		else if(queue_id == SHELL_IND_QUEUE_ID)
		{
			memcpy(&ipc_fwd_data.ind_buf, log_cmd, sizeof(ipc_fwd_data.ind_buf));
			cmd_ind_fwd(data, data_len);

			result = ACK_STATE_PENDING;
		}
		else  // no cmd_hint from slave, so must be log from slave.
		{
			result = shell_log_raw_data(data, data_len);

			if(result == 0)
				result = ACK_STATE_FAIL;
			else
				result = ACK_STATE_COMPLETE;
		}
	}
	else if(cmd == MB_CMD_ASSERT_OUT)
	{
		#if (CONFIG_TASK_WDT)
		bk_task_wdt_feed();
		#endif
		#if (CONFIG_INT_WDT)
		bk_int_wdt_feed();
		#endif

		shell_assert_raw(true, (char *)data, data_len);

		result = ACK_STATE_COMPLETE;
	}

	shell_log_req_cpu = 0;

	/* no cmd handler. */
	return result;
}

int shell_cmd_forward(char *cmd, u16 cmd_len)
{
	mb_chnl_cmd_t	mb_cmd_buf;
	user_cmd_t * user_cmd = (user_cmd_t *)&mb_cmd_buf;

	user_cmd->hdr.data = 0;
	user_cmd->hdr.cmd = MB_CMD_USER_INPUT;
	user_cmd->buf = (u8 *)cmd;
	user_cmd->len = cmd_len;

	u32  int_mask = shell_task_enter_critical();

	int ret_code = ipc_dev->dev_drv->write_cmd(ipc_dev, &mb_cmd_buf);

	shell_task_exit_critical(int_mask);

	return ret_code;
}
#endif

void shell_echo_set(int en_flag)
{
	if(en_flag != 0)
		cmd_line_buf.echo_enable = bTRUE;
	else
		cmd_line_buf.echo_enable = bFALSE;
}

int shell_echo_get(void)
{
	if(cmd_line_buf.echo_enable)
		return 1;

	return 0;
}

void shell_set_log_level(int level)
{
	cmd_line_buf.log_level = level;
}

int shell_get_log_level(void)
{
	return cmd_line_buf.log_level;
}

void shell_set_log_flush(int flush_flag)
{
	cmd_line_buf.log_flush = flush_flag;
}

int shell_get_log_flush(void)
{
	return cmd_line_buf.log_flush;
}

int shell_get_log_statist(u32 * info_list, u32 num)
{
	int   cnt = 0;
	if(num > 0)
	{
		info_list[0] = shell_log_overflow;
		cnt++;
	}
	if(num > 1)
	{
		info_list[1] = shell_log_count;
		cnt++;
	}
	if(num > 2)
	{
		info_list[2] = free_queue[0].ovf_cnt;
		cnt++;
	}
	if(num > 3)
	{
		info_list[3] = free_queue[1].ovf_cnt;
		cnt++;
	}
	if(num > 4)
	{
		info_list[4] = free_queue[2].ovf_cnt;
		cnt++;
	}

	return cnt;
}

void shell_log_flush(void)
{
	u32         int_mask;

	/* just disabled interrupts even when dump out in SMP. */
	/* because it is called by power_saving process or by dumping process. */
	/* in any case, only one core is running. */
	/* can't try to get SPINLOCK, because may be it is called from HardFault handler, */
	/* and meanwhile the spinlock is held by other cores. */
	
	// int_mask = shell_task_enter_critical();
	int_mask = rtos_disable_int();

	log_dev->dev_drv->io_ctrl(log_dev, SHELL_IO_CTRL_FLUSH, NULL);

	// shell_task_exit_critical(int_mask);
	rtos_enable_int(int_mask);
}

void shell_set_uart_port(uint8_t uart_port)
{
#if (LOG_DEV == DEV_UART)
	if(log_dev->dev_type != SHELL_DEV_UART)
	{
		return;
	}

	if ((bk_get_printf_port() != uart_port) && (uart_port < UART_ID_MAX))
	{
		u32  int_mask = shell_task_enter_critical();

		shell_log_flush();
		log_dev->dev_drv->close(log_dev);
		log_dev->dev_drv->io_ctrl(log_dev, SHELL_IO_CTRL_SET_UART_PORT, &uart_port);

		bk_set_printf_port(uart_port);

		log_dev->dev_drv->init(log_dev);
		if(log_dev != cmd_dev)
		{
			log_dev->dev_drv->open(log_dev, shell_log_tx_complete, NULL);  // tx log.
		}
		else
		{
			log_dev->dev_drv->open(log_dev, shell_tx_complete, shell_rx_indicate);
		}

		shell_task_exit_critical(int_mask);
	}
#endif
}

void shell_cmd_ind_out(const char *format, ...)
{
	u16   data_len, buf_len = SHELL_IND_BUF_LEN;
	va_list  arg_list;

	rtos_get_semaphore(&cmd_line_buf.ind_buf_semaphore, SHELL_WAIT_OUT_TIME);

	va_start(arg_list, format);
	data_len = vsnprintf( (char *)&cmd_line_buf.cmd_ind_buff[0], buf_len - 1, format, arg_list );
	va_end(arg_list);

	if(data_len >= buf_len)
		data_len = buf_len - 1;

	cmd_ind_out(cmd_line_buf.cmd_ind_buff, data_len);
}

#if (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)
// flag=1 means need check list cnt, call in not-interrupt_context
static print_node_t *shell_cmd_alloc_buf_ex(int data_len_tmp, u8 flag)
{
	print_node_t *node = NULL;
	struct co_list *head_list = NULL;
	uint16 buf_type = 0, buf_len = 0;

	if(data_len_tmp <= SHELL_LOG_BUF_EX1_DATA_LEN)
	{
		if(co_list_cnt(&buff_ex1_free_list) > 0)
		{
			head_list = &buff_ex1_free_list;
			buf_type = SHELL_LOG_BUF_TYPE_STATIC_1;
			buf_len = SHELL_LOG_BUF_EX1_DATA_LEN;
		}
		else if(co_list_cnt(&buff_ex2_free_list) > 0)
		{
			head_list = &buff_ex2_free_list;
			buf_type = SHELL_LOG_BUF_TYPE_STATIC_2;
			buf_len = SHELL_LOG_BUF_EX2_DATA_LEN;
		}
		else
		{
			if(flag)
			{
				if(co_list_cnt(&buff_ex3_free_list) > MIN_ALLOC_BUF_EX3_NUM)
				{
					head_list = &buff_ex3_free_list;
					buf_type = SHELL_LOG_BUF_TYPE_STATIC_3;
					buf_len = SHELL_LOG_BUF_EX3_DATA_LEN;
				}
			}
			else
			{
				head_list = &buff_ex3_free_list;
				buf_type = SHELL_LOG_BUF_TYPE_STATIC_3;
				buf_len = SHELL_LOG_BUF_EX3_DATA_LEN;
			}
		}
	}
	else if (data_len_tmp <= SHELL_LOG_BUF_EX2_DATA_LEN)
	{
		if(co_list_cnt(&buff_ex2_free_list) > 0)
		{
			head_list = &buff_ex2_free_list;
			buf_type = SHELL_LOG_BUF_TYPE_STATIC_2;
			buf_len = SHELL_LOG_BUF_EX2_DATA_LEN;
		}
		else
		{
			if(flag)
			{
				if(co_list_cnt(&buff_ex3_free_list) > MIN_ALLOC_BUF_EX3_NUM)
				{
					head_list = &buff_ex3_free_list;
					buf_type = SHELL_LOG_BUF_TYPE_STATIC_3;
					buf_len = SHELL_LOG_BUF_EX3_DATA_LEN;
				}
			}
			else
			{
				head_list = &buff_ex3_free_list;
				buf_type = SHELL_LOG_BUF_TYPE_STATIC_3;
				buf_len = SHELL_LOG_BUF_EX3_DATA_LEN;
			}
		}
	}
	else
	{
		if(flag)
		{
			if(co_list_cnt(&buff_ex3_free_list) > MIN_ALLOC_BUF_EX3_NUM)
			{
				if (data_len_tmp <= SHELL_LOG_BUF_EX3_DATA_LEN)
				{
					head_list = &buff_ex3_free_list;
					buf_type = SHELL_LOG_BUF_TYPE_STATIC_3;
					buf_len = SHELL_LOG_BUF_EX3_DATA_LEN;
				}
			}
		}
		else
		{
			head_list = &buff_ex3_free_list;
			buf_type = SHELL_LOG_BUF_TYPE_STATIC_3;
			buf_len = SHELL_LOG_BUF_EX3_DATA_LEN;
		}
	}

	if(head_list)
	{
		u32  int_mask = shell_task_enter_critical();
		if (!co_list_is_empty(head_list))
		{
			node = (print_node_t *)co_list_pop_front(head_list);
			if(node)
			{
				node->buf_type = buf_type;
				node->buf_len = buf_len;
			}
		}
		shell_task_exit_critical(int_mask);
	}

	return node;
}

static void shell_cmd_free_buf_ex(print_node_t *node)
{
	if(node)
	{
		struct co_list *head_list = NULL;
		if(node->buf_type == SHELL_LOG_BUF_TYPE_STATIC_1)
		{
			head_list = &buff_ex1_free_list;
		}
		else if(node->buf_type == SHELL_LOG_BUF_TYPE_STATIC_2)
		{
			head_list = &buff_ex2_free_list;
		}
		else if(node->buf_type == SHELL_LOG_BUF_TYPE_STATIC_3)
		{
			head_list = &buff_ex3_free_list;
		}

		BK_ASSERT(head_list != NULL);
		if(head_list)
		{
			u32  int_mask = shell_task_enter_critical();
			co_list_push_back(head_list, &node->hdr);
			shell_task_exit_critical(int_mask);
		}
	}
}

// Modified by TUYA Start
void shell_cmd_ind_out_ex(char *prefix, const char *format, va_list ap, int data_len_tmp)
{
	print_node_t *node = NULL;

	extern uint32_t platform_is_in_interrupt_context( void );
	if (platform_is_in_interrupt_context())
	{
		node = shell_cmd_alloc_buf_ex(data_len_tmp, 0);
		if((node) && (data_len_tmp > node->buf_len))
		{
			data_len_tmp = node->buf_len;
		}
	}
	else
	{
		node = shell_cmd_alloc_buf_ex(data_len_tmp, 1);
		if(node == NULL)
		{
			uint16_t mem_bytes = 0;
			bk_wifi_get_min_rsv_mem(&mem_bytes);
			if (rtos_get_free_heap_size() > (mem_bytes *2))
			{
				node = (print_node_t *)os_malloc(sizeof(print_node_t) + data_len_tmp + 4);
				if(node)
				{
					node->buf_type = SHELL_LOG_BUF_TYPE_ALLOC;
				}
			}
		}
	}

	if(node == NULL)
	{
		//log_hint_out();
		goto ind_out_exit;
	}

	char *ptr = (char *)(node + 1);
	int data_len = 0;

	// Modified by TUYA Start
	#ifdef prefix_debug
	if(prefix != NULL)
	{
		strcpy((char *)&ptr[0], prefix);
		int prefix_len = strlen((char *)prefix);
		data_len_tmp -= prefix_len;
		data_len += prefix_len;
	}
	#endif
	// Modified by TUYA End

	int len = vsnprintf(&ptr[data_len], data_len_tmp, format, ap);
	if(len > data_len_tmp)
	{
		data_len += data_len_tmp;
		// we have 2bytes more space
		ptr[data_len] = '\r';
		ptr[data_len+1] = '\n';
		data_len += 2;
	}
	else
	{
		data_len += len;
	}

	if ((data_len != 0) && (ptr[data_len - 1] == '\n'))
	{
		if (data_len == 1 || ptr[data_len - 2] != '\r')
		{
			ptr[data_len] = '\n';
			ptr[data_len - 1] = '\r';
			data_len++;
		}
	}
	node->buf_len = data_len;

	u32  int_mask = shell_task_enter_critical();
	co_list_push_back(&cmd_line_buf.pending_list, &node->hdr);
	shell_task_exit_critical(int_mask);

ind_out_exit:
	if (!co_list_is_empty(&cmd_line_buf.pending_list))
	{
		if(0 == platform_local_irq_disabled())
		{
			set_shell_event(SHELL_EVENT_TX_SND);
		}
	}
}
// Modified by TUYA End
#endif // (CONFIG_SOC_BK7236 && CONFIG_SYS_CPU0)

void shell_set_log_cpu(u8 req_cpu)
{
#if (CONFIG_CPU_CNT > 1) && (LOG_DEV != DEV_MAILBOX)
	if(req_cpu >= CONFIG_CPU_CNT)
	{
		shell_log_owner_cpu = 0;
		shell_log_req_cpu = 0;
	}
	else if(shell_log_owner_cpu == 0)
	{
		shell_log_owner_cpu = 1 << req_cpu;
		shell_log_req_cpu   = 1 << req_cpu;
	}
#endif
}

