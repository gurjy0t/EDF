/*FreeRTOS V9.0.0 - Copyright (C) 2016 Real Time Engineers Ltd.
    All rights reserved*/

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include "stm32f4_discovery.h"
/* Kernel includes. */
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"
#include "stm32f4xx.h"

#define green  	0
#define amber  	1
#define red  	2
#define blue  	3

typedef unsigned char BYTE;

uint32_t CREATE = 0;
uint32_t DELETE = 1;
uint32_t PERIODIC = 0;
uint32_t APERIODIC = 1;

uint32_t TIMER = -1;
uint32_t ACTIVE = -2;
uint32_t OVERDUE = -3;

uint32_t MONITOR = 1;
uint32_t LOW = 2;
uint32_t MEDIUM = 3;
uint32_t HIGH = 4;
uint32_t SCHEDULER = 5;

uint32_t ACTIVE_COUNTER=0;
uint32_t IDLE_COUNTER=0;

uint32_t MONITORING=0;

uint32_t MAX_LIST_LENGTH=5;


TimerHandle_t timer;

#define QUEUE_LENGTH 100
#define GENERATOR_WORKERS 1


static void prvSetupHardware(void);

//the task params struct
typedef struct TaskParams{
	uint32_t deadline;
	uint32_t task_type;
	uint32_t execution_time;
	uint32_t creation_time;
	uint32_t period;
	uint32_t light;
	TaskHandle_t task_id;
	TimerHandle_t timer;
	uint32_t command;
} TaskParams;

//The doubly linked list for overdue and active lists
typedef struct TaskList {
	TaskParams params;
	struct TaskList *next;
	struct TaskList *prev;
} TaskList;

//The message struct to send to the scheduler
typedef struct SchedulerMessage{
	xQueueHandle queue;
	TaskParams params;
} SchedulerMessage;

xQueueHandle SchedulerQueue;

void dd_delete(TaskHandle_t xHandle);
void Watch_Deadline(TimerHandle_t xTimer);
void UserPeriodicTask(TaskParams* params);
void UserAPeriodicTask(TaskParams* params);
void dd_tcreate(TaskParams *params);
TaskList* dd_return_active_list();
TaskList* dd_return_overdue_list();
TaskList* getList(TaskParams params);
TaskList* removeFromActiveList(TaskParams params, TaskList **list);
void addToList(TaskList* entry, TaskList **list);
void DD_Scheduler_Task();
void Generator_Task();
void Monitor_Task();
void startGenerators();
static void userTaskDelay(uint32_t delay_time);
void turnLightOn(int light);
void turnLightOff();

/*
 * Parameters: Task handle
 *
 * 1. Create a new TaskParams struct
 * 2. Create new queue which scheduler will respond to
 * 3. Write to the scheduler queue the created params
 * 4. Recieve the message from the scheduler
 * 5. Delete the task and the queue
 */
void dd_delete(TaskHandle_t xHandle){
	TaskParams params = {.task_id = xHandle, .command=DELETE};

	xQueueHandle TaskQueue = xQueueCreate(QUEUE_LENGTH, sizeof(int));
	vQueueAddToRegistry(TaskQueue, "messageQueue");

	SchedulerMessage message = {TaskQueue, params};
	xQueueSend(SchedulerQueue, &message, 50);

	int response_code;
	xQueueReceive(TaskQueue, &response_code, 5000);

	vQueueUnregisterQueue( TaskQueue );
	vQueueDelete( TaskQueue );
	vTaskDelete( xHandle );
}

//Watch a task and notify scheduler if deadline passed
void Watch_Deadline(TimerHandle_t xTimer){
	TaskParams params = {.timer = xTimer, .task_id=-1};
	SchedulerMessage message = {NULL, params};
	xQueueSend(SchedulerQueue, &message, 5000);
}

/* The periodic task function
 *
 * 1. Delays for the execution time
 * 2. Delete the timer
 * 3. Delete self
 */
