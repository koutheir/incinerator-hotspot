
#ifndef INCINERATORTHREAD_HPP_
#define INCINERATORTHREAD_HPP_

#include "precompiled.hpp"
#include <pthread.h>
#include <semaphore.h>

namespace Incinerator {

class ThreadRunnable
{
public:
	virtual ~ThreadRunnable() {}

	virtual bool prologue() {return true;}
	virtual void run() = 0;
	virtual void epilogue() {}
};


class IncineratorThread :
	public JavaThread
{
public:
	IncineratorThread(ThreadRunnable& target);
	virtual ~IncineratorThread() {}

	bool initialize();

	virtual bool start();
	virtual void stop();

	bool is_running() const				{return (_state & RunningState) != 0;}
	bool should_stop_running() const	{return (_state & StopPendingState) != 0;}

protected:
	static inline void atomic_flag_change(
		volatile uint32_t& ptr, uint32_t mask, bool enabled);

	static void entry_point(JavaThread* thread, TRAPS);
	inline void entry_point();

private:
	enum State {
		PrologueSucceeded	= 0x1,
		StopPendingState	= 0x2,
		RunningState		= 0x4,
	};

	sem_t				*_started;
	volatile uint32_t	_state;
	ThreadRunnable&		_target;
};

}

#endif /* INCINERATORTHREAD_HPP_ */
