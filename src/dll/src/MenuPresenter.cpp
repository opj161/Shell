#include <pch.h>
#include "Include/Theme.h"
#include "Include/ContextMenu.h"

/*
	The presenter: everything that turns a composed menu item into pixels.

	Seam step 7 of docs/refactor/04-code-health.md section 4, first half.
	Section 04.4 orders it last and says why - "paint/window events leave
	ContextMenu.cpp only after 1-6 are stable" - and section 08.3.8 named the
	coverage it had to wait for, which now exists: the four `render.*` scenarios
	in src/tests/hostprobe read the live menu back through MSAA and assert item
	order, layout containment and submenu placement. Those are precisely the
	regressions this code can introduce and that no other test in the tree would
	notice, because they look like a menu that draws slightly wrong rather than
	like something that fails.

	**This is a move, not a rewrite**, which is section 04.4's rule for every
	seam: "move code, don't improve it in the same commit". The bodies below are
	byte-identical to the ones that were in ContextMenu.cpp, verified by
	extracting them by line range rather than by retyping them - the same method
	that made seam step 5 safe.

	They are still `ContextMenu::` members, and deliberately so. Turning them
	into a `Win32MenuPresenter` class means giving it an interface onto
	ContextMenu's private state - `_theme`, `_items`, `_hTheme`, `symbol`,
	`composition`, the DC and font caches - and that interface cannot be
	designed honestly until the dependency is visible. Splitting the translation
	unit is what makes it visible: whatever this file reaches for is the
	presenter's surface. Naming the class is the next commit, not this one.

	What is NOT here yet is the other half - `screenshot`, `draw_layer`,
	`UpdateLayered` and `CreateLayer`, the layer-window compositing. It has one
	entanglement this half does not: `screenshot` calls
	`plutovg_stbi_write_png`, which has external linkage and is *defined* in
	Include/stb_image_write.h, so including that header in a second translation
	unit is a duplicate symbol at link time rather than a compile error where
	the mistake is. Moving it needs a declaration instead of the include, which
	is a decision worth making on its own.
*/

using namespace Nilesoft::Diagnostics;

#define _log Logger::Instance()

namespace Nilesoft
{
	namespace Shell
	{
		inline static MenuItemInfo *get_item(uint32_t id, const std::vector<MenuItemInfo *> &list)
		{
			for(auto item : list)
			{
				if(item->wID == id) return item;
			}
			return nullptr;
		}

		inline static MenuItemInfo *get_item(uint32_t id, HMENU hMenu, const std::vector<MenuItemInfo *> &list)
		{
			for(auto item : list)
			{
				if(item->handle == hMenu)
				{
					if(item->wID == id)
					{
						return item;
					}
				}
			}
			return nullptr;
		}

		//
		void ContextMenu::draw_string(HDC hdc, HFONT hFont, const Rect *rc, const Color &color, const wchar_t *text, int length, DWORD format, bool disable_BufferedPaint)
		{
			if(color.a == 0)
				return;

			BufferedPaint bp(hdc, rc);
			HDC hdcPaint = disable_BufferedPaint ? nullptr : bp.begin(color.a);

			if(!hdcPaint)
				hdcPaint = hdc;
			if(hdcPaint)
			{
				bp.clear();
				//::SetTextColor(hdcPaint, color);
				//::SetBkMode(hdcPaint, TRANSPARENT);
				auto hFontOld = ::SelectObject(hdcPaint, hFont);
				DTTOPTS dttOpts = { sizeof(DTTOPTS),  DTT_COMPOSITED | DTT_TEXTCOLOR, color.to_BGR() };
				//::SetTextAlign(hdcPaint, TA_BASELINE | TA_UPDATECP);
				::DrawThemeTextEx(_hTheme, hdcPaint, 0, 0, text, length, format, const_cast<Rect *>(rc), &dttOpts);
				::SelectObject(hdcPaint, hFontOld);
			}
		}

		void ContextMenu::draw_rect(DC *dc, const POINT &pt, const SIZE &size, const Color &color, const Color &border, int radius)
		{
			PlutoVG pluto(size.cx, size.cy);

			if(border.a > 0)
			{
				pluto.rect(.5, .5, size.cx - 1, size.cy - 1, radius, radius);
				if(color.a > 0)
					pluto.fill(color.to_RGB(), color.a, true);
				pluto.stroke_width(1)
					.stroke_fill(border.to_RGB(), border.a);
			}
			else
			{
				pluto.rect(0, 0, size.cx, size.cy, radius, radius)
					.fill(color.to_RGB(), color.a, true);
			}

			auto_gdi<HBITMAP> hbitmap(pluto.tobitmap());
			dc->draw_image(pt, size, hbitmap.get());
		}


		struct DRAWITEMSTATE
		{
			bool selected{};
			bool disabled{};
			bool grayed{};
			bool checked{};
			bool focus{};
			bool default${};
			bool hotlight{};
			bool inactive{};
			bool no_accel{};
			bool no_focus_rect{};

			DRAWITEMSTATE(uint32_t flags)
			{
				Flag<uint32_t> fs = flags;
				selected = fs.has(ODS_SELECTED);
				grayed = fs.has(ODS_GRAYED);
				disabled = fs.has(ODS_DISABLED) || grayed;
				checked = fs.has(ODS_SELECTED);
			}
		};


