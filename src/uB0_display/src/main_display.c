/*******************************************************************************
 *
 * Title:       Embedded Bricks-Breaker Project
 * File:        main_display.c
 * Date:        2017-04-13
 * Author:      Paul-Edouard Sarlin
 * Description: Main coordinator file for the display processor (uB0).
 *
 ******************************************************************************/

#include "xmk.h"
#include "xtft.h"
#include "xparameters.h"
#include "xmutex.h"
#include "xmbox.h"

#include <pthread.h>
#include <sys/init.h>
#include <sys/msg.h>
#include <sys/ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "config.h"
#include "config_display.h"
#include "display.h"

int main (void);
void* main_prog(void *arg);
void* thread_display();
u8 lastCnt();
u8 nextCnt();

static XTft TftInstance;
static XMbox mbx_model;
XMutex mtx_hw;
pthread_t tid_disp;

/* Two different frame buffers are used here */
u32 frames_addr[NB_FRAMES] = {TFT_FRAME_ADDR1, TFT_FRAME_ADDR2};
u8 frames_cnt = 0;


void* thread_display() {
    int Status;
    XTft_Config *TftConfigPtr;
    u32 TftDeviceId = TFT_DEVICE_ID;
    Model_state data, data_prev[NB_FRAMES];
    u32 t_stamp = GET_MS;
    u16 fps = 0;

    safe_printf("[INFO uB0] \t Configuring the display\r\n");

    /* Get address of the XTft_Config structure for the given device id */
    TftConfigPtr = XTft_LookupConfig(TftDeviceId);
    if (TftConfigPtr == (XTft_Config *)NULL)
        return (void*) XST_FAILURE;

    /* Initialize all the TftInstance members */
    Status = XTft_CfgInitialize(&TftInstance, TftConfigPtr,
                                TftConfigPtr->BaseAddress);
    if (Status != XST_SUCCESS)
        return (void*) XST_FAILURE;

    /* Set background color to white and clean each buffer */
    frames_cnt = 0;
    TftInstance.TftConfig.VideoMemBaseAddr = frames_addr[frames_cnt];
    for(u8 i = 0; i < NB_FRAMES; i++) {
        /* Initialize previous states */
        data_prev[frames_cnt].bar_pos = BZ_W/2;
        data_prev[frames_cnt].ball_posx = BZ_W/2;
        data_prev[frames_cnt].ball_posy = BZ_H - BAR_OFFSET_Y - BAR_H - BALL_R;

        XTft_SetColor(&TftInstance, 0, WHITE);
        XTft_ClearScreen(&TftInstance);
        draw_layout(&TftInstance);
        frames_cnt = nextCnt();
        TftInstance.TftConfig.VideoMemBaseAddr = frames_addr[frames_cnt];
    }

    safe_printf("[INFO uB0] \t Listening to the model\r\n");

    while(1) {
        XMbox_ReadBlocking(&mbx_model, (u32*)&data, sizeof(data));

        /* Erase what can be erased */
        set_erase();
        draw_bar(&TftInstance, data_prev[frames_cnt].bar_pos);
        draw_ball(&TftInstance, data_prev[frames_cnt].ball_posx,
                                data_prev[frames_cnt].ball_posy);
        set_draw();

        /* Write data */
        draw_bricks(&TftInstance, data.bricks, data_prev[frames_cnt].bricks);
        draw_bar(&TftInstance, data.bar_pos);
        draw_ball(&TftInstance, data.ball_posx, data.ball_posy);
        display_info(&TftInstance, data);

        /* Display message */
        if(data.game_state != data_prev[frames_cnt].game_state) {
            set_erase();
            display_msg(&TftInstance, data_prev[frames_cnt].game_state);
            set_draw();
            display_msg(&TftInstance, data.game_state);
        }

        /* Display FPS */
        display_fps(&TftInstance, fps);
        fps = (unsigned)(1000./(GET_MS-t_stamp));
        t_stamp = GET_MS;

        data_prev[frames_cnt] = data;

        /* Wait previous frame to be displayed */
        while (XTft_GetVsyncStatus(&TftInstance)
               != XTFT_IESR_VADDRLATCH_STATUS_MASK);
        /* Force display to the current frame buffer */
        XTft_SetFrameBaseAddr(&TftInstance, frames_addr[frames_cnt]);
        /* Switch frame counter */
        frames_cnt = nextCnt();
        /* Set the new frame address for subsequent draws */
        TftInstance.TftConfig.VideoMemBaseAddr = frames_addr[frames_cnt];

    }
}

u8 lastCnt() {
    return (NB_FRAMES + frames_cnt + 1) % NB_FRAMES; }

u8 nextCnt() {
    return (NB_FRAMES + frames_cnt - 1) % NB_FRAMES; }


void* main_prog(void *arg) {
    XMutex_Config* configPtr_mutex;
    XMbox_Config* configPtr_mailbox;
    pthread_attr_t attr;
    struct sched_param sched_par;
    int ret;

    print("[INFO uB0] \t Starting configuration\r\n");
    /* Configure the HW Mutex */
    configPtr_mutex = XMutex_LookupConfig(MUTEX_DEVICE_ID);
    if (configPtr_mutex == (XMutex_Config *)NULL){
        print("[ERROR uB0]\t While configuring the Hardware Mutex\r\n");
        return (void*) XST_FAILURE;
    }
    ret = XMutex_CfgInitialize(&mtx_hw, configPtr_mutex,
                               configPtr_mutex->BaseAddress);
    if (ret != XST_SUCCESS){
        print("[ERROR uB0]\t While initializing the Hardware Mutex\r\n");
        return (void*) XST_FAILURE;
    }

    /* Configure the mailbox */
    configPtr_mailbox = XMbox_LookupConfig(MBOX_DEVICE_ID);
    if (configPtr_mailbox == (XMbox_Config *)NULL) {
        print("[ERROR uB0]\t While configuring the Mailbox\r\n");
        return (void*) XST_FAILURE;
    }
    ret = XMbox_CfgInitialize(&mbx_model, configPtr_mailbox,
                              configPtr_mailbox->BaseAddress);
    if (ret != XST_SUCCESS) {
        print("[ERROR uB0]\t While initializing the Mailbox\r\n");
        return (void*) XST_FAILURE;
    }

    /* Display management thread, priority 1 */
    pthread_attr_init (&attr);
    sched_par.sched_priority = 1;
    pthread_attr_setschedparam(&attr, &sched_par);
    ret = pthread_create (&tid_disp, &attr, (void*)thread_display, NULL);
    if (ret != 0)
        xil_printf("[ERROR uB0]\t (%d) launching thread_display\r\n", ret);
    else
        xil_printf("[INFO uB0] \t Thread_display launched with ID %d \r\n",
                   tid_disp);

    return 0;
}

int main (void) {
    print("[INFO uB0] \t Entering main()\r\n");
    xilkernel_init();
    xmk_add_static_thread(main_prog,0);
    xilkernel_start();
    xilkernel_main ();

    //Control does not reach here
}
