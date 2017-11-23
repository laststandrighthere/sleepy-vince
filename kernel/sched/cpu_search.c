#include "cpu_search.h"
#define TDQ(rq)		(&(rq)->ktz)

static int _rand()
{
	int r;
	//get_random_bytes(&r, sizeof(r));
	return r;
}

