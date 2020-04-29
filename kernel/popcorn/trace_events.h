// SPDX-License-Identifier: GPL-2.0, BSD
#undef TRACE_SYSTEM
#define TRACE_SYSTEM popcorn

#if !defined(_TRACE_EVENTS_POPCORN_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EVENTS_POPCORN_H_

#include <linux/tracepoint.h>


TRACE_EVENT(pgfault,
	TP_PROTO(const int nid, const int pid, const char rw,
		const unsigned long instr_addr, const unsigned long addr,
		const int result),

	TP_ARGS(nid, pid, rw, instr_addr, addr, result),

	TP_STRUCT__entry(
		__field(int, nid)
		__field(int, pid)
		__field(char, rw)
		__field(unsigned long, instr_addr)
		__field(unsigned long, addr)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->nid = nid;
		__entry->pid = pid;
		__entry->rw = rw;
		__entry->instr_addr = instr_addr;
		__entry->addr = addr;
		__entry->result = result;
	),

	TP_printk("%d %d %c %lx %lx %d",
		__entry->nid, __entry->pid, __entry->rw,
		__entry->instr_addr, __entry->addr, __entry->result)
);


TRACE_EVENT(pgfault_stat,
	TP_PROTO(const unsigned long instr_addr, const unsigned long addr,
		const int result, const int retries, const unsigned long time_us),

	TP_ARGS(instr_addr, addr, result, retries, time_us),

	TP_STRUCT__entry(
		__field(unsigned long, instr_addr)
		__field(unsigned long, addr)
		__field(int, result)
		__field(int, retries)
		__field(unsigned long, time_us)
	),

	TP_fast_assign(
		__entry->instr_addr = instr_addr;
		__entry->addr = addr;
		__entry->result = result;
		__entry->retries = retries;
		__entry->time_us = time_us;
	),

	TP_printk("%lx %lx %d %d %lu",
		__entry->instr_addr, __entry->addr, __entry->result,
		__entry->retries, __entry->time_us)
);

#endif

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE

#define TRACE_INCLUDE_PATH ../../kernel/popcorn
#define TRACE_INCLUDE_FILE trace_events
#include <trace/define_trace.h>
