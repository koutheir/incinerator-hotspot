
package Incinerator;

public class Incinerator
{
	private static boolean _incinerator_loaded;
	
	static {
		try {
			Core.markClassLoaderStale(null);
			_incinerator_loaded = true;
		} catch (UnsatisfiedLinkError e) {
			_incinerator_loaded = false;
		}
	}
	
	public static void run()
	{
		if (!_incinerator_loaded) return;
		Core.run();
	}
	
	public static boolean markClassLoaderStale(Object class_loader)
	{
		if (!_incinerator_loaded) return false;
		return Core.markClassLoaderStale(class_loader);
	}
	
	public static void notifyObjectFinalized(Object obj)
	{
		if (!_incinerator_loaded) return;
		Core.notifyObjectFinalized(obj);
	}
}
