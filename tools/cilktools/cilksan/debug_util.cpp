#include <cstdarg>                                                                                  
#include <cstdio>                                                                                   
#include <cstdlib>                                                                                  
#include <strings.h>                                                                                
#include "debug_util.h"

/*
static void print_bt(FILE *f) {
    const int N=10;
    void *buf[N];
    int n = backtrace(buf, N);
    if (1) {
        for (int i = 0; i < n; i++) {
            print_addr(f, buf[i]);
        }
    } else {
        assert(n>=2);
        print_addr(f, buf[2]);
    }
}
*/

void debug_printf(int level, const char *fmt, ...) {
    if(debug_level & level) {
        std::va_list l;
        va_start(l, fmt);
        std::vfprintf(stderr, fmt, l);
        va_end(l);
    }
}

// Print out the error message and exit
__attribute__((noreturn))
void die(int exit_code, const char *fmt, ...) {

    std::va_list l;
    std::fprintf(stderr, "=================================================\n");
    if(exit_code == ERR_EXIT_CODE) {
        std::fprintf(stderr, "racedetector: fatal error\n");
    } else {
        std::fprintf(stderr, "racedetector: race found\n");
    }

    va_start(l, fmt);
    std::vfprintf(stderr, fmt, l);
    std::fprintf(stderr, "=================================================\n");
    fflush(stderr);
    va_end(l);
    std::exit(exit_code);
}

