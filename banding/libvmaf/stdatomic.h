#ifndef __STDATOMIC_H__
#define __STDATOMIC_H__

// workaround lack of stdatomic.h on windows.
// The client should ensure race-free data accesses.
typedef int atomic_int;

static inline void atomic_init(atomic_int *p, int x) { *p = x; }
static inline int atomic_load(atomic_int *p) { return *p; }
static inline int atomic_fetch_add(atomic_int *p, int x) { int r = *p; *p += x; return r; }
static inline int atomic_fetch_sub(atomic_int *p, int x) { int r = *p; *p -= x; return r; }

#endif /* __STDATOMIC_H__ */
