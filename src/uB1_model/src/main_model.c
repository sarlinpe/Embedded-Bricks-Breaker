#include "xmk.h"
#include "xgpio.h"
#include "xparameters.h"
#include "xmutex.h"
#include "xmbox.h"

#include <errno.h>
#include <pthread.h>
#include <sys/init.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/intr.h>
#include <sys/timer.h> // for sleep

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#include "config.h"
#include "config_model.h"
#include "timer.h"

int main (void);
void* main_prog(void *arg);

static XMbox mbx_display;
XMutex mtx_hw;
XGpio gpio_pb;

pthread_t tid_ball, tid_bar, tid_timer_bar;
sem_t sem_debounce, sem_timer_bar;
pthread_mutex_t mtx_bar;

Game_state game_state = WAITING;
//Timer_state timer_state;
Bar bar;
Timer* timer_bar;


/* Push buttons ISR */
static void pb_ISR(void *arg) {
	static u32 prev_pb_state = 0;

	/* Clear the state of the interrupt flag and read the buttons states */
	XGpio_InterruptClear(&gpio_pb,1);
	u32 pb_state = XGpio_DiscreteRead(&gpio_pb, 1);

	if( (pb_state ^ prev_pb_state) & ((1 << PB_RIGHT) | (1 << PB_LEFT)) ) {
		XGpio_InterruptDisable(&gpio_pb,1);
		sem_post(&sem_debounce);
		safe_printf("Interrupt: %d \r\n", pb_state); // debug
	};
	prev_pb_state = pb_state;
}

float rad(u16 deg) {
	return deg/360.*M_PI;
}

int max(int a, int b) {
	return a>b ? a : b;
}

//u32 ms(u32 ticks) {
//	return ticks * (SYSTMR_INTERVAL / SYSTMR_CLK_FREQ_KHZ);
//}

void* thread_bar() {
	u32 pb_state;
	u8 pb_right, pb_left;
	u8 prev_pb_right = 0, prev_pb_left = 0;
	u32 actual_time;

	bar.pos = BZ_W / 2;
	bar.speed = 0;
	bar.jump = NONE;
	bar.last_change = 0;

	while(1) {
		sem_wait(&sem_debounce);
		//if (timer_state != FINISH)
		if(!is_finished(timer_bar))
			sleep(DEBOUNCE_DELAY);

		pb_state = XGpio_DiscreteRead(&gpio_pb, 1);
		pb_right = (pb_state >> PB_RIGHT) & 1;
		pb_left = (pb_state >> PB_LEFT) & 1;
		actual_time = GET_MS;

		safe_printf("New reading from the buttons: %u\n\r", pb_state);

		pthread_mutex_lock(&bar.mtx);
		if( !(pb_right ^ pb_left) ) { // both released or both pressed
			if( (actual_time - bar.last_change) < BAR_DELAY_THRESH) {
				//timer_state = STOP; // Cancel the timer
				timer_stop(timer_bar);
				safe_printf("Jumping\n\r");
				if( !(prev_pb_right ^ prev_pb_left) )
					bar.jump = NONE;
				else if(prev_pb_right)
					bar.jump = RIGHT;
				else if(prev_pb_left)
					bar.jump = LEFT;
			}
			else
				bar.jump = NONE;
			bar.speed = 0;
		}
		else {
			//if (timer_state == FINISH) {
			if(is_finished(timer_bar)) {
				bar.jump = NONE;
				bar.speed = BAR_DEF_SPEED * (pb_right ? 1 : -1);
				print("Setting speed of bar to continuous\n\r");
				//timer_state = STOP;
				timer_stop(timer_bar);
			}
			else
				//sem_post(&sem_timer_bar);
				timer_start(timer_bar);
		}
		bar.last_change = actual_time;
		pthread_mutex_unlock(&bar.mtx);
		prev_pb_right = pb_right;
		prev_pb_left = pb_left;
		XGpio_InterruptEnable(&gpio_pb,1);
	}
}

