#pragma once

/*
	Three file-scope helpers that both halves of the menu need.

	They lived at the top of ContextMenu.cpp and were used from one translation
	unit, so file scope was the right home for them. Seam step 7 of
	docs/refactor/04-code-health.md section 4 moved the painting into
	MenuPresenter.cpp, and `DWM` and `ver` are used from both sides of that
	split - so they need a home both can see.

	This is the part of a translation-unit split that cannot be done by
	declaring things: `DWM` is a *type*, and a type cannot be forward-declared
	into use. `ver` could have been an extern declaration with the type spelled
	out, but splitting one of the three and leaving the others would be a worse
	explanation than moving all three together.

	`ver` is an inline variable (C++17) rather than a definition in one file and
	an extern in the other. That is what keeps it exactly one object with one
	initialisation across both translation units, which is what it was when both
	halves were one file:

	  "An inline function or variable with external linkage ... shall be defined
	   in every translation unit in which it is odr-used and shall have exactly
	   the same definition in every case."

	The bodies are unchanged from ContextMenu.cpp.
*/

struct DWM
{
	enum class BackdropType : int
	{
		Default = 0,
		None = 1,
		Mica = 2,
		Acrylic = 3,
		Tabbed = 4,
	};

	enum class Corner : int
	{
		Default = 0,
		None = 1,
		Round = 2,
		RoundSmall = 3,
		Last = 4,
	};

	HWND handle{};
	DWM(HWND hWnd = nullptr) : handle(hWnd) {}

	HRESULT RemoveCorner()
	{
		return SetCorner(Corner::None);
	}

	HRESULT SetCorner(Corner value = Corner::Round)
	{
		const auto DWMWA_WINDOW_CORNER_PREFERENCE = 33U;
		return SetAttribute(DWMWA_WINDOW_CORNER_PREFERENCE, value);
	}

	HRESULT SetBorderColor(COLORREF color)
	{
		const auto DWMWA_BORDER_COLOR = 34U;
		return SetAttribute(DWMWA_BORDER_COLOR, color);
	}

	/// <summary>
	/// Enable or disable immersive dark mode.
	/// Requires Windows build 19041 or higher.
	/// </summary>
	HRESULT SetImmersiveDarkMode(BOOL state = TRUE)
	{
		const auto DWMWA_IMMERSIVE_DARK_MODE = 20U;
		return SetAttribute(DWMWA_IMMERSIVE_DARK_MODE, state);
	}

	/// <summary>
	/// Set backdrop type on target window
	/// Requires Windows build 22523 or higher.
	/// </summary>
	HRESULT SetBackdropType(BackdropType backdropType)
	{
		const auto DWMWA_SYSTEMBACKDROP_TYPE = 38;
		return SetAttribute(DWMWA_SYSTEMBACKDROP_TYPE, backdropType);
	}

	/// <summary>
	/// Enable or Disable Mica on target window
	/// Supported on Windows builds from 22000 to 22523. It doesn't work on 22523, use <see cref="SetBackdropType(IntPtr, DWM_SYSTEMBACKDROP_TYPE)"/> instead.
	/// </summary>
	HRESULT SetMica(BOOL state = true)
	{
		const auto DWMWA_MICA = 1029U;
		return SetAttribute(DWMWA_MICA, state);
	}

	template<typename T>
	HRESULT SetAttribute(DWORD dwAttribute, T pvAttribute)
	{
		return ::DwmSetWindowAttribute(handle, dwAttribute, (const void *)&pvAttribute, sizeof(T));
	}

	auto ExtendFrameIntoClientArea(MARGINS margins = { -1 })
	{
		return ::DwmExtendFrameIntoClientArea(handle, &margins);
	}
};

inline HMENU GET_HMENU(HWND hWnd) { return SendMSG<HMENU>(hWnd, MN_GETHMENU, 0, 0); }

inline auto ver = &Windows::Version::Instance();
