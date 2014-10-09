
#include "precompiled.hpp"
#include "incinerator/incinerator.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include <algorithm>


extern "C" void Incinerator_bootstrap(JNIEnv* env)
{
	Incinerator::Incinerator* incinerator = Incinerator::Incinerator::get();
	incinerator->initialize(env);
}

extern "C" void Incinerator_classLoaderUnloading(
	const ClassLoaderData* class_loader)
{
	Incinerator::Incinerator* incinerator = Incinerator::Incinerator::get();
	incinerator->class_loader_unloading(class_loader);
}

extern "C" jboolean Java_Incinerator_Core_markClassLoaderStale(
	JNIEnv *env, jclass klass, jobject class_loader)
{
	if (class_loader == NULL) return JNI_FALSE;

	JavaThread* thread = JavaThread::thread_from_jni_environment(env);
	ThreadInVMfromNative tiv(thread);
	HandleMark hm(thread);
	ResourceMark rm(thread);

	oop ldr_obj = JNIHandles::resolve(class_loader);

	if (java_lang_ClassLoader::is_instance(ldr_obj) == false)
		return JNI_FALSE;

	const ClassLoaderData* ldr_data =
		ClassLoaderDataGraph::find_or_create(ldr_obj, thread);

	Incinerator::Incinerator* incinerator = Incinerator::Incinerator::get();
	return incinerator->mark_class_loader_stale(ldr_data) ? JNI_TRUE : JNI_FALSE;
}

extern "C" void Java_Incinerator_Core_run(JNIEnv *env, jclass klass)
{
	JavaThread* thread = JavaThread::thread_from_jni_environment(env);
	ThreadInVMfromNative tiv(thread);

	Incinerator::Incinerator* incinerator = Incinerator::Incinerator::get();
	incinerator->trigger();
}

extern "C" bool Incinerator_isStaleObject(oop obj)
{
	Incinerator::Incinerator* incinerator = Incinerator::Incinerator::get(false);
	if (!incinerator)
		return false;

	return incinerator->is_object_stale(obj);
}

/*
extern "C" void Java_Incinerator_Core_notifyObjectFinalized(JNIEnv *env, jclass klass, jobject jobj)
{
	if (jobj == NULL) return;
	JavaThread* thread = JavaThread::thread_from_jni_environment(env);
	ThreadInVMfromNative tiv(thread);
	HandleMark hm(thread);
	ResourceMark rm(thread);
	oop obj = JNIHandles::resolve(jobj);

	Incinerator::Incinerator::get().notify_object_finalized(obj);
}
*/

