// IComPtr, counted rather than reasoned about.
//
// The wrapper had five ownership defects, and the reason none of them showed up
// as a crash on a developer's machine is that COM interfaces are usually held by
// something else as well - the leak or the extra Release lands on a refcount
// that has room to absorb it. A fake IUnknown that counts exactly does not.
//
//   * the move constructor was `= default`, so it copied the pointer and left
//     the source still holding it: both destructors released the same object;
//   * release() released and left the member pointing at the released object;
//   * the output conversions released, then handed a COM call the address of
//     that stale pointer - so a call that failed without writing left a
//     dangling pointer for the destructor to release a second time;
//   * CreateInstance wrote over the member without releasing what was there;
//   * copy assignment called a swap that cannot bind to its const argument, and
//     compiled only because nothing instantiated it.
//
// Every test below asserts the object was destroyed exactly once.

#include "test.h"

#include <windows.h>
#include <atomic>

#include "System.h"

using namespace Nilesoft;

namespace
{
	// Counts its own references and records its destruction.
	struct CountedUnknown : public IUnknown
	{
		std::atomic<ULONG> refs{ 1 };
		int *destroyed;
		ULONG peak = 1;

		explicit CountedUnknown(int *flag) : destroyed(flag) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
		{
			if(!out)
				return E_POINTER;
			if(riid == IID_IUnknown)
			{
				*out = this;
				AddRef();
				return S_OK;
			}
			*out = nullptr;
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			ULONG now = ++refs;
			if(now > peak)
				peak = now;
			return now;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			ULONG now = --refs;
			if(now == 0)
				(*destroyed)++;
			return now;
		}
	};
}

TEST(comptr, destroys_exactly_once)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);
	{
		IComPtr<IUnknown> ptr;
		ptr.attach(&object);
		CHECK_EQ((int)object.refs.load(), 1);
	}
	CHECK_EQ(destroyed, 1);
	CHECK_EQ((int)object.refs.load(), 0);
}

TEST(comptr, copy_construction_adds_a_reference)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);
	{
		IComPtr<IUnknown> first;
		first.attach(&object);
		{
			IComPtr<IUnknown> second(first);
			CHECK_EQ((int)object.refs.load(), 2);
		}
		CHECK_EQ((int)object.refs.load(), 1);
		CHECK_EQ(destroyed, 0);
	}
	CHECK_EQ(destroyed, 1);
}

// The defaulted move constructor copied the pointer and left the source holding
// it, so this destroyed the object twice.
TEST(comptr, move_construction_steals_rather_than_copies)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);
	{
		IComPtr<IUnknown> source;
		source.attach(&object);

		IComPtr<IUnknown> target(std::move(source));

		CHECK_MSG(source.pointer == nullptr, "the moved-from pointer must not still own it");
		CHECK(target.pointer == &object);
		CHECK_MSG((int)object.refs.load() == 1, "moving must not add a reference");
	}
	CHECK_MSG(destroyed == 1, "released twice - once by each of them");
	CHECK_EQ((int)object.refs.load(), 0);
}

TEST(comptr, copy_assignment_releases_the_old_and_keeps_the_new)
{
	int a_gone = 0, b_gone = 0;
	CountedUnknown a(&a_gone);
	CountedUnknown b(&b_gone);
	{
		IComPtr<IUnknown> left;
		left.attach(&a);
		IComPtr<IUnknown> right;
		right.attach(&b);

		left = right;

		CHECK_MSG(a_gone == 1, "the pointer it was holding must be released");
		CHECK_EQ((int)b.refs.load(), 2);
	}
	CHECK_EQ(b_gone, 1);
}

TEST(comptr, self_assignment_does_not_destroy_the_object)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);
	{
		IComPtr<IUnknown> ptr;
		ptr.attach(&object);

		IComPtr<IUnknown> &alias = ptr;
		ptr = alias;

		CHECK_MSG(destroyed == 0, "assigning to itself must not release what it holds");
		CHECK_EQ((int)object.refs.load(), 1);
		CHECK(ptr.pointer == &object);
	}
	CHECK_EQ(destroyed, 1);
}

TEST(comptr, release_forgets_the_pointer_it_released)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);

	IComPtr<IUnknown> ptr;
	ptr.attach(&object);
	ptr.release();

	CHECK_EQ(destroyed, 1);
	CHECK_MSG(ptr.pointer == nullptr,
			  "release used to leave the member pointing at the released object");

	// Which is what made this safe: a second release must be a no-op, not a
	// second Release on freed memory.
	ptr.release();
	CHECK_EQ(destroyed, 1);
}

TEST(comptr, detach_hands_ownership_over_without_releasing)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);
	{
		IComPtr<IUnknown> ptr;
		ptr.attach(&object);

		IUnknown *raw = ptr.detach();
		CHECK(raw == &object);
		CHECK(ptr.pointer == nullptr);
		CHECK_EQ((int)object.refs.load(), 1);
	}
	CHECK_MSG(destroyed == 0, "the scope no longer owned it");
	object.Release();
	CHECK_EQ(destroyed, 1);
}

// The dangling-output case: a COM call that fails without writing the pointer.
TEST(comptr, a_failed_out_parameter_leaves_it_empty)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);

	IComPtr<IUnknown> ptr;
	ptr.attach(&object);

	// What every call site does: hand the wrapper to a COM call as an out
	// parameter. This one fails and writes nothing.
	auto failing_call = [](void **out) -> HRESULT { (void)out; return E_NOINTERFACE; };
	HRESULT hr = failing_call(ptr.reset());

	CHECK(FAILED(hr));
	CHECK_EQ(destroyed, 1);
	CHECK_MSG(ptr.pointer == nullptr,
			  "the destructor would otherwise release this a second time");
}

TEST(comptr, put_releases_before_handing_out_its_address)
{
	int destroyed = 0;
	CountedUnknown first(&destroyed);
	int second_gone = 0;
	CountedUnknown second(&second_gone);

	{
		IComPtr<IUnknown> ptr;
		ptr.attach(&first);

		IUnknown **out = ptr.put();
		CHECK_MSG(destroyed == 1, "reusing the pointer must release what it held");
		CHECK(ptr.pointer == nullptr);

		*out = &second;   // the COM call succeeds this time
		CHECK(ptr.pointer == &second);
	}
	CHECK_EQ(second_gone, 1);
}

TEST(comptr, query_interface_failure_leaves_nothing_behind)
{
	int destroyed = 0;
	CountedUnknown object(&destroyed);

	IComPtr<IUnknown> source;
	source.attach(&object);

	IComPtr<IUnknown> target;
	// IID_IDispatch is not implemented by the fake.
	HRESULT hr = source->QueryInterface(IID_IDispatch, target.reset());
	CHECK(FAILED(hr));
	CHECK(target.pointer == nullptr);
	CHECK_MSG((int)object.refs.load() == 1, "a failed QueryInterface must not add a reference");
}

TEST(comptr, swap_exchanges_ownership_without_touching_refcounts)
{
	int a_gone = 0, b_gone = 0;
	CountedUnknown a(&a_gone);
	CountedUnknown b(&b_gone);
	{
		IComPtr<IUnknown> left;
		left.attach(&a);
		IComPtr<IUnknown> right;
		right.attach(&b);

		left.swap(right);

		CHECK(left.pointer == &b);
		CHECK(right.pointer == &a);
		CHECK_EQ((int)a.refs.load(), 1);
		CHECK_EQ((int)b.refs.load(), 1);
	}
	CHECK_EQ(a_gone, 1);
	CHECK_EQ(b_gone, 1);
}