//void* timer_bar() {
//	timer_state = STOP;
//	u32 launch_time;
//	while(1) {
//		switch(timer_state){
//			case STOP:
//				sem_wait(&sem_timer_bar);
//				launch_time = GET_MS;
//				timer_state = RUN;
//				break;
//			case RUN:
//				if( (GET_MS - launch_time) > BAR_DELAY_THRESH) {
//					timer_state = FINISH;
//					XGpio_InterruptDisable(&gpio_pb,1);
//					sem_post(&sem_debounce);
//				}
//				else
//					sleep(1);
//				break;
//			case FINISH:
//				sleep(1);
//				break;
//		}
//
//
//	}
//}

void timer_bar_cb() {
	XGpio_InterruptDisable(&gpio_pb,1);
	sem_post(&sem_debounce);
}


void* thread_ball() {
	u32 actual_time, last_sent = GET_MS;
	Model_state model_state;

	Ball ball, ball_new;
	ball.x = BZ_W / 2;
	ball.y = BZ_H - BAR_OFFSET_Y - BAR_H - BALL_RADIUS;
	ball.vel = BALL_DEF_SPEED;
	ball.angle = BALL_DEF_ANGLE;

	model_state.game_state = RUNNING;
	model_state.time = 0;
	model_state.score = 0;
	for(u8 col = 0; col < NB_COLUMNS; col++)
		for(u8 row = 0; row < NB_ROWS; row++) {
			if( (col == 1 || col == 5) && (row > 1) && (row < 5))
				model_state.bricks[col][row] = GOLDEN;
			else
				model_state.bricks[col][row] = NORMAL;
		}

	while(1) {

		//safe_printf("Updating ball\n\r");
		/* Update Bar position */
		pthread_mutex_lock(&bar.mtx);
		switch(bar.jump) {
			case RIGHT:
				bar.pos += BAR_JUMP_DIST;
				print("Jump right\r\n");
				break;
			case LEFT:
				bar.pos -= BAR_JUMP_DIST;
				print("Jump left\r\n");
				break;
			default:
				bar.pos += bar.speed*UPDATE_S;
		}
		bar.jump = NONE;
		pthread_mutex_unlock(&bar.mtx);

		/* Enforce screen width limits */
		if(bar.pos < (BAR_W/2 + 1))
			bar.pos = BAR_W/2 + 1;
		else if(bar.pos >= (BZ_W - BAR_W/2))
			bar.pos = BZ_W - BAR_W/2 - 1;

		if(game_state == RUNNING) {
			ball_new.x = ball.x + UPDATE_S*ball.vel*cos(rad(ball.angle));
			ball_new.y = ball.y + UPDATE_S*ball.vel*sin(rad(ball.angle));
		}

		model_state.ball = ball_new;
		model_state.bar_pos = bar.pos;
		//safe_printf("Position of bar: %u\n\r", model_state.bar_pos);
		//safe_printf("Size sent: %d\n\r", sizeof(model_state));

		actual_time = GET_MS;
		//xil_printf("Last: %u\t New: %u", last_sent, actual_time);
		int wait_time = UPDATE_MS - (int) actual_time + (int) last_sent;
		safe_printf("Wait time: %d\n\r", wait_time);
		sleep(max(wait_time,0));
		last_sent = GET_MS;
		XMbox_WriteBlocking(&mbx_display, (u32*)&model_state, sizeof(model_state));
	}
}

