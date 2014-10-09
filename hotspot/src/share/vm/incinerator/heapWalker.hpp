
#ifndef HEAPWALKER_HPP_
#define HEAPWALKER_HPP_

#include "precompiled.hpp"
#include <set>

namespace Incinerator {

class ObjectMark :
	public CHeapObj<mtInternal>
{
public:
	oop _object;
	markOop _mark;
	ObjectMark(oop object = NULL, markOop mark = NULL) :
		_object(object), _mark(mark) {}
	void restore() {_object->set_mark(_mark);}
};


class ResetObjectMarkClosure :
	public ObjectClosure
{
public:
	void do_object(oop o);
};


class HeapWalkerListener
{
public:
	virtual void heap_walking_prologue() {}

	virtual void heap_object_found(oop obj, const oop referent) = 0;
	virtual void heap_reference_found(oop* ref, const oop referent) = 0;
	virtual void heap_narrow_reference_found(narrowOop* ref, const oop referent) = 0;

	virtual void heap_walking_epilogue() {}
};

class HeapWalkerElementFoundClosure :
	public ExtendedOopClosure,
	public ObjectClosure,
	public CodeBlobClosure,
	public CLDToOopClosure
{
public:
	HeapWalkerElementFoundClosure(HeapWalkerListener& listener);

	~HeapWalkerElementFoundClosure()					{delete _referent;}

	virtual bool accepts_oop_from(OopSourceKind kind);

	virtual bool idempotent()							{return true;}
	virtual bool apply_to_weak_ref_discovered_field()	{return true;}

	virtual bool do_metadata()							{return true;}
	bool do_metadata_v()								{return true;}
	bool do_metadata_nv()								{return true;}

	virtual void do_klass(Klass* k)
		{_listener.heap_object_found(k->java_mirror(), referent());}
	void do_klass_v(Klass* k)
		{_listener.heap_object_found(k->java_mirror(), referent());}
	void do_klass_nv(Klass* k)
		{_listener.heap_object_found(k->java_mirror(), referent());}

	virtual void do_class_loader_data(ClassLoaderData* cld)
		{_listener.heap_object_found(cld->class_loader(), referent());}

	virtual void do_oop(oop* ref)
		{_listener.heap_reference_found(ref, referent());}

	virtual void do_oop(narrowOop* ref)
		{_listener.heap_narrow_reference_found(ref, referent());}

	virtual void do_object(oop obj)
		{_listener.heap_object_found(obj, referent());}

	virtual void do_code_blob(CodeBlob* cb);

	void push_referent(const oop referent)	{_referent->push(referent);}
	const oop pop_referent()				{return _referent->pop();}
	const oop referent() const				{_referent->top();}

protected:
	HeapWalkerListener&	_listener;
	GrowableArray<oop>	*_referent;
};


class HeapWalkerElementFoundClosureReferentPreserver
{
public:
	HeapWalkerElementFoundClosureReferentPreserver(
		HeapWalkerElementFoundClosure& closure, oop referent) : _closure(closure)
		{closure.push_referent(referent);}

	~HeapWalkerElementFoundClosureReferentPreserver() {_closure.pop_referent();}

protected:
	HeapWalkerElementFoundClosure&	_closure;
};


class HeapWalker :
	public CHeapObj<mtInternal>,
	public HeapWalkerListener
{
	friend class EnqueueObjectClosure;
	friend class HeapWalkOperation;

public:
	HeapWalker();
	virtual ~HeapWalker();

	void operator()(HeapWalkerListener& listener, oop first_obj = NULL);

	bool explore_prologue(HeapWalkerListener& listener, oop first_obj = NULL);
	void explore(HeapWalkerListener& listener, oop first_obj = NULL);
	void explore_epilogue(HeapWalkerListener& listener, oop first_obj = NULL);

	virtual void heap_object_found(oop obj, const oop referent);
	virtual void heap_reference_found(oop* ref, const oop referent);
	virtual void heap_narrow_reference_found(narrowOop* ref, const oop referent);

	void set_listener(HeapWalkerListener* listener) {_listener = listener;}

	bool walk_object_later(oop obj);

	void mark_object_visited(oop obj);
	static bool is_object_visited(const oop obj)	{return obj->mark()->is_marked();}

	void backup_objects_marks();
	void restore_objects_marks();

protected:
	static void scan_simple_roots(HeapWalkerElementFoundClosure& closure);
	static void scan_threads_roots(HeapWalkerElementFoundClosure& closure);
	static void scan_thread_roots(HeapWalkerElementFoundClosure& closure, Thread& thread);
	static void scan_object_roots(HeapWalkerElementFoundClosure& closure, oop obj);

	GrowableArray<ObjectMark>	*_savedObjectMarks;
	GrowableArray<oop>*			_objectsToVisit;
	HeapWalkerListener*			_listener;
};


class HeapWalkOperation :
	public VM_Operation
{
public:
	HeapWalkOperation(HeapWalker& heap_walker) : _heap_walker(heap_walker) {}
	virtual ~HeapWalkOperation() {}

	virtual VMOp_Type type() const			{return VMOp_HeapWalkOperation;}
	virtual Mode evaluation_mode() const	{return _safepoint;}

	virtual bool doit_prologue()
		{return _heap_walker.explore_prologue(*_heap_walker._listener);}
	virtual void doit()
		{_heap_walker.explore(*_heap_walker._listener);}
	virtual void doit_epilogue()
		{_heap_walker.explore_epilogue(*_heap_walker._listener);}

protected:
	HeapWalker&	_heap_walker;
};

}

#endif /* HEAPWALKER_HPP_ */
