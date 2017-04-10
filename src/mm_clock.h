#ifndef MM_CLOCK_H_
#define MM_CLOCK_H_

/*
 * machinarium.
 *
 * cooperative multitasking engine.
*/

typedef struct mm_clock_t mm_clock_t;

struct mm_clock_t {
	mm_buf_t timers;
	int      timers_count;
	int      timers_seq;
	int      time;
};

void mm_clock_init(mm_clock_t*);
void mm_clock_free(mm_clock_t*);
int  mm_clock_step(mm_clock_t*);
int  mm_clock_timer_add(mm_clock_t*, mm_timer_t*);
int  mm_clock_timer_del(mm_clock_t*, mm_timer_t*);
mm_timer_t*
mm_clock_timer_min(mm_clock_t*);

#endif
