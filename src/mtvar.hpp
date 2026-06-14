#include <mutex>
#include <shared_mutex>


template<typename T> class MTVariable
{
private:
	T instance;
	mutable std::shared_mutex mutex;

public:
	MTVariable() : instance(empty()) { }
	MTVariable(T instance) : instance(instance) { }

	//Returns default instance
	T empty() const
	{
		return T();
	}

	T get() const
	{
		const auto lock = std::shared_lock(mutex);
		//Return copy
		return T(instance);
	}

	//Zero-copy membership test for associative T (set/map). Avoids the full
	//container copy that get() would make just to test one key. Only resolved
	//when actually called, so scalar MTVariables (bool/int/string) are unaffected.
	template<typename K> bool contains(const K& key) const
	{
		const auto lock = std::shared_lock(mutex);
		return instance.contains(key);
	}

	//Lookup-with-default for map-like T. Return type is bound to the container's
	//mapped_type (not the fallback's), so a narrower fallback literal (e.g. 0u
	//against a map<_, uint64_t>) cannot silently truncate the stored value.
	template<typename K, typename U = T> typename U::mapped_type getOr(const K& key, typename U::mapped_type fallback) const
	{
		const auto lock = std::shared_lock(mutex);
		auto it = instance.find(key);
		return it != instance.end() ? it->second : fallback;
	}

	//fn runs under the read lock with a reference to the live instance. fn MUST
	//return by value (or copy out what it needs) — returning or escaping a
	//reference into instance dangles once the lock is released.
	template<typename F> auto read(F&& fn) const -> decltype(fn(instance))
	{
		const auto lock = std::shared_lock(mutex);
		return fn(instance);
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
