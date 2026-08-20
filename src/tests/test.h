#pragma once

// Minimal self-registering test framework. Deliberately dependency-free so the
// test project builds anywhere the main solution does, with no package restore.
//
//   TEST(suite, name) { CHECK(expr); CHECK_EQ(a, b); }
//
// Failures are reported and counted; the process exit code is the number of
// failed assertions, capped at 125.

#include <cstdio>
#include <cstddef>
#include <vector>
#include <string>

namespace nss_test
{
	struct Registry
	{
		using Fn = void(*)();

		struct Entry { const char *suite; const char *name; Fn fn; };

		static std::vector<Entry> &entries()
		{
			static std::vector<Entry> v;
			return v;
		}

		static int &failures() { static int n = 0; return n; }
		static int &checks() { static int n = 0; return n; }
		static const char *&current() { static const char *s = ""; return s; }
	};

	struct Registrar
	{
		Registrar(const char *suite, const char *name, Registry::Fn fn)
		{
			Registry::entries().push_back({ suite, name, fn });
		}
	};

	inline void fail(const char *file, int line, const char *expr, const char *detail)
	{
		Registry::failures()++;
		std::printf("  FAIL %s\n        %s:%d\n        %s\n",
					Registry::current(), file, line, expr);
		if(detail && *detail)
			std::printf("        %s\n", detail);
	}

	// Renders a UTF-16 string as ASCII plus \uXXXX escapes so failure output is
	// readable regardless of the console code page.
	inline std::string escape(const wchar_t *s)
	{
		std::string out = "\"";
		char buf[16];
		for(; s && *s; s++)
		{
			auto c = static_cast<unsigned>(*s);
			if(c >= 0x20 && c < 0x7F)
				out += static_cast<char>(c);
			else
			{
				std::snprintf(buf, sizeof(buf), "\\u%04X", c);
				out += buf;
			}
		}
		out += "\"";
		return out;
	}

	inline int run(const char *filter = nullptr)
	{
		const char *suite = nullptr;
		for(auto &e : Registry::entries())
		{
			if(filter && *filter && std::string(e.suite).find(filter) == std::string::npos)
				continue;

			if(!suite || std::string(suite) != e.suite)
			{
				suite = e.suite;
				std::printf("\n[%s]\n", suite);
			}

			int before = Registry::failures();
			Registry::current() = e.name;
			// Flushed around the call, so a test that takes the process down with
			// it still says which one it was: stdout is fully buffered when
			// redirected, and a crash loses whatever is still in the buffer.
			std::fflush(stdout);
			e.fn();
			if(Registry::failures() == before)
				std::printf("  ok   %s\n", e.name);
			std::fflush(stdout);
		}

		std::printf("\n%d checks, %d failure(s)\n", Registry::checks(), Registry::failures());
		return Registry::failures() > 125 ? 125 : Registry::failures();
	}
}

#define NSS_CAT2(a, b) a##b
#define NSS_CAT(a, b) NSS_CAT2(a, b)

#define TEST(suite_, name_)                                                    \
	static void NSS_CAT(nss_test_fn_, __LINE__)();                             \
	static ::nss_test::Registrar NSS_CAT(nss_test_reg_, __LINE__)(             \
		#suite_, #name_, &NSS_CAT(nss_test_fn_, __LINE__));                    \
	static void NSS_CAT(nss_test_fn_, __LINE__)()

#define CHECK(expr_)                                                           \
	do {                                                                       \
		::nss_test::Registry::checks()++;                                      \
		if(!(expr_)) ::nss_test::fail(__FILE__, __LINE__, #expr_, "");         \
	} while(0)

#define CHECK_MSG(expr_, msg_)                                                 \
	do {                                                                       \
		::nss_test::Registry::checks()++;                                      \
		if(!(expr_)) ::nss_test::fail(__FILE__, __LINE__, #expr_, msg_);       \
	} while(0)

#define CHECK_EQ(a_, b_)                                                       \
	do {                                                                       \
		::nss_test::Registry::checks()++;                                      \
		auto va_ = (a_); auto vb_ = (b_);                                      \
		if(!(va_ == vb_)) {                                                    \
			char d_[128];                                                      \
			std::snprintf(d_, sizeof(d_), "got %lld, want %lld",               \
						  (long long)va_, (long long)vb_);                     \
			::nss_test::fail(__FILE__, __LINE__, #a_ " == " #b_, d_);          \
		}                                                                      \
	} while(0)