		LRESULT ContextMenu::OnDrawItem(DRAWITEMSTRUCT *di)
		{
			LRESULT lret = TRUE;
			//current.selectitem = nullptr;
			if(di->itemID == 0x5ffffffe)
				return lret;



			
			auto hMenu = reinterpret_cast<HMENU>(di->hwndItem);
			auto rc = reinterpret_cast<const Rect *>(&di->rcItem);

			Flag<uint32_t> faction = di->itemAction;
			Flag<uint32_t> fState = di->itemState;

			DRAWITEMSTATE state(di->itemState);

			auto draw_entire = faction.has(ODA_DRAWENTIRE);

			Color back_color = _theme.back.color.nor;
			Color text_color = _theme.text.color.nor;

			_tip.hide(!draw_entire);

			DC dc = di->hDC;

			dc.set_back_mode();
			
			if(state.selected)
			{
				if(state.disabled)
				{
					back_color = _theme.back.color.sel_dis;
					text_color = _theme.text.color.sel_dis;
				}
				else
				{
					back_color = _theme.back.color.sel;
					text_color = _theme.text.color.sel;
				}
			}
			else if(state.disabled)
			{
				back_color = _theme.back.color.nor_dis;
				text_color = _theme.text.color.nor_dis;
			}

			if(di->itemID == MF_NOITEM)
			{
				auto rect = *rc;
				rect.left += _theme.separator.margin.left;
				rect.right -= _theme.separator.margin.right;
				//rect.top += _theme.separator.margin.top;		
				rect.top += _theme.separator.margin.top;
				rect.bottom = rect.top + _theme.separator.size;
				//draw_rect(&dc, rc->point(), rc->size(), _theme.background.color);
				dc.fill_rect(*rc, composition ? dc.stock_brush(BLACK_BRUSH) : _hbackground);
				draw_rect(&dc, rect.point(), { rect.width(), _theme.separator.size }, _theme.separator.color);
				return 1;
			}

			//dc.draw_fillrect({ rc->left, rc->top, rc->right, rc->bottom }, (composition ? 0x000000 : _theme.background.color));
			//dc.fill_rect(di->rcItem, composition ? dc.stock_brush(BLACK_BRUSH) : _hbackground);

			auto menu = &_menus[hMenu];

			auto mii = get_item(di->itemID, hMenu, _items);

			if(!mii || (mii->title.empty() && !ident.equals(mii->wID)))
			{
				if(auto_gdi<HBITMAP> hbitmap(dc.createbitmap(rc->width(), rc->height())); hbitmap)
				{
					DC dcmem(dc.CreateCompatibleDC(), 1);
					auto old_bmp = ::SelectObject(dcmem, hbitmap.get());

					auto old_hdc = di->hDC;
					di->hDC = dcmem;

					lret = msg.invoke();
					
					di->hDC = old_hdc;

					std::vector<COLORREF> pixels(rc->width() * rc->height());

					BITMAPINFOHEADER bmpInfo = { 0 };
					bmpInfo.biSize = sizeof(bmpInfo);
					bmpInfo.biWidth = rc->width();
					bmpInfo.biHeight = -int(rc->height());
					bmpInfo.biPlanes = 1;
					bmpInfo.biBitCount = 32;
					bmpInfo.biCompression = BI_RGB;

					// Deselect bitmap before calling GetDIBits/SetDIBits per Win32 contract
					::SelectObject(dcmem, old_bmp);
					::GetDIBits(dc, hbitmap.get(), 0, rc->height(), &pixels[0], (LPBITMAPINFO)&bmpInfo, DIB_RGB_COLORS);

					std::for_each(pixels.begin(), pixels.end(), [](COLORREF &pixel) {
						if(pixel != 0) // black pixels stay transparent
							pixel |= 0xFF000000; // set alpha channel to 100%
					});

					::SetDIBits(dc, hbitmap.get(), 0, rc->height(), &pixels[0], (LPBITMAPINFO)&bmpInfo, DIB_RGB_COLORS);
					::SelectObject(dcmem, hbitmap.get());
					dc.draw_image(rc->point(), rc->size(), dcmem);
					::SelectObject(dcmem, old_bmp);

					return lret;
				}

				dc.set_back_mode(true);
				dc.set_back(back_color);
				dc.set_text(text_color);
				dc.fill_rect(di->rcItem, composition ? dc.stock_brush(BLACK_BRUSH) : _hbackground);

				lret = msg.invoke();

				return lret;
			}

			auto is_label = mii->visibility == Visibility::Label;
			auto is_static = mii->visibility == Visibility::Static;
			auto is_static_or_label = is_static || is_label;

			if(draw_entire)
			{
				mii->index = MENU::get_index(hMenu, mii->wID);
				::GetMenuItemRect(0, hMenu, mii->index, &mii->rect);
				dc.fill_rect(di->rcItem, composition ? dc.stock_brush(BLACK_BRUSH) : _hbackground);
			}
			else
			{
				if(state.disabled && is_static_or_label)
				{
					dc.exclude_clip_rect(*rc);
					return true;
				}
				
				dc.fill_rect(di->rcItem, composition ? dc.stock_brush(BLACK_BRUSH) : _hbackground);
			}

			if(!(state.disabled && is_static_or_label))
			{
				if(state.selected)
				{
					current.select_previtem = current.selectitem;
					current.selectitem = mii;
					if(mii->tip)
						current.tip = mii;
				}
			}

			if(state.disabled)
			{
				if(is_static_or_label)
				{
					state.disabled = false;
					back_color = _theme.back.color.nor;
					text_color = _theme.text.color.nor;
				}
			}

			const long image_size = _theme.image.size;

			auto rcblock = *rc;
			
			rcblock.top += _theme.back.margin.top;
			rcblock.bottom -= _theme.back.margin.bottom;

			if(mii->cch > 0 || !menu->has_col)
			{
				rcblock.left += _theme.back.margin.left;
				rcblock.right -= _theme.back.margin.right;
			}
			else
			{
				rcblock.left += _theme.back.margin.left + dpi(3);
				rcblock.right -= _theme.back.margin.right + dpi(3);
			}

			const auto width = rcblock.width();
			const auto height = rcblock.height();

			auto rcimg = rcblock;
			auto rcText = rcblock;

			rcimg.top = rcblock.top + ((height - image_size) / 2);
			rcimg.bottom = rcimg.top + image_size;

			if(mii->cch > 0 || !menu->has_col)
			{
				if(mii->cch == 0)
				{
				}
				else 
				{
					rcimg.left = rcblock.left + _theme.back.padding.left;
					rcimg.right = rcimg.left + image_size;
				}
			}

			if(!is_static_or_label)
			{
				uint8_t op = back_color.a;

				Color border_color = _theme.back.border.nor;

				if(state.selected)
				{
					if(state.disabled && _theme.back.color.sel_dis.a > 0)
						op = _theme.back.color.sel_dis.a;
					else if(!state.disabled && _theme.back.color.sel_dis.a > 0)
						op = _theme.back.color.sel.a;

					border_color = state.disabled ? _theme.back.border.sel_dis : _theme.back.border.sel;
				}
				else
				{
					if(state.disabled && _theme.back.color.nor_dis.a > 0)
						op = _theme.back.color.nor_dis.a;
					else if(!state.disabled && _theme.back.color.nor.a > 0)
						op = _theme.back.color.nor.a;

					if(state.disabled)
						border_color = _theme.back.border.nor_dis;
				}
				
				//dc.draw_fill_rounded_rect(rcblock, _theme.back.radius+2, 0,0);
				//draw_rect(&dc, rcblock.point(), { width, height }, 0xff000000, {}, _theme.back.radius);
				//draw_rect(&dc, rcblock.point(), { width, height }, _theme.background.color, {}, _theme.back.radius);
				//if(op > 0)
				{
					back_color.a = op;
					draw_rect(&dc, rcblock.point(), { width, height }, back_color, border_color, _theme.back.radius);
				}
			}

			auto has_checked_image = menu->draw.checks && menu->draw.images && (_theme.image.display >= 2);

			if(!is_label)
			{
				auto rcchekhed = rcimg;
				if(has_checked_image)
				{
					auto offset = _theme.image.size + _theme.image.gap;
					rcimg.left += offset;
					rcimg.right += offset;
				}

				//Draw custom submenu arrow and checked
				if(mii->is_popup() || mii->is_checked())
				{
					auto sy = mii->is_popup() ? &symbol.chevron : mii->is_radiocheck() ? &symbol.bullet : &symbol.checked;
					auto hbitmap = state.selected ? sy->select : sy->normal;
					
					if(state.disabled)
						hbitmap = state.selected ? sy->select_disabled : sy->normal_disabled;

					if(mii->is_popup())
					{
						auto rect = rcblock;
						auto x = rect.right - (_theme.back.padding.right + symbol.chevron.size.cx);
						auto y = rcblock.top + ((height - symbol.chevron.size.cy) / 2);
						dc.draw_image({ x, y }, { symbol.chevron.size.cx, symbol.chevron.size.cy }, hbitmap, state.disabled ? 64 : 255);
					}
					else if(mii->is_checked() && (_theme.image.display != 1 || !mii->has_image_or_draw()))
					{
						long z = _theme.image.size;
						dc.draw_image(rcchekhed.point(), { z, z }, hbitmap, state.disabled ? 64 : 255);
					}
				}

				auto image = &mii->image;

				if(state.selected && mii->image_select.isvalid())
					image = &mii->image_select;

 
				auto color_luma = [](const Color &color)
				{
					return (int(color.r()) * 299 + int(color.g()) * 587 + int(color.b()) * 114) / 1000;
				};
				auto blended_luma = [](const Color &front, const Color &back)
				{
					auto a = int(front.a);
					auto r = (int(front.r()) * a + int(back.r()) * (255 - a)) / 255;
					auto g = (int(front.g()) * a + int(back.g()) * (255 - a)) / 255;
					auto b = (int(front.b()) * a + int(back.b()) * (255 - a)) / 255;
					return (r * 299 + g * 587 + b * 114) / 1000;
				};

				auto icon_back_luma = blended_luma(back_color, _theme.background.color);

				auto light_icon_recolor_required = [&](int visible_pixels, int colorful_pixels,
													   long long visible_luma, long long visible_spread,
													   int min_luma, int max_luma,
													   int image_area)
				{
					if(visible_pixels <= 0)
						return false;

					auto avg_luma = visible_luma / visible_pixels;
					auto avg_spread = visible_spread / visible_pixels;
					auto min_visible = image_area / 96;
					if(min_visible < 4)
						min_visible = 4;

					if(max_luma - min_luma > 96)
						return false;

					return visible_pixels >= min_visible &&
						abs(int(avg_luma) - icon_back_luma) < 80 &&
						avg_spread < 28 &&
						colorful_pixels <= (visible_pixels / 16 > 2 ? visible_pixels / 16 : 2);
				};

				auto readable_icon_color = [&]()
				{
					Color icon_color = _theme.image.color[0];
					if(!icon_color)
					{
						if(state.disabled && _theme.text.color.nor_dis)
							icon_color = _theme.text.color.nor_dis;
						else
							icon_color = text_color;
					}

					auto contrast = abs(color_luma(icon_color) - icon_back_luma);
					if(contrast < 80)
					{
						auto text_contrast = abs(color_luma(text_color) - icon_back_luma);
						if(text_contrast > contrast)
						{
							icon_color = text_color;
							contrast = text_contrast;
						}
					}

					if(contrast < 80)
						icon_color = icon_back_luma > 128 ? Color(0, 0, 0, 0xFF) : Color(0xFF, 0xFF, 0xFF, 0xFF);

					return icon_color;
				};

				auto recolor_icon_bits = [&](uint32_t *bits, int image_area)
				{
					auto icon_color = readable_icon_color();
					for(auto i = 0; i < image_area; i++)
					{
						auto a = int((bits[i] >> 24) & 0xFF);
						if(a == 0)
							continue;

						a = a * icon_color.a / 255;
						auto pre_r = int(icon_color.r()) * a / 255;
						auto pre_g = int(icon_color.g()) * a / 255;
						auto pre_b = int(icon_color.b()) * a / 255;
						bits[i] = (a << 24) | (pre_r << 16) | (pre_g << 8) | pre_b;
					}
				};

				if(mii->is_toplevel &&
				   mii->native_ownerdraw &&
				   !mii->native_icon_checked &&
				   !mii->has_image_or_draw())
				{
					mii->native_icon_checked = true;

					if(auto_gdi<HBITMAP> hbitmap(dc.createbitmap(rc->width(), rc->height())); hbitmap)
					{
						DC dcmem(dc.CreateCompatibleDC(), 1);
						auto old_bmp = ::SelectObject(dcmem, hbitmap.get());

						BITMAPINFOHEADER bmpInfo = { 0 };
						bmpInfo.biSize = sizeof(bmpInfo);
						bmpInfo.biWidth = rc->width();
						bmpInfo.biHeight = -int(rc->height());
						bmpInfo.biPlanes = 1;
						bmpInfo.biBitCount = 32;
						bmpInfo.biCompression = BI_RGB;

						auto w = rc->width(), h = rc->height();
						auto count = w * h;
						auto capture_right = rcimg.right - rc->left + _theme.image.gap;
						if(capture_right < long(_theme.image.size))
							capture_right = _theme.image.size;
						if(capture_right > w)
							capture_right = w;

						if(auto_gdi<HBRUSH> hbr(CreateSolidBrush(RGB(0x7F, 0x7F, 0x7F))); hbr)
							dcmem.fill_rect({ 0, 0, w, h }, hbr.get());

						auto_gdi<HRGN> clip(::CreateRectRgn(0, 0, capture_right, h));
						auto old_clip = ::SaveDC(dcmem);
						if(clip)
							::SelectClipRgn(dcmem, clip.get());

						auto old_hdc = di->hDC;
						auto old_rcItem = di->rcItem;
						auto old_state = di->itemState;
						di->hDC = dcmem;
						di->rcItem = { 0, 0, w, h };
						di->itemState &= ~(ODS_SELECTED | ODS_FOCUS | ODS_HOTLIGHT);
						msg.invoke();
						di->hDC = old_hdc;
						di->rcItem = old_rcItem;
						di->itemState = old_state;
						if(old_clip)
							::RestoreDC(dcmem, old_clip);

						// Deselect bitmap before GetDIBits
						::SelectObject(dcmem, old_bmp);
						std::vector<COLORREF> pixels(count);
						::GetDIBits(dc, hbitmap.get(), 0, h, &pixels[0],
									 (LPBITMAPINFO)&bmpInfo, DIB_RGB_COLORS);

						long zone_left = 0;
						long zone_top = 0;
						long zone_right = capture_right;
						long zone_bottom = h;
						auto icon_padding = long(_theme.image.size) / 8;
						if(icon_padding < 2)
							icon_padding = 2;

						auto icon_right = rcimg.right - rc->left;
						auto icon_search_right = icon_right + icon_padding;
						if(icon_search_right > zone_right)
							icon_search_right = zone_right;

						std::vector<int> color_buckets(32768);
						for(auto y = zone_top; y < zone_bottom; y++)
						{
							for(auto x = zone_left; x < zone_right; x++)
							{
								uint32_t p = pixels[y * w + x];
								auto bucket = int((((p >> 16) & 0xFF) >> 3) << 10) |
									int((((p >> 8) & 0xFF) >> 3) << 5) |
									int(((p & 0xFF) >> 3));
								color_buckets[bucket]++;
							}
						}

						int bg_bucket = 0;
						for(auto i = 1; i < int(color_buckets.size()); i++)
						{
							if(color_buckets[i] > color_buckets[bg_bucket])
								bg_bucket = i;
						}

						int b0 = ((bg_bucket & 0x1F) << 3) + 4;
						int b1 = (((bg_bucket >> 5) & 0x1F) << 3) + 4;
						int b2 = (((bg_bucket >> 10) & 0x1F) << 3) + 4;

						int max_diff = 0;
						for(auto y = zone_top; y < zone_bottom; y++)
						{
							for(auto x = zone_left; x < zone_right; x++)
							{
								uint32_t p = pixels[y * w + x];
								int d0 = abs(int(p & 0xFF) - b0);
								int d1 = abs(int((p >> 8) & 0xFF) - b1);
								int d2 = abs(int((p >> 16) & 0xFF) - b2);
								int d = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
								if(d > max_diff)
									max_diff = d;
							}
						}

						if(max_diff > 16)
						{
							auto threshold = max_diff / 10;
							if(threshold < 12)
								threshold = 12;

							std::vector<int> component(count);

							struct Component
							{
								long x1{}, y1{}, x2{}, y2{};
								int pixels{};
								int id{};
							};

							std::vector<Component> components;
							std::vector<int> stack;
							stack.reserve(size_t(_theme.image.size * _theme.image.size));

							auto foreground = [&](long x, long y)
							{
								uint32_t p = pixels[y * w + x];
								int d0 = abs(int(p & 0xFF) - b0);
								int d1 = abs(int((p >> 8) & 0xFF) - b1);
								int d2 = abs(int((p >> 16) & 0xFF) - b2);
								int d = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
								return d > threshold;
							};

							int component_id = 0;
							for(auto y = zone_top; y < zone_bottom; y++)
							{
								for(auto x = zone_left; x < zone_right; x++)
								{
									auto start = int(y * w + x);
									if(component[start] || !foreground(x, y))
										continue;

									component_id++;
									Component c{ x, y, x, y, 0, component_id };
									component[start] = component_id;
									stack.clear();
									stack.push_back(start);

									while(!stack.empty())
									{
										auto pos = stack.back();
										stack.pop_back();
										auto px = pos % w;
										auto py = pos / w;

										c.pixels++;
										if(px < c.x1)
											c.x1 = px;
										if(py < c.y1)
											c.y1 = py;
										if(px > c.x2)
											c.x2 = px;
										if(py > c.y2)
											c.y2 = py;

										for(auto oy = -1; oy <= 1; oy++)
										{
											for(auto ox = -1; ox <= 1; ox++)
											{
												if(ox == 0 && oy == 0)
													continue;

												auto nx = px + ox;
												auto ny = py + oy;
												if(nx < zone_left || nx >= zone_right ||
												   ny < zone_top || ny >= zone_bottom)
													continue;

												auto np = int(ny * w + nx);
												if(component[np] || !foreground(nx, ny))
													continue;

												component[np] = component_id;
												stack.push_back(np);
											}
										}
									}

									components.push_back(c);
								}
							}

							auto image_area = int(_theme.image.size * _theme.image.size);
							long merged_x1 = zone_right;
							long merged_y1 = zone_bottom;
							long merged_x2 = zone_left;
							long merged_y2 = zone_top;
							int merged_pixels = 0;
							int accepted_components = 0;

							for(auto &c : components)
							{
								auto bw = c.x2 - c.x1 + 1;
								auto bh = c.y2 - c.y1 + 1;
								if(c.x1 >= icon_search_right)
									continue;
								if(c.pixels < image_area / 96)
									continue;
								if(c.pixels > image_area * 2)
									continue;
								if(bw > long(_theme.image.size * 2) ||
								   bh > long(_theme.image.size + _theme.image.gap))
									continue;

								accepted_components++;
								merged_pixels += c.pixels;
								if(c.x1 < merged_x1)
									merged_x1 = c.x1;
								if(c.y1 < merged_y1)
									merged_y1 = c.y1;
								if(c.x2 > merged_x2)
									merged_x2 = c.x2;
								if(c.y2 > merged_y2)
									merged_y2 = c.y2;
							}

							if(accepted_components > 0 && merged_pixels <= image_area * 2)
							{
								long bx1 = merged_x1;
								long by1 = merged_y1;
								long bx2 = merged_x2;
								long by2 = merged_y2;
								auto target_size = long(_theme.image.size);

								for(auto edge = 0; edge < 3; edge++)
								{
									if(bx1 > 0 && bx2 - bx1 + 1 < target_size)
										bx1--;
									if(by1 > 0 && by2 - by1 + 1 < target_size)
										by1--;
									if(bx2 + 1 < w && bx2 - bx1 + 1 < target_size)
										bx2++;
									if(by2 + 1 < h && by2 - by1 + 1 < target_size)
										by2++;
								}

								BITMAPINFO bmp{};
								bmp.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
								bmp.bmiHeader.biWidth = _theme.image.size;
								bmp.bmiHeader.biHeight = -int(_theme.image.size);
								bmp.bmiHeader.biPlanes = 1;
								bmp.bmiHeader.biBitCount = 32;
								bmp.bmiHeader.biCompression = BI_RGB;

								uint32_t *bits{};
								auto hicon = ::CreateDIBSection(nullptr, &bmp, DIB_RGB_COLORS,
																 reinterpret_cast<void **>(&bits),
																 nullptr, 0);
								if(hicon && bits)
								{
									std::fill(bits, bits + image_area, 0);

									auto bw = bx2 - bx1 + 1;
									auto bh = by2 - by1 + 1;
									auto target_width = bw;
									auto target_height = bh;

									if(target_width > target_size || target_height > target_size)
									{
										if(target_width >= target_height)
										{
											target_width = target_size;
											target_height = (bh * target_size) / bw;
										}
										else
										{
											target_height = target_size;
											target_width = (bw * target_size) / bh;
										}

										if(target_width < 1)
											target_width = 1;
										if(target_height < 1)
											target_height = 1;
									}

									auto dx0 = (target_size - target_width) / 2;
									auto dy0 = (target_size - target_height) / 2;

									int visible_pixels = 0;
									int colorful_pixels = 0;
									long long visible_luma = 0;
									long long visible_spread = 0;
									int min_luma = 255;
									int max_luma = 0;

									for(auto dy = 0; dy < target_height; dy++)
									{
										auto sy = by1 + (dy * bh) / target_height;

										for(auto dx = 0; dx < target_width; dx++)
										{
											auto sx = bx1 + (dx * bw) / target_width;
											auto dest_x = dx0 + dx;
											auto dest_y = dy0 + dy;

											uint32_t p = pixels[sy * w + sx];
											int pb = p & 0xFF;
											int pg = (p >> 8) & 0xFF;
											int pr = (p >> 16) & 0xFF;
											int d0 = abs(pb - b0);
											int d1 = abs(pg - b1);
											int d2 = abs(pr - b2);
											int d = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
											if(d <= threshold)
												continue;

											auto divisor = max_diff - threshold;
											if(divisor < 1)
												divisor = 1;

											int a = (d - threshold) * 255 / divisor;
											if(a < 0)
												a = 0;
											if(a > 255)
												a = 255;

											auto pre_r = pr * a / 255;
											auto pre_g = pg * a / 255;
											auto pre_b = pb * a / 255;
											bits[dest_y * _theme.image.size + dest_x] =
												(a << 24) | (pre_r << 16) | (pre_g << 8) | pre_b;

											if(a > 32)
											{
												auto maxc = pr > pg
													? (pr > pb ? pr : pb)
													: (pg > pb ? pg : pb);
												auto minc = pr < pg
													? (pr < pb ? pr : pb)
													: (pg < pb ? pg : pb);
												auto luma = (pr * 299 + pg * 587 + pb * 114) / 1000;
												auto spread = maxc - minc;
												visible_pixels++;
												visible_luma += luma;
												visible_spread += spread;
												if(luma < min_luma)
													min_luma = luma;
												if(luma > max_luma)
													max_luma = luma;
												if(spread > 48)
													colorful_pixels++;
											}
										}
									}

									if(light_icon_recolor_required(visible_pixels, colorful_pixels,
																   visible_luma, visible_spread,
																   min_luma, max_luma,
																   image_area))
										recolor_icon_bits(bits, image_area);

									mii->image.hbitmap = hicon;
									mii->image.size = { long(_theme.image.size), long(_theme.image.size) };
									mii->image.import = ImageImport::Image;
									image = &mii->image;
								}
							}
						}
					}
				}

				auto make_readable_light_bitmap = [&](HDC source, SIZE size) -> HBITMAP
				{
					if(!source || size.cx <= 0 || size.cy <= 0 || size.cx > 256 || size.cy > 256)
						return nullptr;

					BITMAPINFO bmp{};
					bmp.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
					bmp.bmiHeader.biWidth = size.cx;
					bmp.bmiHeader.biHeight = -int(size.cy);
					bmp.bmiHeader.biPlanes = 1;
					bmp.bmiHeader.biBitCount = 32;
					bmp.bmiHeader.biCompression = BI_RGB;

					uint32_t *bits{};
					auto_gdi<HBITMAP> hcopy(::CreateDIBSection(nullptr, &bmp, DIB_RGB_COLORS,
															   reinterpret_cast<void **>(&bits),
															   nullptr, 0));
					if(!hcopy || !bits)
						return nullptr;

					DC copyDC(::CreateCompatibleDC(dc), 1);
					if(!copyDC || !copyDC.select_bitmap(hcopy.get()))
						return nullptr;

					::BitBlt(copyDC, 0, 0, size.cx, size.cy, source, 0, 0, SRCCOPY);

					auto unpremultiply = [](int channel, int alpha)
					{
						if(alpha > 0 && alpha < 255 && channel <= alpha)
						{
							channel = channel * 255 / alpha;
							if(channel > 255)
								channel = 255;
						}
						return channel;
					};

					auto image_area = int(size.cx * size.cy);
					int visible_pixels = 0;
					int colorful_pixels = 0;
					long long visible_luma = 0;
					long long visible_spread = 0;
					int min_luma = 255;
					int max_luma = 0;

					for(auto i = 0; i < image_area; i++)
					{
						auto p = bits[i];
						auto a = int((p >> 24) & 0xFF);
						if(a <= 32)
							continue;

						auto pb = unpremultiply(int(p & 0xFF), a);
						auto pg = unpremultiply(int((p >> 8) & 0xFF), a);
						auto pr = unpremultiply(int((p >> 16) & 0xFF), a);
						auto maxc = pr > pg
							? (pr > pb ? pr : pb)
							: (pg > pb ? pg : pb);
						auto minc = pr < pg
							? (pr < pb ? pr : pb)
							: (pg < pb ? pg : pb);
						auto luma = (pr * 299 + pg * 587 + pb * 114) / 1000;
						auto spread = maxc - minc;

						visible_pixels++;
						visible_luma += luma;
						visible_spread += spread;
						if(luma < min_luma)
							min_luma = luma;
						if(luma > max_luma)
							max_luma = luma;
						if(spread > 48)
							colorful_pixels++;
					}

					auto recolor = light_icon_recolor_required(visible_pixels, colorful_pixels,
																visible_luma, visible_spread,
																min_luma, max_luma,
																image_area);
					if(recolor)
						recolor_icon_bits(bits, image_area);

					copyDC.reset_bitmap();
					return recolor ? hcopy.release() : nullptr;
				};

				if(image->hbitmap)
				{
					DC memDC(::CreateCompatibleDC(dc), 1);
					if(memDC)
					{
						if(memDC.select_bitmap(image->hbitmap))
						{
							Rect rcim = { rcimg.left + ((image_size - image->size.cx) / 2),
								(rcblock.top + (rcblock.bottom - image->size.cy)) / 2,
								image->size.cx, image->size.cy };
							
							//if(image->bitsPixel < 32)
							//	dc.bitblt({ rcim.left,rcim.top,size.cx,size.cy }, memDC, 0, 0);
							//else
							{
								bool is_16 = image->size.cx <= dpi(16) && image->size.cy <= dpi(16);
								auto draw_bitmap = [&](HDC hdc)
								{
									if(image->import == ImageImport::SVG && is_16)
										dc.draw_image(rcim.point(), image->size, hdc, state.disabled ? 48 : 192);
									dc.draw_image(rcim.point(), image->size, hdc, state.disabled ? 64 : 255);
								};

								auto_gdi<HBITMAP> readable_bitmap;
								if(mii->is_toplevel && image->import != ImageImport::SVG)
									readable_bitmap.reset(make_readable_light_bitmap(memDC, image->size));

								if(readable_bitmap)
								{
									DC readableDC(::CreateCompatibleDC(dc), 1);
									if(readableDC && readableDC.select_bitmap(readable_bitmap.get()))
									{
										draw_bitmap(readableDC);
										readableDC.reset_bitmap();
									}
									else
										draw_bitmap(memDC);
								}
								else
									draw_bitmap(memDC);
							}
							memDC.reset_bitmap();
						}
					}
				}
				else if(image->import == ImageImport::Draw)
				{
					auto draw = &image->draw;
					Color clr = text_color;
					SIZE size{};

					Color color_[2];
					color_[0] = text_color;

					if(draw->type == draw->DT_SHAPE)
					{
						auto shape = &draw->shape;
						size = shape->size;

						if(size.cx > 0 && size.cy > 0)
						{
							clr = shape->color[0];

							if(state.disabled) clr.opacities();

							if(size.cx > image_size) size.cx = image_size;
							if(size.cy > image_size) size.cy = image_size;

							if(shape->solid)
							{
								if(size.cx <= 3) size.cx = size.cx + 2;
								if(size.cy <= 3) size.cy = size.cy + 2;
							}
							else
							{
								if(size.cx == 1) size.cx = size.cx + 1;
								if(size.cy == 1) size.cy = size.cy + 1;
							}

							long left = rcimg.left + ((image_size - size.cx) / 2);
							if(mii->cch == 0)
							{
								left = rcimg.left + ((width - size.cx) / 2);
							}

							long top = rcimg.top + ((image_size - size.cy) / 2);
							draw_rect(&dc, { left, top}, size, clr);
						}
					}
					else if(draw->type == draw->DT_GLYPH)
					{
						if(mii->cch == 0)
						{
							rcimg = *rc;
							rcimg.left += 3;
							rcimg.right -= 3;
						}

						auto g = &draw->glyph;
						size = { g->size.cx, g->size.cy };
						if(size.cx > 0 && size.cy > 0 && g->font)
						{
							if(size.cx > image_size) size.cx = image_size;
							if(size.cy > image_size) size.cy = image_size;
							
							color_[0] = g->color[0];
							color_[1] = g->color[1];

							if(!color_[0])
							{
								if(_theme.image.color[0])
									color_[0] = _theme.image.color[0];
								else
								{
									if(state.disabled && (_theme.text.color.nor_dis))
										color_[0] = _theme.text.color.nor_dis;
									else
										color_[0] = text_color;
								}
							}

							if(!color_[1])
								color_[1] = _theme.image.color[0] ? _theme.image.color[1] : text_color;

							if(state.disabled)
							{
								color_[0].opacities();
								color_[1].opacities();
							}

							auto txtfmt = DT_NOCLIP | DT_SINGLELINE | DT_VCENTER;
							if(g->code[0])
								draw_string(dc, g->font, &rcimg, color_[0], &g->code[0], 1, DT_CENTER | txtfmt);

							if(g->code[1])
								draw_string(dc, g->font, &rcimg, color_[1], &g->code[1], 1, DT_CENTER | txtfmt);
						}
					}
				}
			}

			if(!mii->title.empty())
			{
				Color clrtext = text_color;

				if(!is_label && menu->draw.has_align())
				{
					rcText.left = rcblock.left + _theme.image.size + _theme.image.gap + _theme.back.padding.left;
					if(has_checked_image)
						rcText.left += _theme.image.size + _theme.image.gap;
				}
				else
				{
					rcText.left += _theme.back.padding.left;
				}

				rcText.right -= _theme.back.padding.right;

				if(mii->tab >= 0 && mii->is_popup())
					rcText.right -= _theme.image.size;

				auto txtfmt = DT_NOCLIP | DT_SINGLELINE | DT_VCENTER;

				if(_theme.text.prefix)
					txtfmt |= _theme.text.prefix;

				if(_hTheme)
				{
					rcText.top = rc->top - dpi(1);
					rcText.bottom = rc->bottom;

					if(mii->tab <= 0 && mii->keys.empty())
					{
						draw_string(dc, font.handle, &rcText, clrtext, mii->title, mii->title.length<int>(), (mii->tab < 0 ? DT_LEFT : DT_RIGHT) | txtfmt);
					}
					else
					{
						auto ds = [&](const string &left, const string &right)
						{
							if(!left.empty())
								draw_string(dc, font.handle, &rcText, clrtext, left, left.length<int>(), DT_LEFT | txtfmt);

							if(!right.empty())
							{
								Color c = clrtext;
								LOGFONTW lf{};
								std::memcpy(&lf, &_theme.font.lfHeight, sizeof lf);
								lf.lfHeight = long(lf.lfHeight * 0.80f);
								lf.lfWeight = FW_LIGHT;
								auto_gdi<HFONT> r_hfont(::CreateFontIndirectW(&lf));
								if(menu->id == IDENT_ID_INSERT_UNICODE_CONTROL_CHARACTER)
									c.opacity(state.disabled ? 50 : 100);
								else
									c.opacity(state.disabled ? 30 : 50);

								draw_string(dc, r_hfont.get(), &rcText, c, right, right.length<int>(), DT_RIGHT | txtfmt);
							}
						};

						if(mii->keys.empty())
						{
							string left = mii->title.text.substr(0, mii->tab).trim_end().move();
							string right = mii->title.text.substr(mii->tab).trim_start().move();
							ds(left, right);
						}
						else
						{
							ds(mii->title.text, mii->keys);
						}
					}
				}
			}
			
			// exlude menu item rectangle to prevent drawing by windows after us
			dc.exclude_clip_rect(*rc);

			if(state.selected && mii->tip)
				//	_tip.show(mii->tip, mii->rect);
				_tip.show(mii->tip.text, mii->tip.type, mii->tip.time, mii->rect);

			return TRUE;
		}