void UserPeriodicTask(TaskParams* params){
	//delay
	for(;;){
		userTaskDelay(params->execution_time);
		xTimerDelete(params->timer, 0);
		dd_delete(params->task_id);
	}
}

/* The Aperiodic task function
 *
 * 1. Delays for the execution time
 * 2. Delete the timer
 */
void UserAPeriodicTask(TaskParams* params){
	//delay
	for(;;){
		userTaskDelay(params->execution_time);
		xTimerDelete(params->timer, 0);
		dd_delete(params->task_id);
	}
}

static void userTaskDelay(uint32_t delay_time){
	if(!MONITORING){
		uint32_t prev = xTaskGetTickCount();
		uint32_t curr;
		uint32_t remaining = delay_time;
		while(remaining>0){
			curr = xTaskGetTickCount();
			if(curr>prev){
				remaining--;
				prev=curr;
			}
		}
	}
	else{
	 vTaskDelay(delay_time);
	}
}

/*
 * Parameters: Task params
 *
 * 1. Create a timer to watch for the tasks deadline
 * 1. Create the task in freertos
 * 2. add timer_id, task_id to params struct
 * 3. Create a queue for the scheduler to respond to
 * 4. Send the params to the scheduler queue
 * 5. Recieve message from created queue
 * 6. Delete the queue
 */
void dd_tcreate(TaskParams *params){
	TimerHandle_t timer = xTimerCreate("watch_deadline", params->deadline, pdFALSE, (void *)0, Watch_Deadline);
	xTimerStart(timer, 0);
	TaskHandle_t xHandle = NULL;
	if (params->task_type == PERIODIC){
		xTaskCreate(UserPeriodicTask, "User", configMINIMAL_STACK_SIZE, params, LOW, &xHandle);
	}else{
		xTaskCreate(UserAPeriodicTask, "User", configMINIMAL_STACK_SIZE, params, LOW, &xHandle);
	}

	params->timer = timer;
	params->task_id = xHandle;
	params->command = CREATE;

	xQueueHandle TaskQueue = xQueueCreate(QUEUE_LENGTH, sizeof(int));
    vQueueAddToRegistry(TaskQueue, "messageQueue");

	SchedulerMessage message = {TaskQueue, *params};
	xQueueSend(SchedulerQueue, &message, 50);

	int response_code;
	xQueueReceive(TaskQueue, &response_code, 5000);

	//do something with the response code

	vQueueUnregisterQueue( TaskQueue );
	vQueueDelete( TaskQueue );
}


// Returns the active list
TaskList* dd_return_active_list(){
	TaskParams params = {.task_id = ACTIVE};
	TaskList* list = getList(params);
	return list;
}

// Returns the overdue list
TaskList* dd_return_overdue_list(){
	TaskParams params = {.task_id = OVERDUE};
	TaskList* list = getList(params);
	return list;
}

// Returns a generic list
TaskList* getList(TaskParams params){
	xQueueHandle TaskQueue = xQueueCreate(QUEUE_LENGTH, sizeof(TaskList*));
	vQueueAddToRegistry(TaskQueue, "messageQueue");

	SchedulerMessage message = {TaskQueue, params};
	xQueueSend(SchedulerQueue, &message, 50);

	TaskList *list = NULL;
	xQueueReceive(TaskQueue, &list, 5000);

	//do something with the response code

	vQueueUnregisterQueue( TaskQueue );
	vQueueDelete( TaskQueue );
	return (list);
}

/*
 * Parameters: Task params, task list
 *
 * 1. Find the item in the list that needs to be removed
 * 2. Update the priority of the next element to be medium
 * 3. Remove item from list
 */
