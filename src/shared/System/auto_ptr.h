#pragma once

namespace Nilesoft
{
	typedef class auto_handle
	{
		HANDLE _handle;

	public:
		
		auto_handle() : _handle(INVALID_HANDLE_VALUE) {}
		auto_handle(HANDLE handle) : _handle(handle == nullptr ? INVALID_HANDLE_VALUE : handle) {}
		auto_handle(auto_handle &&other) noexcept = default;
		// Non-copyable: two owners of one HANDLE would both close it.
		auto_handle(const auto_handle &) = delete;
		auto_handle &operator=(const auto_handle &) = delete;

		~auto_handle(void) { close(); }

		HANDLE release() noexcept
		{
			return std::exchange(_handle, INVALID_HANDLE_VALUE);
		}

		void reset(HANDLE other = INVALID_HANDLE_VALUE) noexcept
		{
			close();
			_handle = other;
		}

		bool valid() const { return (_handle && (_handle != INVALID_HANDLE_VALUE)); }
		void close() { if(valid()) ::CloseHandle(_handle); _handle = INVALID_HANDLE_VALUE; }

		template<typename T = HANDLE>
		T get() const { return static_cast<T>(_handle); }

		operator HANDLE(void) const { return _handle; }
		operator HINSTANCE(void) const { return (HINSTANCE)_handle; }
		explicit operator bool(void) const { return valid(); }

		auto_handle& operator=(auto_handle &&rhs) noexcept
		{
			if(&rhs != this || _handle != rhs._handle)
			{
				close();
				_handle = rhs.release();
			}
			return *this;
		}

		auto_handle &operator=(HANDLE handle)
		{
			if(handle != _handle)
				reset(handle);
			return *this;
		}
	}Handle;
}