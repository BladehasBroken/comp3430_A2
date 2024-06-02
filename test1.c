#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <signal.h>
#include "common_threads.h"

#define DEFAULT_TIMESLICE 20
#define MAXIMUM_TASK 65536
#define MAXIMUM_COMPUTEANDIO 255

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_dispatch = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_schedule = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_dispatcher_working = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_task_done = PTHREAD_MUTEX_INITIALIZER;

//static pthread_cond_t dispatch_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t schedule_cond = PTHREAD_COND_INITIALIZER;


typedef struct {
    int task_id;
    int start_time;
    int tasks[MAXIMUM_COMPUTEANDIO];
    int max_task;
    int curr;
    int task_start_time;
    int block_start_time;
    int block_end_time;
    int remaining_time;
    int running_time;
    bool done;
} Task;

typedef struct {
    int items[MAXIMUM_TASK];
    int front;
    int rear;
} Queue;


int time_slice = DEFAULT_TIMESLICE;
int num_tasks = 0;
int compelete_task = 0;
int current_task = -1;
int count = 0;
long start_time;
long current_time;
bool dispatcher_working = false;
bool task_done = false;

Task task_list[MAXIMUM_TASK];

Queue pre_ready;

// get the current time in ms
long get_current_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}



void get_future_time(struct timespec *ts, int milliseconds) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec + milliseconds / 1000;
    ts->tv_nsec = (tv.tv_usec * 1000) + (milliseconds % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

int find_task(int task_id){
    int index = 0;
    for(int i = 0; i < num_tasks; i++){
        if(task_list[i].task_id == task_id){
            index = i;
            break;
        }
    }
    return index;
}


// Initialize queue
void initQueue(Queue *q) {
    q->front = -1;
    q->rear = -1;
}

// check if the queue is empty
bool isEmpty(Queue *q) {
    return q->front == -1;
}

// check if the queue is full
bool isFull(Queue *q) {
    return q->rear == MAXIMUM_TASK - 1;
}

// add item into the queue
void enqueue(Queue *q, int value) {
    if (isFull(q)) {
        printf("The queue is full\n");
        return;
    }
    if (isEmpty(q)) {
        q->front = 0;
    }
    q->rear++;
    q->items[q->rear] = value;
    //printf("insert %d\n", value);
}

// remove item from queue
int dequeue(Queue *q) {
    int item  = -1; 
    if (isEmpty(q)) {
        printf("the queue is empty, can't dequeue\n");
        return item;
    }
    item = q->items[q->front];
    q->front++;
    if (q->front > q->rear) {
        q->front = q->rear = -1;
    }
    //printf("remove %d\n", item);
    return item;
}


// print the item in queue
void printQueue(Queue *q) {
    if (isEmpty(q)) {
        printf("The queue is empty\n");
    } else {
        printf("the item in queue: ");
        for (int i = q->front; i <= q->rear; i++) {
            printf("%d ", q->items[i]);
            
            int index = find_task(q -> items[i]);
            printf("task's start time is %d\n", task_list[index].start_time);
        }
        printf("\n");
    }
}

// rewrite the compare function for qsort()
int compare_blocked(const void *a, const void *b) {
    int indexA = *(int *)a;
    int indexB = *(int *)b;
    return task_list[indexA].block_end_time - task_list[indexB].block_end_time;
}

void sortQueue(Queue *q) {
    if (!isEmpty(q)) {
        qsort(q->items + q->front, q->rear - q->front + 1, sizeof(int), compare_blocked);
    }
}

void signal_handler(int sig) {
    if (sig == SIGUSR1) {
        printf("Signal...\n");
        int finished_time = get_current_time() - start_time;
        task_list[current_task].running_time += finished_time - task_list[current_task].task_start_time;
        task_list[current_task].remaining_time -= finished_time - task_list[current_task].task_start_time;
        fprintf(stdout,"Signal, Task %d finished on time %ld\n", current_task, get_current_time() - start_time);
        pthread_exit(NULL); 
    }
}

void *task(void *arg){
    task_list[current_task].task_start_time = get_current_time() - start_time;
    printf("task %d start on %d\n", current_task, task_list[current_task].task_start_time);
    int task_end_time = task_list[current_task].task_start_time + task_list[current_task].remaining_time;
    //printf("task end time is %d\n", task_end_time);
    signal(SIGUSR1, signal_handler);
    while(get_current_time() - start_time < task_end_time){

    }
    int finished_time = get_current_time() - start_time;
    task_list[current_task].running_time += finished_time - task_list[current_task].task_start_time;
    task_list[current_task].remaining_time -= finished_time - task_list[current_task].task_start_time;
    fprintf(stdout,"Task %d finished on time %d\n", current_task, finished_time);
    fprintf(stderr, "The remaining time for task %d is %d\n", current_task, task_list[current_task].remaining_time);
    pthread_mutex_lock(&lock_task_done);
    task_done =true;
    pthread_mutex_unlock(&lock_task_done);
    return NULL;
}

void *dispatcher(void *arg){
    int run = 0;
    pthread_t run_task;
    Queue blocked;
    initQueue(&blocked);
    int prev_task = -1;
    while(compelete_task < num_tasks){
        if (run == 0) {
            pthread_mutex_lock(&lock_schedule);
            while(dispatcher_working == false ){
                pthread_cond_wait(&schedule_cond, &lock_schedule);
            }
            if(current_task != -1) {
                run = 1;
            }
            pthread_mutex_unlock(&lock_schedule);
        }

        if( run == 1){
            prev_task = current_task;

            printf("the current task %d's start time is %d, the current task is %d\n", current_task, task_list[current_task].start_time, task_list[current_task].tasks[task_list[current_task].curr]);
            task_list[current_task].remaining_time = task_list[current_task].tasks[0];
            int signal_time = get_current_time() - start_time + time_slice;
            Pthread_create(&run_task, NULL, task, NULL);

            // time slice interrupt
            while(get_current_time() - start_time <= signal_time){
                if(task_done == true){
                    pthread_mutex_lock(&lock_task_done);
                    task_done = false;
                    pthread_mutex_unlock(&lock_task_done);
                    break;
                }
                if(get_current_time() - start_time == signal_time){
                    //task_done = false;
                    pthread_kill(run_task, SIGUSR1);
                    break;
                }
            }

            Pthread_join(run_task, NULL);

            // check the blocked queue
            int curr_time = get_current_time() - start_time;
            int index = 0;
            while(index <= blocked.rear){
                if(curr_time >= task_list[index].block_end_time){
                    pthread_mutex_lock(&lock);
                    task_list[index].curr++;
                    enqueue(&pre_ready, dequeue(&blocked));
                    pthread_mutex_unlock(&lock);
                }else{
                    break;
                }
                index++;
            }

            // do status transition
            if(task_list[current_task].remaining_time == 0){ // finish the current computing task
                if(task_list[current_task].curr < task_list[current_task].max_task){ // not the last computing task
                    task_list[current_task].curr ++;
                    if(task_list[current_task].curr % 2 == 0){ // if it is the computing task
                        pthread_mutex_lock(&lock);
                        enqueue(&pre_ready, current_task);
                        pthread_mutex_unlock(&lock);
                    }else{ // if it is the IO request
                        task_list[current_task].remaining_time = task_list[current_task].tasks[task_list[current_task].curr];
                        task_list[current_task].block_start_time = get_current_time() - start_time;
                        task_list[current_task].block_end_time = task_list[current_task].block_start_time + task_list[current_task].remaining_time;
                        enqueue(&blocked, current_task);
                        fprintf(stderr, "put task %d into the blocked\n", current_task);
                        sortQueue(&blocked);
                    }
                }else{ // finish the last computing task
                    task_list[current_task].done = true;
                    compelete_task++;
                }
            }else { // not finish the current computing task
                task_list[current_task].tasks[task_list[current_task].curr] = task_list[current_task].remaining_time;
                pthread_mutex_lock(&lock);
                enqueue(&pre_ready, current_task);
                pthread_mutex_unlock(&lock);
            }

            pthread_mutex_lock(&lock_dispatcher_working);
            dispatcher_working = false;
            pthread_mutex_unlock(&lock_dispatcher_working);
            run = 0;
        }
        //pthread_mutex_unlock(&lock_dispatch);
    }
    return NULL;
}

void *scheduler(void *arg){
    Queue ready;
    initQueue(&ready);
    int i = 0;
    while(compelete_task < num_tasks){
        // launcher send signal to push a new task into queue
        if(i < num_tasks){
            if(isEmpty(&pre_ready) == 0){
                int current_time = get_current_time() - start_time;
                pthread_mutex_lock(&lock);
                int new_task = dequeue(&pre_ready);
                enqueue(&ready, new_task);
                pthread_mutex_unlock(&lock);
                printf("The time add task %d is %d\n", new_task, current_time);
            }
        }

        pthread_mutex_lock(&lock_schedule);
        if(dispatcher_working == false && isEmpty(&ready) == 0){
            current_task = dequeue(&ready);
            dispatcher_working = true;
                pthread_cond_signal(&schedule_cond);
        }
        pthread_mutex_unlock(&lock_schedule);
        
    }
    printQueue(&ready);
    return NULL;
}

// read the data from stdin and sort them based on arrival time
void *launcher(void *arg){
    pthread_t schedular;
    pthread_t dispatch;
    char input[MAXIMUM_COMPUTEANDIO + 1];
    int i = 0;
    printf("Please enter the numbers(press ctrl + d to end input):\n");
    while (fgets(input, sizeof(input), stdin) != NULL) {
        input[strcspn(input, "\n")] = '\0';
        const char delimiters[] = ":,";
        char *token;
        if (num_tasks < MAXIMUM_TASK) {
            task_list[i].task_id = i;
            token = strtok(input, delimiters);
            task_list[i].start_time = atoi(token);
            token = strtok(NULL, delimiters);
            int j = 0;
            while(token != NULL){
                task_list[i].tasks[j] = atoi(token);
                token = strtok(NULL, delimiters);
                //printf("task[%d][%d] is %d\n", i, j, task_list[i].tasks[j]);
                j++;
            }
            task_list[i].max_task = j;
            task_list[i].curr = 0;
            task_list[i].running_time = 0;
            task_list[i].done = false;
            i++;
            num_tasks++;
        }
    }


    initQueue(&pre_ready);

    Pthread_create(&schedular, NULL, scheduler, NULL);
    Pthread_create(&dispatch, NULL, dispatcher, NULL);

    start_time = get_current_time();
    printf("start time is %lu\n", start_time);

    int j = 0;
    while( j < num_tasks) {
        //current_time = get_current_time() - start_time;
        //printf("current time is %lu\n", current_time);
        current_time = get_current_time() - start_time;
        //printf("current time is %lu\n", current_time);

        if(current_time < task_list[j].start_time) {
            //printf("sleep time for %d is %lu\n", i, task_list[j].start_time - current_time);
            usleep((task_list[j].start_time - current_time)*1000);
        } 
        pthread_mutex_lock(&lock);
        enqueue(&pre_ready, j);
        pthread_mutex_unlock(&lock);
        j++;
    }

    Pthread_join(schedular, NULL);
    Pthread_join(dispatch, NULL);

    return NULL;
}

int main(int argc, char *argv[]){
    bool detail = false;
    pthread_t launch;
    if(argc > 1){
        char command[20];
        strcpy(command, argv[1]);
        if(argc == 2){
            if(strchr(command, '-') != NULL){
                detail = true;
            }else{
                time_slice = atoi(command);
            }
        }else if(argc == 3){
            if(strchr(command, '-') != NULL){
                detail = true;
                time_slice = atoi(argv[2]);
            }else {
                fprintf(stderr, "usage: ./q1 [-v] [number]\n");
                exit(-1);
            }
        }else {
            fprintf(stderr, "usage: ./q1 [-v] [number]\n");
            exit(-1);
        }
    }

    Pthread_create(&launch, NULL, launcher, NULL);

    Pthread_join(launch, NULL);



    printf("time slice is %d, detail is %d\n", time_slice, detail);
    return 0;
}