TaskList* removeFromActiveList(TaskParams params, TaskList **list){
//	TickType_t ticks = xTaskGetTickCount();
//	uint32_t normalized_time = ticks;
	TaskList *curr = (*list);
	TaskList *prev = NULL;
	//Find the one to remove
	while(curr->params.task_id!=params.task_id && curr->params.timer!=params.timer){
		prev = curr;
		curr=curr->next;
		if(curr==NULL){
			return *list;
		}
	}
	//If the one to remove is head
	if(prev==NULL){
		(*list) = curr->next;
		(*list)->prev = NULL;
		vTaskPrioritySet((*list)->params.task_id, MEDIUM);
//		printf("Increasing priority DELETE: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		return curr;
	}
	TaskList *temp = curr;
	prev->next = curr->next;
	curr = curr->next;
	curr->prev = prev;
	vTaskPrioritySet((*list)->params.task_id, MEDIUM);
//	printf("Increasing priority DELETE: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
	return temp;
}

/*
 * Parameters: Task entry, task list
 *
 * 1. Find the place in the list to insert the item, sorted based on deadlines
 * 2. if item is first in list, update priority to be medium and change heads priority to be low
 * 3. Insert item into proper spot
 */
void addToList(TaskList* entry, TaskList **list){
//	TickType_t ticks = xTaskGetTickCount();
//	uint32_t normalized_time = ticks;
	//If list is empty
	if((*list) == NULL){
		(*list) = entry;
//		printf("Increasing priority HEAD: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		vTaskPrioritySet(entry->params.task_id, MEDIUM);
		return;
	}
	TaskList *curr = (*list)->next;
	TaskList *prev = (*list);
	//if head is needed to be switched
	if((prev->params.creation_time+prev->params.deadline)>(entry->params.creation_time+entry->params.deadline)){
//		printf("Lowering priority: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		vTaskPrioritySet((*list)->params.task_id, LOW);
		entry->next = (*list);
		entry->prev = NULL;
		if ((*list)!=NULL){
			(*list)->prev = entry;
		}
		(*list) = entry;
		vTaskPrioritySet((*list)->params.task_id, MEDIUM);
//		printf("Increasing priority: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		return;
	}
	while(curr!=NULL && (entry->params.creation_time+entry->params.deadline) > (curr->params.creation_time+curr->params.deadline)){
		prev = curr;
		curr=curr->next;
	}
	//if inserting at end
	if(curr==NULL){
		prev->next = entry;
		entry->prev = prev;
		entry->next=NULL;
		return;
	}
	//Inserting middle
	prev->next = entry;
	entry->next = curr;
	entry->prev = prev;
	curr->prev = entry;
}


void limitLength(TaskList **list){
	TaskList *curr = (*list);
	int counter=0;
	while(curr!=NULL){
		counter++;
		curr=curr->next;
		if(counter>MAX_LIST_LENGTH){
			TaskList *temp = (*list);
			(*list) = (*list)->next;
			vPortFree(temp);
			return;
		}
	}
	return counter;
}

/*
 * 1. If a timer has gone off, remove from active list, delete and add to overdue list
 * 2. If a get list request, respond to queue
 * 3. If delete request, remove from active list and respond
 * 4. else create a new task and insert into list
 */
void DD_Scheduler_Task(){
	SchedulerMessage message;
	TaskList *active = NULL;
	TaskList *overdue = NULL;
	for(;;){
		if (xQueueReceive(SchedulerQueue, &message, 50000)){
			if(active!=NULL){
				turnLightOn(active->params.light);
			}else{
				turnLightOff();
			}
//			printf("Heap size: %d, free space: %d\n", configTOTAL_HEAP_SIZE, xPortGetFreeHeapSize());
			//timer message
			if(message.params.task_id==TIMER){
				TaskList *entry = removeFromActiveList(message.params, &active);
				entry->next = NULL;
				entry->prev = NULL;
				vTaskDelete(entry->params.task_id);
				addToList(entry, &overdue);
				limitLength(&overdue);
			//dd_return_active_list
			}else if(message.params.task_id==ACTIVE){
				xQueueSend(message.queue, &active, 50);
			//dd_return_overdue_list
			}else if(message.params.task_id==OVERDUE){
				xQueueSend(message.queue, &overdue, 50);
			}else{
				if(message.params.command==DELETE){
					TaskList *entry = removeFromActiveList(message.params, &active);
					vPortFree(entry);
					xQueueSend(message.queue, 1, 50);
				}else{
					TaskList *entry = (TaskList*)pvPortMalloc(sizeof(TaskList));
					entry->next = NULL;
					entry->prev = NULL;
					entry->params = message.params;
					TickType_t ticks = xTaskGetTickCount();
					entry->params.creation_time = ticks;
					addToList(entry, &active);
					xQueueSend(message.queue, 1, 50);
				}
			}
		}
	}
}

