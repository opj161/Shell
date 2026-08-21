#pragma once

#include <windows.h>
#include <objidl.h>

namespace Nilesoft
{
	namespace Shell
	{
		// One-shot same-process marshal, the primitive Microsoft recommends when
		// an interface is unmarshaled once rather than through the GIT.
		//
		//   https://learn.microsoft.com/windows/win32/com/single-threaded-apartments
		//   https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-comarshalinterthreadinterfaceinstream
		//   https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cogetinterfaceandreleasestream
		//
		// assign() runs in the source apartment. consume() runs in the destination
		// apartment and releases the stream even on failure. A failure leaves no
		// raw-pointer fallback: the caller must not use the original pointer from
		// the other apartment.
		class OneShotMarshal
		{
		public:
			OneShotMarshal() = default;
			~OneShotMarshal() { reset(); }

			OneShotMarshal(const OneShotMarshal &) = delete;
			OneShotMarshal &operator=(const OneShotMarshal &) = delete;

			OneShotMarshal(OneShotMarshal &&other) noexcept
				: _stream(other._stream)
			{
				other._stream = nullptr;
			}

			OneShotMarshal &operator=(OneShotMarshal &&other) noexcept
			{
				if(this != &other)
				{
					reset();
					_stream = other._stream;
					other._stream = nullptr;
				}
				return *this;
			}

			bool empty() const { return _stream == nullptr; }

			bool assign(IUnknown *unk, REFIID riid)
			{
				reset();
				if(!unk)
					return false;

				IStream *stream = nullptr;
				auto hr = ::CoMarshalInterThreadInterfaceInStream(riid, unk, &stream);
				if(FAILED(hr) || !stream)
				{
					if(stream)
						stream->Release();
					return false;
				}

				_stream = stream;
				return true;
			}

			// Consumes the stream. On success *ppv is an owning pointer valid in
			// this apartment. A second call is empty.
			bool consume(REFIID riid, void **ppv)
			{
				if(ppv)
					*ppv = nullptr;

				auto stream = _stream;
				_stream = nullptr;
				if(!stream)
					return false;

				void *local = nullptr;
				auto hr = ::CoGetInterfaceAndReleaseStream(stream, riid, &local);
				if(FAILED(hr) || !local)
				{
					if(local)
						static_cast<IUnknown *>(local)->Release();
					return false;
				}

				if(!ppv)
				{
					static_cast<IUnknown *>(local)->Release();
					return false;
				}

				*ppv = local;
				return true;
			}

			void reset()
			{
				IUnknown *discard = nullptr;
				if(consume(IID_IUnknown, reinterpret_cast<void **>(&discard)) && discard)
					discard->Release();
			}

		private:
			IStream *_stream = nullptr;
		};
	}
}