void* main_prog(void *arg) {
    XMutex_Config* configPtr_mutex;
    XMbox_Config* configPtr_mailbox;
    pthread_attr_t attr;
    struct sched_param sched_par;
    int ret;

    print("[INFO uB1] \t Starting configuration\r\n");

    sem_init(&sem_debounce, 1, 0);
    //sem_init(&sem_timer_bar, 1, 0);

    /* Configure the HW Mutex */
    configPtr_mutex = XMutex_LookupConfig(MUTEX_DEVICE_ID);
    if (configPtr_mutex == (XMutex_Config *)NULL){
        print("[ERROR uB1]\t While configuring the Hardware Mutex\r\n");
        return (void*) XST_FAILURE;
    }
    ret = XMutex_CfgInitialize(&mtx_hw, configPtr_mutex, configPtr_mutex->BaseAddress);
    if (ret != XST_SUCCESS){
        print("[ERROR uB1]\t While initializing the Hardware Mutex\r\n");
        return (void*) XST_FAILURE;
    }

    /* Configure the mailbox */
    configPtr_mailbox = XMbox_LookupConfig(MBOX_DEVICE_ID);
    if (configPtr_mailbox == (XMbox_Config *)NULL) {
    	print("[ERROR uB1]\t While configuring the Mailbox\r\n");
        return (void*) XST_FAILURE;
    }
    ret = XMbox_CfgInitialize(&mbx_display, configPtr_mailbox, configPtr_mailbox->BaseAddress);
    if (ret != XST_SUCCESS) {
    	print("[ERROR uB1]\t While initializing the Mailbox\r\n");
        return (void*) XST_FAILURE;
    }

    /* Configure interrupts */
    ret = XGpio_Initialize(&gpio_pb, XPAR_GPIO_0_DEVICE_ID);
	XGpio_SetDataDirection(&gpio_pb, 1, 0x000000FF); // all 1 --> intput
	XGpio_InterruptGlobalEnable(&gpio_pb);
	XGpio_InterruptEnable(&gpio_pb,1);
	register_int_handler(XPAR_MICROBLAZE_1_AXI_INTC_AXI_GPIO_0_IP2INTC_IRPT_INTR,
						 pb_ISR, &gpio_pb);
	enable_interrupt(XPAR_MICROBLAZE_1_AXI_INTC_AXI_GPIO_0_IP2INTC_IRPT_INTR);

	/* Bar timer thread, priority 1 */
//    pthread_attr_init(&attr);
//    sched_par.sched_priority = 1;
//    pthread_attr_setschedparam(&attr, &sched_par);
//    ret = pthread_create (&tid_timer_bar, &attr, (void*)timer_bar, NULL);
//    if (ret != 0) {
//        xil_printf("[ERROR uB1]\t (%d) launching timer_bar\r\n", ret);
//        return (void*) XST_FAILURE;
//    }
//    else
//        xil_printf("[INFO uB1] \t timer_bar launched with ID %d \r\n", tid_timer_bar);

	timer_bar = timer_init(BAR_DELAY_THRESH, &timer_bar_cb);

    /* Bar management thread, priority 1 */
    pthread_attr_init(&attr);
    sched_par.sched_priority = 1;
    pthread_attr_setschedparam(&attr, &sched_par);
    ret = pthread_create (&tid_bar, &attr, (void*)thread_bar, NULL);
    if (ret != 0) {
        xil_printf("[ERROR uB1]\t (%d) launching thread_bar\r\n", ret);
        return (void*) XST_FAILURE;
    }
    else
        xil_printf("[INFO uB1] \t thread_bar launched with ID %d \r\n", tid_bar);

    /* Ball management thread, priority 1 */
    pthread_attr_init (&attr);
    sched_par.sched_priority = 1;
    pthread_attr_setschedparam(&attr, &sched_par);
    ret = pthread_create (&tid_ball, &attr, (void*)thread_ball, NULL);
    if (ret != 0) {
        xil_printf("[ERROR uB1]\t (%d) launching thread_ball\r\n", ret);
        return (void*) XST_FAILURE;
    }
    else
        xil_printf("[INFO uB1] \t thread_ball launched with ID %d \r\n", tid_ball);



    return 0;
}

int main (void) {
    print("[INFO uB1] \t Entering main()\r\n");
    xilkernel_init();
    xmk_add_static_thread(main_prog,0);
    xilkernel_start();
    xilkernel_main ();

    //Control does not reach here
}