void Generator_Task1(){
	//Example 1: Blue, Green, Red rotating(no pre-empt)
	//TaskParams params = {.period = 5000, .deadline = 4997, .execution_time=1000, .task_type=PERIODIC, .light=blue};

	//Example 2: Blue(pre-empted), Green(pre-empted), Red, Green, Blue
	//TaskParams params = {.period = 5000, .deadline = 5000, .execution_time=1000, .task_type=PERIODIC, .light=blue};

	//Test Bench 1:
	TaskParams params = {.period = 5000, .deadline = 4999, .execution_time=950, .task_type=PERIODIC, .light=blue};

	for(;;){
		dd_tcreate(&params);
		vTaskDelay(params.period);
	}
}

void Generator_Task2(){
	//Example 1:
	//TaskParams params = {.period = 5000, .deadline = 4998, .execution_time=1000, .task_type=PERIODIC, .light=green};
	//vTaskDelay(1500);


	//Example 2:
	//TaskParams params = {.period = 5000, .deadline = 4000, .execution_time=1000, .task_type=PERIODIC, .light=green};
	//vTaskDelay(500);

	//Test Bench 1:
	TaskParams params = {.period = 5000, .deadline = 5000, .execution_time=1500, .task_type=PERIODIC, .light=green};

	for(;;){
		dd_tcreate(&params);
		vTaskDelay(params.period);
	}
}

void Generator_Task3(){
	//Example 1:
	//TaskParams params = {.period = 5000, .deadline = 4999, .execution_time=1000, .task_type=PERIODIC, .light=red};
	//vTaskDelay(3000);

	//Example 2:
	//TaskParams params = {.period = 5000, .deadline = 2500, .execution_time=1000, .task_type=PERIODIC, .light=red};
	//vTaskDelay(1000);

	//Test Bench 1:
	TaskParams params = {.period = 7500, .deadline = 7500, .execution_time=2500, .task_type=PERIODIC, .light=red};

	for(;;){
		dd_tcreate(&params);
		vTaskDelay(params.period);
	}
}

void Aperiodic_Generator(){
	TaskParams params = {.period = 750, .deadline = 750, .execution_time=250, .task_type=APERIODIC};
	dd_tcreate(&params);
	for(;;){
		vTaskDelay(500000);
	}
}

void turnLightOff(){
	STM_EVAL_LEDOff(green);
	STM_EVAL_LEDOff(red);
	STM_EVAL_LEDOff(blue);
	STM_EVAL_LEDOff(amber);
}

void turnLightOn(int light){
	STM_EVAL_LEDOff(green);
	STM_EVAL_LEDOff(red);
	STM_EVAL_LEDOff(blue);
	STM_EVAL_LEDOff(amber);
	STM_EVAL_LEDOn(light);
}

// Used to check the processor utilization
void Processor_Delay(TimerHandle_t xTimer){
	TaskList* active = dd_return_active_list();
	TaskList* overdue = dd_return_overdue_list();
	if(active!=NULL){
		ACTIVE_COUNTER++;
	}else{
		IDLE_COUNTER++;
	}
}


