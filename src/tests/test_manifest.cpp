// The application manifest, checked with an XML parser rather than by eye.
//
// Both copies of the manifest - the one compiled into the binaries and the
// template the version tool stamps over it - had two undeclared namespace
// prefixes, `asmv3:` and `ws2:`. That is not well-formed XML, and mt.exe
// refuses the shipped binaries outright:
//
//     general error c101008c: Failed to read the manifest from the resource of
//     file "...\shell.exe". Windows was unable to parse the requested XML data.
//
// Windows itself is quieter about it, which is why this survived: the parse
// stops at the bad prefix instead of failing. Everything before it is honoured
// - the Common-Controls 6.0 dependency, so themed controls work and the app
// looks correctly manifested - and everything from <asmv3:application> onward
// is dropped. Measured with three otherwise identical executables:
//
//     embedded manifest      GetThreadDpiAwarenessContext
//     none (control)         UNAWARE
//     the malformed one      UNAWARE
//     the documented one     PER_MONITOR_AWARE, PerMonitorV2
//
// So dpiAware, dpiAwareness, longPathAware and activeCodePage were all doing
// nothing at all. The namespaces asserted below are the ones Microsoft's own
// examples use, per element:
//
//     https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests
//
// activeCodePage belongs to the 2019 namespace, not the 2005 one the rest of
// the block defaults to - it was inheriting the wrong one even before the
// prefixes are counted.

#include "test.h"

#include <windows.h>
#include <msxml6.h>
#include <string>

namespace
{
	// src\bin\<arch>\tests.exe -> src\, so the manifests can be reached from
	// wherever the test binary was built.
	std::wstring source_root()
	{
		wchar_t module[MAX_PATH];
		DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
		if(n == 0 || n >= MAX_PATH)
			return {};

		std::wstring path(module, n);
		for(int i = 0; i < 3; i++)
		{
			size_t slash = path.find_last_of(L'\\');
			if(slash == std::wstring::npos)
				return {};
			path.resize(slash);
		}
		return path;
	}

	struct Com
	{
		HRESULT hr;
		Com() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
		~Com() { if(SUCCEEDED(hr)) CoUninitialize(); }
	};

	// Loads one manifest and reports what the parser made of it. Well-formedness
	// only: there is no schema to validate against, and the failure being pinned
	// here is a syntax error.
	struct Manifest
	{
		IXMLDOMDocument2 *doc = nullptr;
		std::wstring error;

		explicit Manifest(const std::wstring &path)
		{
			if(FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
									   IID_PPV_ARGS(&doc))))
			{
				error = L"DOMDocument60 unavailable";
				return;
			}

			doc->put_async(VARIANT_FALSE);
			doc->put_validateOnParse(VARIANT_FALSE);
			doc->put_resolveExternals(VARIANT_FALSE);

			// XPath, and the prefixes the queries below use. MSXML defaults to
			// XSLPattern, which has no namespace support at all.
			set_property(L"SelectionLanguage", L"XPath");
			set_property(L"SelectionNamespaces",
						 L"xmlns:asm='urn:schemas-microsoft-com:asm.v1' "
						 L"xmlns:asmv3='urn:schemas-microsoft-com:asm.v3' "
						 L"xmlns:ws2005='http://schemas.microsoft.com/SMI/2005/WindowsSettings' "
						 L"xmlns:ws2016='http://schemas.microsoft.com/SMI/2016/WindowsSettings' "
						 L"xmlns:ws2019='http://schemas.microsoft.com/SMI/2019/WindowsSettings'");

			VARIANT_BOOL ok = VARIANT_FALSE;
			BSTR file = SysAllocStringLen(path.c_str(), static_cast<UINT>(path.size()));
			VARIANT src;
			VariantInit(&src);
			src.vt = VT_BSTR;
			src.bstrVal = file;
			doc->load(src, &ok);
			VariantClear(&src);

			if(ok == VARIANT_TRUE)
				return;

			IXMLDOMParseError *pe = nullptr;
			if(SUCCEEDED(doc->get_parseError(&pe)) && pe)
			{
				BSTR reason = nullptr;
				long line = 0;
				pe->get_reason(&reason);
				pe->get_line(&line);
				error = L"line " + std::to_wstring(line) + L": " + (reason ? reason : L"");
				SysFreeString(reason);
				pe->Release();
			}
			else
				error = L"parse failed";

			doc->Release();
			doc = nullptr;
		}

		~Manifest() { if(doc) doc->Release(); }

		void set_property(const wchar_t *name, const wchar_t *value)
		{
			BSTR n = SysAllocString(name);
			VARIANT v;
			VariantInit(&v);
			v.vt = VT_BSTR;
			v.bstrVal = SysAllocString(value);
			doc->setProperty(n, v);
			VariantClear(&v);
			SysFreeString(n);
		}

		// Text of the first node matching an XPath, or a sentinel that cannot be
		// confused with a real value.
		std::wstring text(const wchar_t *xpath) const
		{
			if(!doc)
				return L"<no document>";

			BSTR q = SysAllocString(xpath);
			IXMLDOMNode *node = nullptr;
			HRESULT hr = doc->selectSingleNode(q, &node);
			SysFreeString(q);

			if(FAILED(hr) || !node)
				return L"<absent>";

			BSTR value = nullptr;
			node->get_text(&value);
			std::wstring out = value ? value : L"";
			SysFreeString(value);
			node->Release();
			return out;
		}
	};

	const wchar_t *kSettings =
		L"/asm:assembly/asmv3:application/asmv3:windowsSettings";

	void check_manifest(const wchar_t *relative)
	{
		std::wstring root = source_root();
		CHECK_MSG(!root.empty(), "could not locate the source tree from the test binary");
		if(root.empty())
			return;

		std::wstring path = root + L"\\" + relative;

		Manifest m(path);
		CHECK_MSG(m.doc != nullptr,
				  m.error.empty() ? "manifest did not parse"
								  : nss_test::escape(m.error.c_str()).c_str());
		if(!m.doc)
			return;

		// Each setting has to resolve in the namespace its own documented
		// example uses; the prefix being merely *present* is what the broken
		// version already had.
		CHECK(m.text((std::wstring(kSettings) + L"/ws2005:dpiAware").c_str()) == L"True/PM");
		CHECK(m.text((std::wstring(kSettings) + L"/ws2016:dpiAwareness").c_str())
			  == L"PerMonitorV2,PerMonitor");
		CHECK(m.text((std::wstring(kSettings) + L"/ws2016:longPathAware").c_str()) == L"true");
		CHECK(m.text((std::wstring(kSettings) + L"/ws2019:activeCodePage").c_str()) == L"UTF-8");
	}
}

TEST(manifest, embedded_manifest_is_well_formed_and_namespaced)
{
	Com com;
	check_manifest(L"shared\\Resource\\manifest.xml");
}

// The version tool copies this one over the other. Fixing only the destination
// would last exactly until the next version bump.
TEST(manifest, version_tool_template_matches)
{
	Com com;
	check_manifest(L"tools\\version\\manifest.xml");
}
