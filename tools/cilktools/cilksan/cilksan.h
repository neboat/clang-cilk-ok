#ifndef __CILKSAN_H__
#define __CILKSAN_H__

#if defined (__cplusplus)
extern "C" {
#endif

int  __cilksan_error_count(void);
void __cilksan_enable_checking(void);
void __cilksan_disable_checking(void);

#if defined (__cplusplus)
}
#endif

#endif // __CILKSAN_H__