namespace Incinerator {

Incinerator* Incinerator::_singleton = NULL;

#define FN_PTR(f) CAST_FROM_FN_PTR(void*, &f)

const JNINativeMethod Incinerator::_native_methods[] = {
	{(char*)"markClassLoaderStale", (char*)"(Ljava/lang/Object;)Z",
		FN_PTR(Java_Incinerator_Core_markClassLoaderStale)},
	{(char*)"run", (char*)"()V",
		FN_PTR(Java_Incinerator_Core_run)},
};

#undef FN_PTR

Incinerator::Incinerator() :
	_thread(*new IncineratorThread(*this)),
	_heap_walk_op(_heap_walker),
	_incoming_stale_class_loaders_lock(
		Mutex::native, "Incinerator::_incoming_stale_class_loaders_lock", true),
	_stale_class_loaders_lock(
		Mutex::native, "Incinerator::_stale_class_loaders_lock", true),
	_run_incinerator(false),
	_needs_another_execution(false)

#if DEFER_STALE_REF_FOR_FINALIZATION
	, _exclude_stale_references(*this)
#endif

{
}

Incinerator::~Incinerator()
{
	_thread.stop();
}

bool Incinerator::initialize(JNIEnv* env)
{
	// Do NOT initialize Incinerator if "Incinerator.Core" is not loadable
	jclass incinerator_core = env->FindClass("Incinerator/Core");
	if (!incinerator_core)
		return false;

	// Give access to Incinerator from the Java world
	size_t native_methods_count =
		sizeof(_native_methods)/sizeof(*_native_methods);
	env->RegisterNatives(
		incinerator_core, _native_methods, native_methods_count);

	return _thread.start();
}

Incinerator* Incinerator::get(bool optionally_create)
{
	if ((!_singleton) && optionally_create) {
		_singleton = new Incinerator();
		assert(_singleton != NULL, "Incinerator allocation failed");
	}
	return _singleton;
}

void Incinerator::run()
{
	for (;;) {
		{
			ObjectLocker thread_obj_locker(_thread.threadObj(), &_thread);
			while (!_run_incinerator)
				thread_obj_locker.wait(&_thread);
		}

		usleep(300000);

		if (_needs_another_execution) {
			// Wait for a moment so finalizers are executed, before running
			// Incinerator again.

			//sleep(1);
		}

		_heap_walker.set_listener(this);
		VMThread::execute(&_heap_walk_op);

		_run_incinerator = false;
	}
}

void Incinerator::trigger()
{
	if (_run_incinerator) return;

	JavaThread* thread = JavaThread::current();
	ObjectLocker thread_obj_locker(_thread.threadObj(), thread);

	_run_incinerator = true;
	thread_obj_locker.notify_all(thread);
}

void Incinerator::heap_walking_prologue()
{
	tty->print_cr("[INCINERATOR] Scanning...");

	if (_incoming_stale_class_loaders.size() > 0) {
		MutexLocker ml1(&_incoming_stale_class_loaders_lock);
		MutexLocker ml2(&_stale_class_loaders_lock);

		_stale_class_loaders.insert(
			_incoming_stale_class_loaders.begin(),
			_incoming_stale_class_loaders.end());

		_incoming_stale_class_loaders.clear();
	}

	_needs_another_execution = false;
}

void Incinerator::heap_object_found(oop obj, oop referent)
{
#if DEFER_STALE_REF_FOR_FINALIZATION

	if (obj->klass()->has_finalizer())		// Object is finalizable
		record_finalizable_object(obj, referent);

#endif

	if ((is_object_stale_no_lock(obj) == false) ||
		stale_object_should_be_skipped(obj, referent))
		return;

	tty->print("[INCINERATOR] Stale object found:");
	StaleReference(&obj, referent).print_value();
	tty->cr();
}

void Incinerator::heap_reference_found(oop* ref, oop referent)
{
#if DEFER_STALE_REF_FOR_FINALIZATION

	if ((**ref).klass()->has_finalizer())	// Object is finalizable
		record_finalizable_object(*ref, referent);

#endif

	if ((is_object_stale_no_lock(*ref) == false) ||
		stale_object_should_be_skipped(*ref, referent))
		return;

#if DEFER_STALE_REF_FOR_FINALIZATION

	_stale_references.insert(StaleReference(ref, referent));

#else

	StaleReference::reset(ref, referent);

#endif
}

void Incinerator::heap_narrow_reference_found(narrowOop* ref, oop referent)
{
	oop obj = oopDesc::decode_heap_oop_not_null(*ref);

#if DEFER_STALE_REF_FOR_FINALIZATION

	if (obj->klass()->has_finalizer())		// Object is finalizable
		record_finalizable_object(obj, referent);

#endif

	if ((is_object_stale_no_lock(obj) == false) ||
		stale_object_should_be_skipped(obj, referent))
		return;

#if DEFER_STALE_REF_FOR_FINALIZATION

	_stale_references_narrow.insert(NarrowStaleReference(ref, referent));

#else

	NarrowStaleReference::reset(ref, referent);

#endif
}

void Incinerator::heap_walking_epilogue()
{
#if DEFER_STALE_REF_FOR_FINALIZATION

	exclude_stale_references_reachable_from_unreachable_finalizable_objects();

#endif

	reset_stale_references();

	ObjectSynchronizer::deflate_idle_monitors();

	tty->print_cr("[INCINERATOR] Done.");
}

void Incinerator::reset_stale_references()
{
#if DEFER_STALE_REF_FOR_FINALIZATION

	std::for_each(
		_stale_references.begin(), _stale_references.end(),
		Incinerator::reset_stale_reference<oop>);

	std::for_each(
		_stale_references_narrow.begin(), _stale_references_narrow.end(),
		Incinerator::reset_stale_reference<narrowOop>);

	_stale_references.clear();
	_stale_references_narrow.clear();

#else

	// Nothing to do here.
	// Stale references are eliminated during the graph exploration.

#endif
}

bool Incinerator::stale_object_should_be_skipped(oop obj, oop referent) const
{
	if (!referent) return false;
	const Klass* referent_class = referent->klass();

	if (referent_class == SystemDictionary::WeakReference_klass()) {
		// Keep weak references, the GC will handle them
		tty->print("[INCINERATOR] Weak stale ref skipped: ");
		StaleReference::print_value_on(tty, obj, referent);
		tty->cr();
		return true;
	} else if ((referent_class == SystemDictionary::Finalizer_klass()) &&
		obj->klass()->has_finalizer())
	{
		// Keep the references of the Finalizer
		tty->print("[INCINERATOR] Stale ref skipped for finalization: ");
		StaleReference::print_value_on(tty, obj, referent);
		tty->cr();
		return true;
	}

	return false;
}

bool Incinerator::mark_class_loader_stale(const ClassLoaderData* class_loader)
{
	if (!class_loader) return false;

	tty->print("[INCINERATOR] Class loader marked stale: 0x%x", class_loader->class_loader());
	tty->cr();
	class_loader->class_loader()->print();

	MutexLocker ml(&_incoming_stale_class_loaders_lock);
	_incoming_stale_class_loaders.insert(class_loader);

//	trigger();
	return true;
}

void Incinerator::class_loader_unloading(const ClassLoaderData* class_loader)
{
	MutexLocker ml1(&_incoming_stale_class_loaders_lock);
	MutexLocker ml2(&_stale_class_loaders_lock);

	if ((_stale_class_loaders.find(class_loader) != _stale_class_loaders.end()) ||
		(_incoming_stale_class_loaders.find(class_loader) != _incoming_stale_class_loaders.end()))
	{
		tty->print("[INCINERATOR] Stale class loader unloading: 0x%x", class_loader->class_loader());
		tty->cr();
		class_loader->class_loader()->print();
	}

	_stale_class_loaders.erase(class_loader);
	_incoming_stale_class_loaders.erase(class_loader);
}

bool Incinerator::is_object_stale_no_lock(const oop obj) const
{
	assert(SafepointSynchronize::is_at_safepoint(), "Must be called at a safe point.");

	if (oopDesc::is_null(obj) || (_stale_class_loaders.size() == 0))
		return false;	// No stale objects at all

	const Klass* klass = obj->klass();
	const ClassLoaderData* cld = klass->class_loader_data();

	// _incoming_stale_class_loaders is not checked now

	return (_stale_class_loaders.find(cld) != _stale_class_loaders.end());
}

bool Incinerator::is_object_stale(const oop obj) const
{
	if (oopDesc::is_null(obj) || (_stale_class_loaders.size() == 0))
		return false;	// No stale objects at all

	const Klass* klass = obj->klass();
	const ClassLoaderData* cld = klass->class_loader_data();

	// _incoming_stale_class_loaders is not checked now

	MutexLocker ml(&_stale_class_loaders_lock);
	return (_stale_class_loaders.find(cld) != _stale_class_loaders.end());
}

oop Incinerator::callObjectToString(oop obj, Thread* thread)
{
	static Symbol* meth_sym = NULL, *sign_sym = NULL;

	if (!meth_sym) {
		static const char
			*meth_name = "toString", *sign_name = "()Ljava/lang/String;";

		meth_sym = SymbolTable::lookup(meth_name, strlen(meth_name), thread),
		sign_sym = SymbolTable::lookup(sign_name, strlen(sign_name), thread);
	}

	JavaValue str_value(T_OBJECT);
	JavaCalls::call_virtual(&str_value, obj, obj->klass(), meth_sym, sign_sym, thread);
	return (oop)str_value.get_jobject();
}

#if DEFER_STALE_REF_FOR_FINALIZATION

void Incinerator::record_finalizable_object(oop obj, oop referent)
{
	map_oop_to_bool_t::iterator i =
		_finalizable_reachable_objects.find(obj);

	if (i == _finalizable_reachable_objects.end()) {
		// Finalizable object's reachability registered for the first time
		bool referent_is_not_finalizer =
			(referent != NULL) ?
				(referent->klass() != SystemDictionary::Finalizer_klass()) :
				true;

		_finalizable_reachable_objects[obj] = referent_is_not_finalizer;
	} else {
		if (i->second == false) {	// Finalizable object was unreachable
			bool referent_is_not_finalizer =
				(referent != NULL) ?
					(referent->klass() != SystemDictionary::Finalizer_klass()) :
					true;

			i->second = referent_is_not_finalizer;
		}
	}
}

void Incinerator::
	exclude_stale_references_reachable_from_unreachable_finalizable_objects()
{
	for (map_oop_to_bool_t::iterator
		i = _finalizable_reachable_objects.begin(),
		e = _finalizable_reachable_objects.end(); i != e; ++i)
	{
		if (i->second) {
			// Finalizable reachable object.
			// Reset stale references reachable from here.
			continue;
		}

		// Finalizable unreachable object.
		// Exclude stale references reachable from here.
		tty->print("[INCINERATOR] Excluding stale ref reachable from the unreachable finalizable object: ");
		i->first->print_value_on(tty);
		tty->cr();

		_heap_walker(_exclude_stale_references, i->first);
	}
	_finalizable_reachable_objects.clear();
}

void Incinerator::exclude_stale_object(oop obj)
{
	for (set_reference_t::iterator
		i = _stale_references.begin(), e = _stale_references.end(); i != e; )
	{
		if (i->obj() == obj) {
			tty->print("[INCINERATOR] Stale reference excluded: ");
			i->print_value_on(tty);
			tty->cr();

			_stale_references.erase(i);
			i = _stale_references.begin();
		} else {
			++i;
		}
	}

	for (set_narrow_reference_t::iterator
		i = _stale_references_narrow.begin(),
		e = _stale_references_narrow.end(); i != e; )
	{
		if (i->obj() == obj) {
			tty->print("[INCINERATOR] Stale reference excluded: ");
			i->print_value_on(tty);
			tty->cr();

			_stale_references_narrow.erase(i);
			i = _stale_references_narrow.begin();
		} else {
			++i;
		}
	}
}

void ExcludeStaleReferences::heap_object_found(oop obj, oop referent)
{
	if ((_incinerator.is_object_stale_no_lock(obj) == false) ||
		_incinerator.stale_object_should_be_skipped(obj, referent))
		return;

	_incinerator.exclude_stale_object(obj);
}

void ExcludeStaleReferences::heap_reference_found(oop* ref, oop referent)
{
	if ((_incinerator.is_object_stale_no_lock(*ref) == false) ||
		_incinerator.stale_object_should_be_skipped(*ref, referent))
		return;

	_incinerator.exclude_stale_object(*ref);
}

void ExcludeStaleReferences::heap_narrow_reference_found(
	narrowOop* ref, oop referent)
{
	oop obj = oopDesc::decode_heap_oop_not_null(*ref);

	if ((_incinerator.is_object_stale_no_lock(obj) == false) ||
		_incinerator.stale_object_should_be_skipped(obj, referent))
		return;

	_incinerator.exclude_stale_object(obj);
}

#endif

template<typename T>
void StaleReferenceT<T>::reset(T* reference, const oop referent)
{
	Handle h_obj(oopDesc::decode_heap_oop_not_null(*reference));

	ObjectMonitor* monitor = NULL;
	JavaThread* thread = ObjectSynchronizer::get_lock_owner(h_obj, false, &monitor);

	if (!monitor) {
		tty->print("[INCINERATOR] Stale ref eliminated: ");
		print_value_on(tty, reference, referent);
		tty->cr();

		oopDesc::encode_store_heap_oop(reference, NULL);
	} else {
		tty->print("[INCINERATOR] Stale ref used for synchronization: ");
		print_value_on(tty, reference, referent);
		tty->cr();

		int recursions = 0;
		if (thread != NULL) {
			monitor->notifyAll(thread);
			recursions = monitor->complete_exit(thread);
		}

		intptr_t monitor_count = monitor->count();
//		if (monitor_count > 0) {
//			monitor->set_count(--monitor_count);
//		}

//		if (monitor_count == 0) {
//			ObjectMonitor *FreeHead = NULL, *FreeTail = NULL;
//			ObjectSynchronizer::deflate_monitor(monitor, h_obj(), &FreeHead, &FreeTail);
//
//			ObjectSynchronizer::walk_monitor_list(
//				thread->omInUseList_addr(), &FreeHead, &FreeTail);
//		}

		oopDesc::encode_store_heap_oop(reference, NULL);	// JNIHandles::deleted_handle()
	}

/*
	ObjectMonitor* monitor = NULL;
	JavaThread* thread = ObjectSynchronizer::get_lock_owner(h_obj, false, &monitor);

	if (monitor != NULL) {
		tty->print("[INCINERATOR] Stale ref used for synchronization: ");
		print_value_on(tty, reference, referent);
		tty->cr();

		if (UseBiasedLocking) {
			BiasedLocking::revoke_at_safepoint(h_obj);
			assert(!h_obj()->mark()->has_bias_pattern(), "biases should be revoked by now");
		}

		if (thread != NULL) {
			monitor->notifyAll(thread);
			monitor->complete_exit(thread);
		}

		intptr_t monitor_count = monitor->count();
		if (monitor_count > 0) {
			monitor->set_count(--monitor_count);
		}

		if (monitor_count == 0) {
			ObjectMonitor *FreeHead = NULL, *FreeTail = NULL;
			ObjectSynchronizer::deflate_monitor(monitor, h_obj(), &FreeHead, &FreeTail);

			ObjectSynchronizer::walk_monitor_list(
				thread->omInUseList_addr(), &FreeHead, &FreeTail);
		}

//		oopDesc::encode_store_heap_oop(reference, NULL);	// JNIHandles::deleted_handle()

		if ((uintptr_t)obj->mark() == 3) {
			int i = 2;
		}

		return;
	}

	tty->print("[INCINERATOR] Stale ref eliminated: ");
	print_value_on(tty, reference, referent);
	tty->cr();

	oopDesc::encode_store_heap_oop(reference, NULL);
*/
}

template<typename T>
void StaleReferenceT<T>::print_value_on(outputStream* st, const T obj, const oop referent)
{
	oop the_obj = oopDesc::decode_heap_oop_not_null(obj);
	st->print("0x%x: ", the_obj);
	the_obj->print_value_on(st);
	st->print(", referent: ");
	referent->print_value_on(st);
}

template<typename T>
void StaleReferenceT<T>::print_value_on(outputStream* st, const T* reference, const oop referent)
{
	st->print("0x%x => ", reference);
	print_value_on(st, *reference, referent);
}

template<typename T>
void StaleReferenceT<T>::print_on(outputStream* st, const T obj, const oop referent)
{
	oop the_obj = oopDesc::decode_heap_oop_not_null(obj);
	st->print("0x%x: ", the_obj);
	the_obj->print_on(st);
	st->print_cr(" --- referent:");
	referent->print_on(st);
}

template<typename T>
void StaleReferenceT<T>::print_on(outputStream* st, const T* reference, const oop referent)
{
	st->print("0x%x => ", reference);
	print_on(st, *reference, referent);
}

}
