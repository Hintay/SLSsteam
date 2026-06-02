#include <mutex>
#include <shared_mutex>


template<typename T> class MTVariable
{
private:
	T instance;
	std::shared_mutex mutex;

public:
	MTVariable() : instance(empty()) { }
	MTVariable(T instance) : instance(instance) { }

	//Returns default instance
	T empty()
	{
		return T();
	}

	T get()
	{
		const auto lock = std::shared_lock(mutex);
		//Return copy
		return T(instance);
	}

	void set(T value)
	{
		const auto lock = std::unique_lock(mutex);
		instance = value;
	}

	//Atomic read-modify-write: the mutator runs under the writer lock with a
	//reference to the live instance. Use this instead of get()+set() whenever
	//the new value depends on the old one and another thread may race (e.g.
	//config hot-reload merging into a map that Steam worker threads read).
	template<typename F> void update(F&& fn)
	{
		const auto lock = std::unique_lock(mutex);
		fn(instance);
	}

	void operator=(MTVariable<T> other)
	{
		set(other.instance);
	}
};
