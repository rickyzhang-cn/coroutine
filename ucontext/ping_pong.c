#include <ucontext.h>
#include <stdio.h>

#define MAX_COUNT (1<<30)

/* 这里是一个不会造成stackoverflow错误的大循环程序 */


//uc[0]用于切换时保存上下文信息
static ucontext_t uc[3];
static int count = 0;

void ping();
void pong();

void ping(){
    while(count < MAX_COUNT){
        printf("ping %d\n", ++count);
        // yield to pong
        swapcontext(&uc[1], &uc[2]); // 保存当前context于uc[1],切换至uc[2]的context运行
    }
}

void pong(){
    while(count < MAX_COUNT){
        printf("pong %d\n", ++count);
        // yield to ping
        swapcontext(&uc[2], &uc[1]);// 保存当前context于uc[2],切换至uc[1]的context运行
    }
}

char st1[8192];
char st2[8192];

int main(int argc, char *argv[]){


    // initialize context
    getcontext(&uc[1]);
    getcontext(&uc[2]);

    uc[1].uc_link = &uc[0]; // 这个玩意表示uc[1]运行完成后，会跳至uc[0]指向的context继续运行
    uc[1].uc_stack.ss_sp = st1; // 设置新的堆栈
    uc[1].uc_stack.ss_size = sizeof st1;
    makecontext (&uc[1], ping, 0);

    uc[2].uc_link = &uc[0]; // 这个玩意表示uc[2]运行完成后，会跳至uc[0]指向的context继续运行
    uc[2].uc_stack.ss_sp = st2; // 设置新的堆栈
    uc[2].uc_stack.ss_size = sizeof st2;
    makecontext (&uc[2], pong, 0);

    // start ping-pong
    swapcontext(&uc[0], &uc[1]); // 将当前context信息保存至uc[0],跳转至uc[1]保存的context去执行

    return 0;
}
