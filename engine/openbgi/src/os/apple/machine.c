/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - the Apple back end's machine services (inc/bgi/os.h): the
 *             memory figures, the clock counters, the version and adapter
 *             reports, and the shell services, which an application on
 *             iOS, tvOS or visionOS has no counterpart for
 *
 * This replaces src/os/posix/machine.c, whose sysinfo, fork and exec do not
 * exist on these systems.
 */
#include "apple_internal.h"

#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if __has_include(<os/proc.h>)
#include <os/proc.h>
#endif

// the physical memory, bytes
static uint64_t PhysicalMemory(void)
{
	uint64_t mem = 0;
	size_t len = sizeof mem;
	if(sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0)
		mem = 0;
	return mem;
}

/* What the application may still allocate: os_proc_available_memory on the
 * systems that end an application above a limit (iOS, tvOS, visionOS),
 * else the free and inactive pages. */
static uint64_t AvailableMemory(void)
{
#if __has_include(<os/proc.h>) && !TARGET_OS_OSX
	size_t avail = os_proc_available_memory();
	if(avail)
		return avail;
#endif
	{
		vm_statistics64_data_t st;
		mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
		vm_size_t page = 0;
		if(host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
			host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&st, &count) != KERN_SUCCESS)
			return PhysicalMemory() / 2;
		return ((uint64_t)st.free_count + st.inactive_count) * page;
	}
}

static uint32_t Cap32(uint64_t v)
{
	return v > 0xffffffffu ? 0xffffffffu : (uint32_t)v;
}

void OS_MemoryStatus(OsMemStatus_t* m)
{
	uint64_t total = PhysicalMemory(), avail = AvailableMemory();
	if(avail > total)
		avail = total;
	m->totalPhys = Cap32(total);
	m->availPhys = Cap32(avail);
	m->loadPercent = total ? (uint32_t)(100 - avail * 100 / total) : 0;
	m->totalPageFile = m->totalPhys;
	m->availPageFile = m->availPhys;
	m->totalVirtual = 0x7ffe0000u; // a 32-bit process's 2 GB, as the original sees it
	m->availVirtual = 0x7ffe0000u;
}

void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	*totalPhys = PhysicalMemory();
	*availPhys = AvailableMemory();
}

int OS_ComputerName(char* buf, size_t n)
{
	if(gethostname(buf, n) != 0)
		snprintf(buf, n, "APPLE");
	return 1;
}

int OS_UserName(char* buf, size_t n)
{
	snprintf(buf, n, "mobile");
	return 1;
}

// no x86 CPUID or time stamp counter on the Apple processors
int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
	BGI_UNUSED(leaf);
	out[0] = out[1] = out[2] = out[3] = 0;
	return 0;
}

uint64_t OS_ReadTsc(void)
{
	return 0;
}

uint32_t OS_CpuSignature(void)
{
	return 0;
}

int OS_PerfCounterNs(int64_t* out)
{
	static mach_timebase_info_data_t tb;
	uint64_t t = mach_absolute_time();
	if(tb.denom == 0)
		mach_timebase_info(&tb);
	*out = (int64_t)(t * tb.numer / tb.denom);
	return 1;
}

// the NT line, Windows 7 (6.1), so that the engine takes its NT paths
int OS_Version(int* platform, int* major, int* minor)
{
	*platform = 2;
	*major = 6;
	*minor = 1;
	return 1;
}

void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	out[0] = 6;
	out[1] = 1;
	out[2] = 7601;
	out[3] = 2;
	if(n)
		snprintf(csd, n, "Service Pack 1");
}

int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	snprintf(desc, n, "Apple GPU");
	version[0] = version[1] = version[2] = version[3] = 0;
	return 1;
}

// ---- processes and the shell: none on these systems --------------------------------------

OsProcess_t* OS_ProcessStart(const char* cmdLine)
{
	BGI_UNUSED(cmdLine);
	return NULL;
}

void OS_ProcessWaitIdle(OsProcess_t* p)
{
	BGI_UNUSED(p);
}

int OS_ProcessWait(OsProcess_t* p, uint32_t ms)
{
	BGI_UNUSED(p);
	BGI_UNUSED(ms);
	return 1;
}

void OS_ProcessClose(OsProcess_t* p)
{
	BGI_UNUSED(p);
}

int OS_ShellExecute(const char* file, const char* params, const char* dir)
{
	BGI_UNUSED(file);
	BGI_UNUSED(params);
	BGI_UNUSED(dir);
	return 0;
}

int OS_SetWallpaper(const char* path, int stretch, int tile)
{
	BGI_UNUSED(path);
	BGI_UNUSED(stretch);
	BGI_UNUSED(tile);
	return 0;
}

int OS_CreateShortcut(const char* linkPath, const char* target)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	return 0;
}

int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	BGI_UNUSED(args);
	return 0;
}
