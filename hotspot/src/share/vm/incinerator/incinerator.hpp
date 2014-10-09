
#ifndef SHARE_VM_INCINERATOR_INCINERATOR_HPP
#define SHARE_VM_INCINERATOR_INCINERATOR_HPP

#include "precompiled.hpp"
#include <stdint.h>
#include <semaphore.h>
#include <set>
#include <map>
#include <list>

#define DEFER_STALE_REF_FOR_FINALIZATION	1

#include "incinerator/incineratorThread.hpp"
#include "incinerator/heapWalker.hpp"

namespace Incinerator {

template<class T>
class VMHeapAllocator :
	public CHeapObj<mtInternal>
{
public:
	typedef size_t     size_type;
	typedef ptrdiff_t  difference_type;
	typedef T*         pointer;
	typedef const T*   const_pointer;
	typedef T&         reference;
	typedef const T&   const_reference;
	typedef T          value_type;

	template<typename T2>
	struct rebind { typedef VMHeapAllocator<T2> other; };

	VMHeapAllocator() throw() {}
	VMHeapAllocator(const VMHeapAllocator& alloc) throw() {}
	template <class U>
	VMHeapAllocator(const VMHeapAllocator<U>& alloc) throw() {}
	virtual ~VMHeapAllocator() throw() {}

	pointer address(reference x) const {return &x;}
	const_pointer address(const_reference x) const {return &x;}
	size_type max_size() const throw() {return (size_type)(-1) / sizeof(value_type);}

	void construct(pointer p, const_reference val) {new ((unsigned char*)p) value_type(val);}
	void destroy (pointer p) {p->~value_type();}

	pointer allocate (size_type n, const_pointer hint=0)
		{return reinterpret_cast<pointer>(AllocateHeap(n * sizeof(value_type), mtInternal));}
	void deallocate (pointer p, size_type n) {FreeHeap(p, mtInternal);}
};


template<typename T>
class StaleReferenceT
{
public:
	T*			_reference;
	const oop	_referent;

	StaleReferenceT(T* reference, const oop referent) :
		_reference(reference), _referent(referent) {}

	oop obj() const
		{return oopDesc::decode_heap_oop_not_null(*_reference);}

	void reset()
		{reset(_reference, _referent);}

	static void reset(T* reference, const oop referent);

	bool operator < (const StaleReferenceT<T>& obj) const
		{return (_reference < obj._reference);}

	static void print_value_on(outputStream* st, const T obj, const oop referent);
	static void print_value_on(outputStream* st, const T* reference, const oop referent);
	static void print_on(outputStream* st, const T obj, const oop referent);
	static void print_on(outputStream* st, const T* reference, const oop referent);
	void print_value_on(outputStream* st) const
		{print_value_on(st, _reference, _referent);}
	void print_on(outputStream* st) const
		{print_on(st, _reference, _referent);}
	void print_value() const
		{print_value_on(tty, _reference, _referent);}
	void print() const
		{print_on(tty, _reference, _referent);}
};

typedef StaleReferenceT<oop>		StaleReference;
typedef StaleReferenceT<narrowOop>	NarrowStaleReference;

typedef std::set<const ClassLoaderData*,
	std::less<const ClassLoaderData*>,
	VMHeapAllocator<const ClassLoaderData*> >
	set_class_loader_t;

typedef std::set<StaleReference, std::less<StaleReference>,
	VMHeapAllocator<StaleReference> >
	set_reference_t;

typedef std::set<NarrowStaleReference, std::less<NarrowStaleReference>,
	VMHeapAllocator<NarrowStaleReference> >
	set_narrow_reference_t;

typedef std::set<oop, std::less<oop>, VMHeapAllocator<oop> > set_oop_t;

typedef std::map<oop, bool, std::less<oop>,
	VMHeapAllocator<std::pair<const oop, bool> > >
	map_oop_to_bool_t;

class Incinerator;

#if DEFER_STALE_REF_FOR_FINALIZATION

class ExcludeStaleReferences :
	public HeapWalkerListener
{
public:
	ExcludeStaleReferences(Incinerator& incinerator) :
		_incinerator(incinerator) {}

	virtual void heap_walking_prologue() {}
	virtual void heap_object_found(oop obj, oop referent);
	virtual void heap_reference_found(oop* ref, oop referent);
	virtual void heap_narrow_reference_found(narrowOop* ref, oop referent);
	virtual void heap_walking_epilogue() {}

protected:
	Incinerator&	_incinerator;
};

#endif

class Incinerator :
	public CHeapObj<mtInternal>,
	public ThreadRunnable,
	public HeapWalkerListener
{
protected:
	Incinerator();
	virtual ~Incinerator();

public:
	static Incinerator* get(bool optionally_create = true);

	bool initialize(JNIEnv* env);

	void trigger();

	bool mark_class_loader_stale(const ClassLoaderData* class_loader);
	void class_loader_unloading(const ClassLoaderData* class_loader);

	bool is_object_stale(const oop obj) const;
	bool is_object_stale_no_lock(const oop obj) const;
	bool stale_object_should_be_skipped(oop obj, const oop referent) const;

	void set_needs_another_execution(bool v)	{_needs_another_execution = v;}

	virtual void heap_walking_prologue();
	virtual void heap_object_found(oop obj, const oop referent);
	virtual void heap_reference_found(oop* ref, const oop referent);
	virtual void heap_narrow_reference_found(narrowOop* ref, const oop referent);
	virtual void heap_walking_epilogue();

	static oop callObjectToString(const oop obj, Thread* thread);

protected:
	virtual void run();

	void reset_stale_references();

	template<class T>
	static void reset_stale_reference(const StaleReferenceT<T>& ref)
		{const_cast<StaleReferenceT<T>&>(ref).reset();}

	static Incinerator				*_singleton;
	static const JNINativeMethod	_native_methods[];

	IncineratorThread&		_thread;
	HeapWalker				_heap_walker;
	HeapWalkOperation		_heap_walk_op;
	set_class_loader_t		_incoming_stale_class_loaders, _stale_class_loaders;
	mutable Mutex			_incoming_stale_class_loaders_lock, _stale_class_loaders_lock;
	volatile bool			_run_incinerator;
	bool					_needs_another_execution;

#if DEFER_STALE_REF_FOR_FINALIZATION

public:
	void exclude_stale_object(oop obj);

protected:
	void record_finalizable_object(oop obj, oop referent);
	void exclude_stale_references_reachable_from_unreachable_finalizable_objects();

protected:
	set_reference_t			_stale_references;
	set_narrow_reference_t	_stale_references_narrow;
	map_oop_to_bool_t		_finalizable_reachable_objects;
	ExcludeStaleReferences	_exclude_stale_references;

#endif
};

}


extern "C" void Incinerator_bootstrap(JNIEnv* env);
extern "C" void Incinerator_classLoaderUnloading(const ClassLoaderData* class_loader);
extern "C" bool Incinerator_isStaleObject(oop obj);

#endif
