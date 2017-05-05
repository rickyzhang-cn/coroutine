#include <stdio.h>
#include <ucontext.h> 
#include <unistd.h> 
int main(int argc, char *argv[]) 
{ 
  ucontext_t context; 
  getcontext(&context); 
  puts("Hello world"); 
  sleep(1); 
  setcontext(&context); //这里就相当于goto 
  return 0; 
}
