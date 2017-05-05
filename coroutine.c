#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

/* 提供给外部使用的接口有coroutine_open/close，coroutine_new，coroutine_resume/yield，coroutine_status
 * 所有的协程使用调度器提供的stack，但都有自己的stack用于保存公共stack上的信息
 * 使用makecontext，swapcontext系统调用来进行上下文切换，加强版sigsetjmp，siglongjmp */
struct coroutine;

struct schedule {
	char stack[STACK_SIZE]; //协程共享的栈
	ucontext_t main; //用于保存现场的上下文
	int nco; //协程个数
	int cap; //协程最多的个数
	int running; //当前运行协程的编号
	struct coroutine **co; //协程结构体数组
};

struct coroutine {
	coroutine_func func; //协程运行函数
	void *ud; //函数参数
	ucontext_t ctx; //协程上下文
	struct schedule * sch; //协程的调度器
	ptrdiff_t cap; 
	ptrdiff_t size;
	int status; //协程状态
	char *stack; //用于保存stack
};

//初始化协程数据结构的内容
struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL;
	return co;
}

void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co);
}

struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE;
	S->running = -1;
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

//在调度器S中创建一个协程，其运行函数为func，参数为ud
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
	struct coroutine *co = _co_new(S, func , ud);
	if (S->nco >= S->cap) { //当调度器中的协程数组不够大时，需要进行扩容
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else { //寻找协程数组中为NULL的地方，填写新创建的协程
		int i;
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

//唤醒调取器中协程数组中索引为id的协程
void 
coroutine_resume(struct schedule * S, int id) {
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
	case COROUTINE_READY:
		getcontext(&C->ctx); //对当前协程的上下文进行初始化
		//协程运行之前需要设置其stack位置和大小
        C->ctx.uc_stack.ss_sp = S->stack; 
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		C->ctx.uc_link = &S->main; //当前上下文运行完成后，后续运行的上下文
		S->running = id;
		C->status = COROUTINE_RUNNING;
		uintptr_t ptr = (uintptr_t)S;
        //设置当前上下文的执行入口为mainfunc，以及其参数
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
		//协程相关设置完成后，进行切换
        swapcontext(&S->main, &C->ctx);
		break;
	case COROUTINE_SUSPEND: //当协程处于中止状态时
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size); //将协程的stack内容拷贝到公共stack中去
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx); //切换上下文，保存当前上下文到S->main中
		break;
	default:
		assert(0);
	}
}

static void
_save_stack(struct coroutine *C, char *top) {
	char dummy = 0; //dummy是在栈顶，其地址值最小
	assert(top - &dummy <= STACK_SIZE);
	//协程用于存储stack信息的空间在这里动态申请
    if (C->cap < top - &dummy) {
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	C->size = top - &dummy;
	memcpy(C->stack, &dummy, C->size);
}

//协程主动放弃执行权限，进入中止休眠状态
void
coroutine_yield(struct schedule * S) {
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	_save_stack(C,S->stack + STACK_SIZE);
	C->status = COROUTINE_SUSPEND;
	S->running = -1;
	swapcontext(&C->ctx , &S->main); //交还运行
}

int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

int 
coroutine_running(struct schedule * S) {
	return S->running;
}

