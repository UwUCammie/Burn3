#include "crash_report.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>
#include <typeinfo>

namespace TakedownRenderer
{
namespace
{
std::atomic<bool> reported{false};

// "module.dll+0x1234", or the raw address when it is in no module.
void print_location(const char *prefix, const void *address)
{
	HMODULE module = nullptr;
	char path[MAX_PATH] = {};
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       static_cast<LPCSTR>(address), &module) &&
	    module && GetModuleFileNameA(module, path, MAX_PATH))
	{
		const char *name = path;
		for (const char *p = path; *p; p++)
			if (*p == '\\' || *p == '/')
				name = p + 1;
		std::fprintf(stderr, "%s%s+0x%llx\n", prefix, name,
		             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
		                                             reinterpret_cast<uintptr_t>(module)));
	}
	else
		std::fprintf(stderr, "%s0x%llx\n", prefix,
		             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
}

void print_modules()
{
	// Base addresses of the modules that matter for resolving offsets by hand.
	const char *names[] = { nullptr, "vulkan-1.dll", "amdvlk64.dll", "SDL3.dll", "libstdc++-6.dll" };
	for (const char *name : names)
		if (HMODULE module = GetModuleHandleA(name))
		{
			char path[MAX_PATH] = {};
			GetModuleFileNameA(module, path, MAX_PATH);
			std::fprintf(stderr, "[CRASH]:   module %p %s\n", static_cast<void *>(module), path);
		}
}

// Unwinds from a captured context using the x64 unwind tables.
void print_stack(CONTEXT context)
{
	for (int frame = 0; frame < 48 && context.Rip; frame++)
	{
		char prefix[32];
		std::snprintf(prefix, sizeof(prefix), "[CRASH]:   #%02d ", frame);
		print_location(prefix, reinterpret_cast<const void *>(context.Rip));
		DWORD64 image_base = 0;
		PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
		if (!function)
		{
			// Leaf function: the return address is on top of the stack.
			if (!context.Rsp)
				break;
			context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
			context.Rsp += 8;
			continue;
		}
		void *handler_data = nullptr;
		DWORD64 establisher = 0;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
		                 &establisher, nullptr);
	}
}

void report_here(const char *what)
{
	if (reported.exchange(true))
		return;
	std::fprintf(stderr, "[CRASH]: %s (thread %lu)\n", what, GetCurrentThreadId());
	CONTEXT context = {};
	RtlCaptureContext(&context);
	print_stack(context);
	print_modules();
	std::fflush(stderr);
}

LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *info)
{
	if (!reported.exchange(true))
	{
		const EXCEPTION_RECORD *record = info->ExceptionRecord;
		std::fprintf(stderr, "[CRASH]: exception 0x%08lx (thread %lu)\n", record->ExceptionCode,
		             GetCurrentThreadId());
		print_location("[CRASH]: at ", record->ExceptionAddress);
		if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
			std::fprintf(stderr, "[CRASH]: %s address 0x%llx\n",
			             record->ExceptionInformation[0] == 0 ? "reading" :
			             record->ExceptionInformation[0] == 8 ? "executing" : "writing",
			             static_cast<unsigned long long>(record->ExceptionInformation[1]));
		print_stack(*info->ContextRecord);
		print_modules();
		std::fflush(stderr);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int)
{
	report_here("abort()");
}

void on_terminate()
{
	const char *type = "no active exception";
	const char *message = "";
	if (std::exception_ptr current = std::current_exception())
	{
		type = "unknown exception";
		try
		{
			std::rethrow_exception(current);
		}
		catch (const std::exception &e)
		{
			type = typeid(e).name();
			message = e.what();
		}
		catch (...)
		{
		}
	}
	std::fprintf(stderr, "[CRASH]: std::terminate: %s %s\n", type, message);
	report_here("std::terminate");
	std::abort();
}
}

void install_crash_report()
{
	SetUnhandledExceptionFilter(unhandled_exception);
	std::signal(SIGABRT, on_abort);
	std::set_terminate(on_terminate);
}
}
#else
namespace TakedownRenderer
{
void install_crash_report()
{
}
}
#endif
