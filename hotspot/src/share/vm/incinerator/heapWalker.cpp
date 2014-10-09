
#include "precompiled.hpp"
#include "incinerator/heapWalker.hpp"
#include "services/serviceUtil.hpp"
#include "incinerator/incinerator.hpp"

namespace Incinerator {

void ResetObjectMarkClosure::do_object(oop obj)
{
	if (!obj) return;

	markOop mark = obj->mark();
	if (mark->is_marked() == false) return;

	obj->init_mark();
}

HeapWalkerElementFoundClosure::HeapWalkerElementFoundClosure(
	HeapWalkerListener& listener) :
	CLDToOopClosure(this, true), _listener(listener)
{
	_referent =
		new (ResourceObj::C_HEAP, mtInternal) GrowableArray<oop>(16, true);
	push_referent(NULL);
}

void HeapWalkerElementFoundClosure::do_code_blob(CodeBlob* cb)
{
	nmethod* nm = cb->as_nmethod_or_null();
	if (!nm) return;
	nm->oops_do(this);
}

bool HeapWalkerElementFoundClosure::accepts_oop_from(OopSourceKind kind)
{
	switch (kind) {
	case OopClosure::OopFrom_BasicObjectLock:
	case OopClosure::OopFrom_Thread_GCHandles:
	case OopClosure::OopFrom_JNIHandles:
	case OopClosure::OopFrom_InflatedMonitors:
		return false;
	}
	return true;
}

HeapWalker::HeapWalker() :
	_listener(NULL), _savedObjectMarks(NULL)
{
	_objectsToVisit = new (ResourceObj::C_HEAP, mtInternal)
		GrowableArray<oop>(1024, true);
}

HeapWalker::~HeapWalker()
{
	delete _objectsToVisit;
	delete _savedObjectMarks;
}

void HeapWalker::operator()(HeapWalkerListener& listener, oop first_obj)
{
	if (explore_prologue(listener, first_obj) == false) return;
	explore(listener, first_obj);
	explore_epilogue(listener, first_obj);
}

bool HeapWalker::explore_prologue(HeapWalkerListener& listener, oop first_obj)
{
	return true;
}

void HeapWalker::explore(HeapWalkerListener& listener, oop first_obj)
{
	Thread* thread = Thread::current();
	HandleMark hm(thread);
	ResourceMark rm(thread);

	_listener = &listener;
	_listener->heap_walking_prologue();

	backup_objects_marks();

	HeapWalkerElementFoundClosure closure(*this);

	if (first_obj == NULL) {
		scan_simple_roots(closure);
		scan_threads_roots(closure);
	} else {
		scan_object_roots(closure, first_obj);
	}

	while (_objectsToVisit->is_empty() == false) {
		oop obj = _objectsToVisit->pop();
		if (is_object_visited(obj))
			continue;

		mark_object_visited(obj);

		HeapWalkerElementFoundClosureReferentPreserver guard(closure, obj);
		obj->klass()->oop_oop_iterate(obj, &closure);
	}

	restore_objects_marks();

	_listener->heap_walking_epilogue();
	_listener = NULL;
}

void HeapWalker::explore_epilogue(HeapWalkerListener& listener, oop first_obj)
{
	_objectsToVisit->clear();
	_savedObjectMarks->clear();
}

void HeapWalker::scan_simple_roots(HeapWalkerElementFoundClosure& closure)
{
	HeapWalkerElementFoundClosureReferentPreserver guard(closure, NULL);

	// JNI globals
	if (closure.accepts_oop_from(OopClosure::OopFrom_JNIHandles)) {
		JNIHandles::oops_do(&closure);
	}

	// Preloaded classes and loader from the system dictionary
	SystemDictionary::always_strong_oops_do(&closure);
	KlassToOopClosure klass_blk(&closure);
	ClassLoaderDataGraph::always_strong_oops_do(&closure, &klass_blk, false);

	// Inflated monitors
	if (closure.accepts_oop_from(OopClosure::OopFrom_InflatedMonitors)) {
		ObjectSynchronizer::oops_do(&closure);
	}

	// Other kinds of roots maintained by HotSpot
	// Many of these won't be visible but others (such as instances of important
	// exceptions) will be visible.
	Universe::oops_do(&closure);

	// If there are any non-perm roots in the code cache, visit them.
	CodeBlobToOopClosure look_in_blobs(&closure, false);
	CodeCache::scavenge_root_nmethods_do(&look_in_blobs);
}

void HeapWalker::scan_threads_roots(HeapWalkerElementFoundClosure& closure)
{
	for (JavaThread* thread = Threads::first();
		thread != NULL;
		thread = thread->next())
	{
		if (thread->is_exiting() || thread->is_hidden_from_external_view())
			continue;

		HeapWalkerElementFoundClosureReferentPreserver
			guard(closure, thread->threadObj());

		scan_thread_roots(closure, *thread);
	}
}

void HeapWalker::scan_thread_roots(HeapWalkerElementFoundClosure& closure, Thread& thread)
{
	thread.oops_do(&closure, &closure, &closure);
}

void HeapWalker::scan_object_roots(HeapWalkerElementFoundClosure& closure, oop obj)
{
	closure.do_object(obj);
}

void HeapWalker::backup_objects_marks()
{
	if (_savedObjectMarks == NULL) {
		_savedObjectMarks = new (ResourceObj::C_HEAP, mtInternal)
			GrowableArray<ObjectMark>(64, true);
	}

	// Prepare the heap for iteration
	Universe::heap()->ensure_parsability(false /* no need to retire TLABs */);

	if (UseBiasedLocking)
		BiasedLocking::preserve_marks();
}

void HeapWalker::restore_objects_marks()
{
	// Restore mark bits of all objects to their initial value
	ResetObjectMarkClosure resetObjectMarkClosure;
	Universe::heap()->object_iterate(&resetObjectMarkClosure);

	for (int i=0, count=_savedObjectMarks->length(); i < count; ++i)
		_savedObjectMarks->at(i).restore();

	if (UseBiasedLocking)
		BiasedLocking::restore_marks();
}

void HeapWalker::mark_object_visited(oop obj)
{
	markOop mark = obj->mark();

	if (mark->must_be_preserved(obj))
		_savedObjectMarks->push(ObjectMark(obj, mark));

	obj->set_mark(markOopDesc::prototype()->set_marked());
}

void HeapWalker::heap_object_found(oop obj, const oop referent)
{
	if (oopDesc::is_null(obj) || (obj == JNIHandles::deleted_handle())) return;
	obj = oopDesc::decode_heap_oop_not_null(obj);

	walk_object_later(obj);

	_listener->heap_object_found(obj, referent);
}

void HeapWalker::heap_reference_found(oop* ref, const oop referent)
{
	if (!ref || oopDesc::is_null(*ref) || (*ref == JNIHandles::deleted_handle())) return;

	walk_object_later(oopDesc::decode_heap_oop_not_null(*ref));

	_listener->heap_reference_found(ref, referent);
}

void HeapWalker::heap_narrow_reference_found(narrowOop* ref, const oop referent)
{
	if (!ref || oopDesc::is_null(*ref)) return;

	oop obj = oopDesc::decode_heap_oop_not_null(*ref);
	if (obj == JNIHandles::deleted_handle()) return;

	walk_object_later(obj);

	_listener->heap_narrow_reference_found(ref, referent);
}

bool HeapWalker::walk_object_later(oop obj)
{
//	if (!ServiceUtil::visible_oop(obj)) return false;

	if (oopDesc::is_null(obj) ||
		(obj == JNIHandles::deleted_handle()) ||
		is_object_visited(obj))
		return false;

	_objectsToVisit->push(obj);
	return true;
}

}
