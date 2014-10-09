
package Incinerator;

class Core
{
	public static native void run();
	public static native boolean markClassLoaderStale(Object class_loader);
	public static native void notifyObjectFinalized(Object obj);
}