		LRESULT ContextMenu::OnMeasureItem(MEASUREITEMSTRUCT *mi)
		{
			LRESULT lret = 0;
			auto menu = current.menu;

			mi->itemHeight = 0;
			mi->itemWidth = 0;
			
			if(mi->itemID == 0x5ffffffe)
			{
				mi->itemWidth = 260;
				mi->itemHeight = 50;
			}
			else if(mi->itemID == MF_NOITEM)
			{
				mi->itemHeight = _theme.separator.margin.height() + _theme.separator.size;
			}
			else
			{
				mi->itemWidth = _theme.back.width();
				auto mii = get_item(mi->itemID, menu->handle, _items);
				if(mii)
				{
					if(mii->cch == 0)
					{
						if(!ident.equals(mi->itemID))
							lret = msg.invoke();
						else 
						{
							auto v = (uint32_t)_theme.view2;
							if(v < _theme.image.size)
								v = _theme.image.size;

							mi->itemWidth += v;
							if(mi->itemWidth >= (v*2))
							mi->itemWidth /= 2;
							mi->itemHeight += mii->is_spacer() ? dpi(10u) : v;
						}
					}
					else 
					{
						mi->itemHeight += mii->size.cy + _theme.back.height();
						if(mi->itemHeight % 2)
							mi->itemHeight++;

						mi->itemWidth += std::max<uint32_t>(menu->draw.length, mii->size.cx);

						// Remove extra space 'Submenu icon size'
						mi->itemWidth -= dpi.original(14);
					}
				}
			}

			if(_theme.layout.max_width > 0)
			{
				if(mi->itemWidth > _theme.layout.max_width)
					mi->itemWidth = _theme.layout.max_width;
			}

			if(_theme.layout.min_width > 0)
			{
				if(mi->itemWidth < _theme.layout.min_width)
					mi->itemWidth = _theme.layout.min_width;
			}

			return lret;
		}
	}
}
