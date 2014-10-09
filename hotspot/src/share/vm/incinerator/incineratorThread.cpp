
#include "precompiled.hpp"
#include "incinerator/incineratorThread.hpp"

namespace Incinerator {


IncineratorThread::IncineratorThread(ThreadRunnable& target) :
	JavaThread(IncineratorThread::entry_point),
	_started(NULL), _state(0), _target(target)
{
	initialize();
}

bool IncineratorThread::initialize()
{
	JavaThread* thread = JavaThread::current();
	ResourceMark rm(thread);
	HandleMark hm(thread);

	ThreadInVMfromNative thread_trans(thread);

	Klass* thread_klass = SystemDictionary::resolve_or_null(
		vmSymbols::java_lang_Thread(), thread);
	if (!thread_klass) return false;

	instanceKlassHandle thread_class_instance(thread, thread_klass);
	instanceHandle thread_obj =
		thread_class_instance->allocate_instance_handle(thread);

	// Initialize java.lang.Thread object to put it into the system threadGroup
	JavaValue result(T_VOID);
	JavaCalls::call_special(&result, thread_obj, thread_class_instance,
		vmSymbols::object_initializer_name(),
		vmSymbols::threadgroup_string_void_signature(),
		Handle(thread, Universe::system_thread_group()),
		java_lang_String::create_from_str("Incinerator", thread),
		thread);

	MutexLocker mu(Threads_lock, thread);

	java_lang_Thread::set_thread(thread_obj(), this);

	//java_lang_Thread::set_priority(thread_obj(), NormPriority);
	//os::set_native_priority(this, os::java_to_os_priority[NormPriority]);

	java_lang_Thread::set_daemon(thread_obj());

	this->set_threadObj(thread_obj());
	Threads::add(this);
	return true;
}

bool IncineratorThread::start()
{
	if ((_state & RunningState) != 0) return false;

	atomic_flag_change(_state, StopPendingState, false);

	sem_t started_on_stack;
	_started = &started_on_stack;
	if (sem_init(_started, 0, 0) == -1)
		return false;

	Thread::start(this);
	os::yield();

	sem_wait(_started);

	sem_destroy(_started);
	_started = NULL;

	if ((_state & PrologueSucceeded) != 0) {
		atomic_flag_change(_state, PrologueSucceeded, false);
		return true;
	} else {
		return false;
	}
}

void IncineratorThread::stop()
{
	if ((_state & RunningState) == 0)
		return;

	if ((_state & StopPendingState) == 0)
		atomic_flag_change(_state, StopPendingState, true);

	do {
		os::yield();
	} while ((_state & StopPendingState) != 0);
}

void IncineratorThread::atomic_flag_change(
	volatile uint32_t& ptr, uint32_t mask, bool enabled)
{
	uint32_t old_flags, new_flags;

	do {
		old_flags = ptr;
		new_flags = enabled ? (old_flags | mask) : (old_flags & ~mask);
	} while (__sync_bool_compare_and_swap(&ptr, old_flags, new_flags) == false);
}

void IncineratorThread::entry_point(JavaThread* thread, TRAPS)
{
	static_cast<IncineratorThread*>(thread)->entry_point();
}

void IncineratorThread::entry_point()
{
	if ((_state & StopPendingState) != 0) {
		atomic_flag_change(_state, StopPendingState, false);
		sem_post(_started);
		return;
	}

	atomic_flag_change(_state, RunningState, true);

	ResourceMark rm(this);
	HandleMark hm(this);

	if (_target.prologue() == false) {
		atomic_flag_change(_state, RunningState, false);
		sem_post(_started);
		return;
	}

	atomic_flag_change(_state, PrologueSucceeded, true);
	sem_post(_started);

	if ((_state & StopPendingState) == 0)
		_target.run();

	_target.epilogue();

	if ((_state & StopPendingState) != 0)
		atomic_flag_change(_state, StopPendingState, false);

	atomic_flag_change(_state, RunningState, false);
}

}
