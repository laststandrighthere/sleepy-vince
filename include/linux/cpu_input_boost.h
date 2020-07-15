/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_INPUT_BOOST_H_
#define _CPU_INPUT_BOOST_H_

#include <linux/types.h>

#ifdef CONFIG_CPU_INPUT_BOOST
void cpu_input_boost_kick(void);
void cpu_input_boost_kick_max(unsigned int duration_ms);
void cpu_input_boost_kick_wake(void);

bool cpu_input_boost_within_input(unsigned long timeout_ms);
#else
static inline void cpu_input_boost_kick(void)
{
}
static inline void cpu_input_boost_kick_max(unsigned int duration_ms)
{
}
static inline void cpu_input_boost_kick_wake(void)
{
}

static inline bool cpu_input_boost_within_input(unsigned long timeout_ms)
{
	return true;
}
#endif

#endif /* _CPU_INPUT_BOOST_H_ */