//Monitors the tasks
void Monitor_Task(){
	TaskList* active;
	TaskList* overdue;
	for(;;){
		active = dd_return_active_list();
		overdue = dd_return_overdue_list();
		printf("Processor utilization %d/%d\n",ACTIVE_COUNTER,(IDLE_COUNTER+ACTIVE_COUNTER));
		if(MONITORING){
			if(active!=NULL){
				printf("Active list highest priority: {deadline: %d, execution_time: %d, creation_time: %d}\n",active->params.deadline, active->params.execution_time,active->params.creation_time);
			}
			if(overdue!=NULL){
				printf("overdue list head: {deadline: %d, execution_time: %d, creation_time: %d}\n",overdue->params.deadline, overdue->params.execution_time, overdue->params.creation_time);
			}
		}
		IDLE_COUNTER++;
		vTaskDelay(500);
	}
}


int main(void) {
  STM_EVAL_LEDInit(amber);
  STM_EVAL_LEDInit(green);
  STM_EVAL_LEDInit(red);
  STM_EVAL_LEDInit(blue);

  prvSetupHardware();

  //start the timer to monitor
  TimerHandle_t monitor_timer = xTimerCreate("watch_processor", 5, pdTRUE, (void *)0, Processor_Delay);
  xTimerStart(monitor_timer, 0);

  // Initialize the four queues needed to communicate
  SchedulerQueue = xQueueCreate(QUEUE_LENGTH, sizeof(SchedulerMessage));

  // Add the queues to the registry
  vQueueAddToRegistry(SchedulerQueue, "SchedulerQueue");

  //Periodic tasks
  xTaskCreate(Generator_Task1, "Generator1", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);
  xTaskCreate(Generator_Task2, "Generator2", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);
  xTaskCreate(Generator_Task3, "Generator3", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);

  //Scheduler
  xTaskCreate(DD_Scheduler_Task, "DDScheduler", configMINIMAL_STACK_SIZE, NULL, SCHEDULER, NULL);
  //Monitor
  xTaskCreate(Monitor_Task, "MonitorTask", configMINIMAL_STACK_SIZE, NULL, MONITOR, NULL);

  //APeriodic task
//  xTaskCreate(Aperiodic_Generator, "Aperiodic_Generator", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);

  // Start the scheduler
  vTaskStartScheduler();

  return 0;
}

void vApplicationMallocFailedHook(void) {
  /* The malloc failed hook is enabled by setting
  configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

  Called if a call to pvPortMalloc() fails because there is insufficient
  free memory available in the FreeRTOS heap.  pvPortMalloc() is called
  internally by FreeRTOS API functions that create tasks, queues, software
  timers, and semaphores.  The size of the FreeRTOS heap is set by the
  configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
  for (;;)
    ;
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook(xTaskHandle pxTask,
                                   signed char *pcTaskName) {
  (void)pcTaskName;
  (void)pxTask;

  /* Run time stack overflow checking is performed if
  configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
  function is called if a stack overflow is detected.  pxCurrentTCB can be
  inspected in the debugger if the task name passed into this function is
  corrupt. */
  for (;;)
    ;
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook(void) {
//  volatile size_t xFreeStackSpace;
//
//  /* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
//  FreeRTOSConfig.h.
//
//  This function is called on each cycle of the idle task.  In this case it
//  does nothing useful, other than report the amount of FreeRTOS heap that
//  remains unallocated. */
////  xFreeStackSpace = xPortGetFreeHeapSize();
//
//  if (xFreeStackSpace > 100) {
//    /* By now, the kernel has allocated everything it is going to, so
//    if there is a lot of heap remaining unallocated then
//    the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
//    reduced accordingly. */
//  }
}
/*-----------------------------------------------------------*/

static void prvSetupHardware(void) {
  /* Ensure all priority bits are assigned as preemption priority bits.
  http://www.freertos.org/RTOS-Cortex-M3-M4.html */
  NVIC_SetPriorityGrouping(0);

  /* TODO: Setup the clocks, etc. here, if they were not configured before
  main() was called. */
}
