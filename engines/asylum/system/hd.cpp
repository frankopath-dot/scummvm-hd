/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define FORBIDDEN_SYMBOL_EXCEPTION_getenv

#include "common/file.h"
#include "common/list.h"
#include "common/system.h"
#include "common/debug.h"
#include "common/events.h"
#include "common/textconsole.h"

#include "engines/util.h"

#include "graphics/cursorman.h"

#include "image/png.h"

#include "asylum/system/hd.h"
#include "asylum/system/graphics.h"

namespace Asylum {

// HD frames are kept as 0xRRGGBBAA
static const Graphics::PixelFormat kHdFormat(4, 8, 8, 8, 8, 24, 16, 8, 0);

// Upscaled art on disk is 4x the original
static const int kSourceScale = 4;

// Cache budget for decoded HD frames
static const uint64 kCacheBytes = 1536ULL * 1024 * 1024;

HdRenderer::HdRenderer(OSystem *system, const Common::FSNode &dir, int scale) :
	_system(system), _dir(dir), _scale(CLIP(scale, 2, 4)), _hasSnapshot(false), _gen(0), _cacheBytes(0), _presentCount(0), _dumpCount(0) {
	_top        = new Owner[kWidth * kHeight]();
	_under      = new Owner[kWidth * kHeight]();
	_snapTop    = new Owner[kWidth * kHeight]();
	_snapUnder  = new Owner[kWidth * kHeight]();
	_snapBuffer = new byte[kWidth * kHeight]();
	memset(_snapPalette, 0, sizeof(_snapPalette));

	_entries.push_back(Entry());
	_resolved.push_back(nullptr);
	_resolvedGen.push_back(0);
}

HdRenderer::~HdRenderer() {
	delete[] _top;
	delete[] _under;
	delete[] _snapTop;
	delete[] _snapUnder;
	delete[] _snapBuffer;

	for (Common::HashMap<uint64, HdFrame *>::iterator it = _cache.begin(); it != _cache.end(); ++it) {
		if (it->_value) {
			delete[] it->_value->pixels;
			delete it->_value;
		}
	}

	for (Common::HashMap<uint32, byte *>::iterator it = _packPalettes.begin(); it != _packPalettes.end(); ++it)
		delete[] it->_value;

	_out.free();
	_cursor.free();
}

bool HdRenderer::initMode() {
	Common::List<Graphics::PixelFormat> modes = _system->getSupportedFormats();
	for (Common::List<Graphics::PixelFormat>::iterator g = modes.begin(); g != modes.end(); ) {
		if (g->bytesPerPixel != 4)
			g = modes.erase(g);
		else
			++g;
	}

	if (modes.empty())
		return false;

	initGraphics(kWidth * _scale, kHeight * _scale, modes);
	if (_system->getScreenFormat().bytesPerPixel != 4)
		return false;

	if (_format != _system->getScreenFormat() || !_out.getPixels()) {
		_format = _system->getScreenFormat();
		_out.free();
		_out.create(kWidth * _scale, kHeight * _scale, _format);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// Recording
//////////////////////////////////////////////////////////////////////////

uint16 HdRenderer::entryFor(ResourceId id, uint32 frameIndex) {
	uint64 k = key(id, frameIndex);
	if (_entryIds.contains(k))
		return _entryIds[k];

	if (_entries.size() >= 0xFFFF) {
		// Out of ids: forget everything that refers to them
		_entries.resize(1);
		_resolved.resize(1);
		_resolvedGen.resize(1);
		_entryIds.clear();
		invalidateAll();
		_hasSnapshot = false;
	}

	Entry e;
	e.id = id;
	e.frameIndex = frameIndex;
	_entries.push_back(e);
	_resolved.push_back(nullptr);
	_resolvedGen.push_back(0);

	uint16 n = (uint16)(_entries.size() - 1);
	_entryIds[k] = n;
	return n;
}

void HdRenderer::recordDraw(ResourceId id, uint32 frameIndex, const GraphicFrame *frame, const Common::Rect &src, const Common::Rect &dstRect, bool colorKey, const byte *backBuffer) {
	const Graphics::Surface &s = frame->surface;
	Common::Rect dst = dstRect;
	dst.clip(Common::Rect(kWidth, kHeight));
	if (!s.getPixels() || dst.isEmpty() || !src.isValidRect())
		return;

	int16 w = dst.width(), h = dst.height();
	int16 offX = dst.left - dstRect.left, offY = dst.top - dstRect.top;

	// The blitters walk the source in slightly different ways (plain, mirrored
	// from right or right - 1, ...): pick the mapping that explains what landed
	// in the back buffer.
	const int16 starts[4] = { src.left, (int16)(src.right - 1), src.right, (int16)(src.right - 2) };
	const int16 dirs[4]   = { 1, -1, -1, -1 };

	int best = -1, bestMatch = 0;
	int16 step = (int16)MAX<int>(1, (int)sqrt((double)(w * h) / 4096.0));
	for (int m = 0; m < 4; m++) {
		int total = 0, match = 0;
		for (int16 y = 0; y < h; y += step) {
			int16 sy = src.top + offY + y;
			if (sy < 0 || sy >= s.h)
				continue;
			const byte *row = (const byte *)s.getBasePtr(0, sy);
			for (int16 x = 0; x < w; x += step) {
				int sx = starts[m] + dirs[m] * (offX + x);
				if (sx < 0 || sx >= s.w)
					continue;
				byte p = row[sx];
				if (colorKey && !p)
					continue;
				total++;
				if (backBuffer[(dst.top + y) * kWidth + dst.left + x] == p)
					match++;
			}
		}
		if (match > bestMatch) {
			best = m;
			bestMatch = match;
		}
		if (total && match == total)
			break;
	}

	if (best < 0)
		return;

	uint16 entry = entryFor(id, frameIndex);
	for (int16 y = 0; y < h; y++) {
		int16 sy = src.top + offY + y;
		if (sy < 0 || sy >= s.h)
			continue;
		const byte *row = (const byte *)s.getBasePtr(0, sy);
		int p0 = (dst.top + y) * kWidth + dst.left;
		for (int16 x = 0; x < w; x++) {
			int sx = starts[best] + dirs[best] * (offX + x);
			if (sx < 0 || sx >= s.w)
				continue;
			byte p = row[sx];
			if (colorKey && !p)
				continue;
			if (backBuffer[p0 + x] != p)
				continue;
			Owner &o = _top[p0 + x];
			_under[p0 + x] = o;
			o.entry = entry;
			o.sx = (uint16)sx;
			o.sy = (uint16)sy;
			o.index = p;
			o.flags = dirs[best] < 0 ? kFlagMirrored : 0;
		}
	}
}

void HdRenderer::recordTranslucent(const GraphicFrame *frame, const Common::Rect &src, const Common::Rect &dstRect, bool mirrored, const byte *backBuffer) {
	const Graphics::Surface &s = frame->surface;
	Common::Rect dst = dstRect;
	dst.clip(Common::Rect(kWidth, kHeight));
	if (!s.getPixels() || dst.isEmpty())
		return;

	int16 offX = dst.left - dstRect.left, offY = dst.top - dstRect.top;
	for (int16 y = 0; y < dst.height(); y++) {
		int16 sy = src.top + offY + y;
		if (sy < 0 || sy >= s.h)
			continue;
		const byte *row = (const byte *)s.getBasePtr(0, sy);
		int p0 = (dst.top + y) * kWidth + dst.left;
		for (int16 x = 0; x < dst.width(); x++) {
			int sx = mirrored ? src.right - 1 - (offX + x) : src.left + offX + x;
			if (sx < 0 || sx >= s.w || !row[sx])
				continue;
			Owner &o = _top[p0 + x];
			if (o.entry && backBuffer[p0 + x] == o.index)
				o.flags |= kFlagTranslucent;
		}
	}
}

void HdRenderer::invalidate(const Common::Rect &rect) {
	Common::Rect r = rect;
	r.clip(Common::Rect(kWidth, kHeight));
	if (r.isEmpty())
		return;

	for (int16 y = r.top; y < r.bottom; y++) {
		memset((void *)&_top[y * kWidth + r.left], 0, sizeof(Owner) * r.width());
		memset((void *)&_under[y * kWidth + r.left], 0, sizeof(Owner) * r.width());
	}
}

void HdRenderer::invalidateAll() {
	invalidate(Common::Rect(kWidth, kHeight));
}

//////////////////////////////////////////////////////////////////////////
// HD frames
//////////////////////////////////////////////////////////////////////////

const byte *HdRenderer::readPalette(uint32 key, const Common::FSNode &node) {
	if (_packPalettes.contains(key))
		return _packPalettes[key];

	byte *pal = nullptr;
	Common::File f;
	if (node.exists() && f.open(node) && f.size() == 256 * 3) {
		pal = new byte[256 * 3];
		f.read(pal, 256 * 3);
	}

	_packPalettes[key] = pal;
	return pal;
}

const byte *HdRenderer::resourcePalette(uint32 pack, uint16 index) {
	// Resources shown with another palette than their scene's were extracted with it
	Common::FSNode packDir = _dir.getChild(Common::String::format("RES.%03d", pack));
	const byte *pal = readPalette((pack << 16) | index, packDir.getChild(Common::String::format("%04d.pal", index)));
	if (pal)
		return pal;

	return readPalette((pack << 16) | 0xFFFF, packDir.getChild("_palette.bin"));
}

HdRenderer::HdFrame *HdRenderer::load(ResourceId id, uint32 frameIndex) {
	uint32 pack = RESOURCE_PACK(id);
	Common::FSNode node = _dir.getChild(Common::String::format("RES.%03d", pack))
	                          .getChild(Common::String::format("%04d_%03d.png", RESOURCE_INDEX(id), frameIndex));
	if (!node.exists())
		return nullptr;

	uint32 start = _system->getMillis();
	Common::File f;
	Image::PNGDecoder dec;
	if (!f.open(node) || !dec.loadStream(f)) {
		warning("HD: cannot load %s", node.getPath().toString().c_str());
		return nullptr;
	}

	const Graphics::Surface *png = dec.getSurface();
	Graphics::Surface *src = png->convertTo(kHdFormat, dec.getPalette().data(), dec.getPalette().size());
	if (!src)
		return nullptr;

	int ow = src->w / kSourceScale, oh = src->h / kSourceScale;
	HdFrame *hd = new HdFrame();
	hd->w = ow * _scale;
	hd->h = oh * _scale;
	hd->pixels = new uint32[MAX(1, hd->w * hd->h)];
	hd->palette = resourcePalette(pack, RESOURCE_INDEX(id));
	hd->lastUse = _gen;
	hd->bytes = (uint32)(hd->w * hd->h * 4);

	if (_scale == kSourceScale) {
		for (int y = 0; y < hd->h; y++)
			memcpy(hd->pixels + y * hd->w, src->getBasePtr(0, y), hd->w * 4);
	} else {
		// Area-average down to our scale (alpha-weighted so transparent pixels don't bleed)
		for (int y = 0; y < hd->h; y++) {
			int y0 = y * kSourceScale * 256 / _scale, y1 = (y + 1) * kSourceScale * 256 / _scale;
			for (int x = 0; x < hd->w; x++) {
				int x0 = x * kSourceScale * 256 / _scale, x1 = (x + 1) * kSourceScale * 256 / _scale;
				uint64 r = 0, g = 0, b = 0, a = 0, wsum = 0;
				for (int sy = y0 >> 8; sy < (y1 + 255) >> 8 && sy < src->h; sy++) {
					int wy = MIN(y1, (sy + 1) * 256) - MAX(y0, sy * 256);
					const uint32 *row = (const uint32 *)src->getBasePtr(0, sy);
					for (int sx = x0 >> 8; sx < (x1 + 255) >> 8 && sx < src->w; sx++) {
						int wx = MIN(x1, (sx + 1) * 256) - MAX(x0, sx * 256);
						uint32 p = row[sx];
						uint64 wgt = (uint64)(wx * wy);
						uint64 pa = (p & 0xFF) * wgt;
						r += (p >> 24) * pa;
						g += ((p >> 16) & 0xFF) * pa;
						b += ((p >> 8) & 0xFF) * pa;
						a += pa;
						wsum += wgt;
					}
				}
				uint32 out = 0;
				if (a) {
					out = (uint32)(r / a) << 24 | (uint32)(g / a) << 16 | (uint32)(b / a) << 8 | (uint32)(wsum ? a / wsum : 0);
				}
				hd->pixels[y * hd->w + x] = out;
			}
		}
	}

	src->free();
	delete src;

	debug(2, "HD: loaded %s (%dx%d) in %u ms", node.getName().c_str(), hd->w, hd->h, _system->getMillis() - start);

	_cacheBytes += hd->bytes;
	return hd;
}

HdRenderer::HdFrame *HdRenderer::frameFor(uint16 entry) {
	if (_resolvedGen[entry] == _gen)
		return _resolved[entry];

	const Entry &e = _entries[entry];
	uint64 k = key(e.id, e.frameIndex);
	HdFrame *hd;
	if (_cache.contains(k)) {
		hd = _cache[k];
	} else {
		hd = load(e.id, e.frameIndex);
		_cache[k] = hd;
	}

	if (hd)
		hd->lastUse = _gen;

	_resolved[entry] = hd;
	_resolvedGen[entry] = _gen;
	return hd;
}

void HdRenderer::evict() {
	if (_cacheBytes <= kCacheBytes)
		return;

	// Drop the least recently used frames until we are well under budget
	Common::Array<uint64> keys;
	for (Common::HashMap<uint64, HdFrame *>::iterator it = _cache.begin(); it != _cache.end(); ++it)
		if (it->_value && it->_value->lastUse != _gen)
			keys.push_back(it->_key);

	Common::sort(keys.begin(), keys.end(), [this](uint64 a, uint64 b) { return _cache[a]->lastUse < _cache[b]->lastUse; });

	for (uint i = 0; i < keys.size() && _cacheBytes > kCacheBytes * 3 / 4; i++) {
		HdFrame *hd = _cache[keys[i]];
		_cacheBytes -= hd->bytes;
		delete[] hd->pixels;
		delete hd;
		_cache.erase(keys[i]);
	}
}

//////////////////////////////////////////////////////////////////////////
// Output
//////////////////////////////////////////////////////////////////////////

void HdRenderer::present(const byte *backBuffer, const byte *palette) {
	memcpy(_snapBuffer, backBuffer, kWidth * kHeight);
	memcpy((void *)_snapTop, _top, sizeof(Owner) * kWidth * kHeight);
	memcpy((void *)_snapUnder, _under, sizeof(Owner) * kWidth * kHeight);
	_hasSnapshot = true;

	compose(_snapBuffer, _snapTop, _snapUnder, palette);
	dump(_snapBuffer, palette);
}

int HdRenderer::testVideo() {
	const char *video = getenv("ASYLUM_HD_VIDEO");
	return video ? atoi(video) : -1;
}

int HdRenderer::testScene() {
	const char *scene = getenv("ASYLUM_HD_SCENE");
	return scene ? atoi(scene) : -1;
}

void HdRenderer::dump(const byte *buffer, const byte *palette) {
	const char *dir = getenv("ASYLUM_HD_DUMP");
	if (!dir)
		return;

	uint32 every = getenv("ASYLUM_HD_DUMP_EVERY") ? (uint32)atoi(getenv("ASYLUM_HD_DUMP_EVERY")) : 30;
	uint32 max   = getenv("ASYLUM_HD_DUMP_MAX")   ? (uint32)atoi(getenv("ASYLUM_HD_DUMP_MAX"))   : 20;
	if (_presentCount++ % MAX<uint32>(1, every))
		return;

	Common::DumpFile f;
	if (f.open(Common::Path(Common::String::format("%s/hd_%03d.png", dir, _dumpCount), '/')))
		Image::writePNG(f, _out);
	f.close();

	Graphics::Surface orig;
	orig.init(kWidth, kHeight, kWidth, const_cast<byte *>(buffer), Graphics::PixelFormat::createFormatCLUT8());
	if (f.open(Common::Path(Common::String::format("%s/orig_%03d.png", dir, _dumpCount), '/')))
		Image::writePNG(f, orig, palette);
	f.close();

	// Why each pixel looks the way it does: black = nobody claimed it, red = claimed but
	// overwritten since, yellow = no HD frame, green = HD, blue = HD through translucency
	static const byte why[] = { 0, 0, 0,  255, 0, 0,  255, 255, 0,  0, 200, 0,  0, 80, 255 };
	Graphics::Surface reasons;
	reasons.create(kWidth, kHeight, Graphics::PixelFormat::createFormatCLUT8());
	for (int p = 0; p < kWidth * kHeight; p++) {
		const Owner &o = _snapTop[p];
		byte r = 0;
		if (o.entry) {
			if (o.index != buffer[p] && !(o.flags & kFlagTranslucent))
				r = 1;
			else if (!frameFor(o.entry))
				r = 2;
			else
				r = (o.flags & kFlagTranslucent) ? 4 : 3;
		}
		((byte *)reasons.getPixels())[p] = r;
	}
	if (f.open(Common::Path(Common::String::format("%s/why_%03d.png", dir, _dumpCount), '/')))
		Image::writePNG(f, reasons, why, 5);
	f.close();
	reasons.free();

	if (++_dumpCount >= max) {
		Common::Event ev;
		ev.type = Common::EVENT_QUIT;
		_system->getEventManager()->pushEvent(ev);
	}
}

void HdRenderer::paletteChanged(const byte *palette) {
	if (_hasSnapshot)
		compose(_snapBuffer, _snapTop, _snapUnder, palette);
}

// Fixed point 8.8 ratio between a live and an extraction color component
static inline int ratio(byte live, byte orig) {
	return ((live + 2) << 8) / (orig + 2);
}

static inline byte tint(uint32 c, int r) {
	return (byte)MIN<uint32>(255, (c * r) >> 8);
}

void HdRenderer::compose(const byte *buffer, const Owner *top, const Owner *under, const byte *palette) {
	uint32 start = _system->getMillis();
	++_gen;
	memcpy(_snapPalette, palette, sizeof(_snapPalette));

	const int s = _scale;
	const int W = kWidth * s;
	uint32 *out = (uint32 *)_out.getPixels();

	for (int y = 0; y < kHeight; y++) {
		for (int x = 0; x < kWidth; x++) {
			int p = y * kWidth + x;
			byte idx = buffer[p];
			const byte *live = palette + idx * 3;
			uint32 *dst = out + (y * s) * W + x * s;

			const Owner &o = top[p];
			HdFrame *f = nullptr;
			if (o.entry && (o.index == idx || (o.flags & kFlagTranslucent)))
				f = frameFor(o.entry);

			if (!f) {
				uint32 c = _format.RGBToColor(live[0], live[1], live[2]);
				for (int j = 0; j < s; j++)
					for (int i = 0; i < s; i++)
						dst[j * W + i] = c;
				continue;
			}

			int rr = 256, rg = 256, rb = 256;
			if (f->palette) {
				const byte *orig = f->palette + o.index * 3;
				rr = ratio(live[0], orig[0]);
				rg = ratio(live[1], orig[1]);
				rb = ratio(live[2], orig[2]);
			}

			// Layer below, for the HD frame's soft or transparent edges
			const Owner &u = under[p];
			HdFrame *uf = nullptr;
			bool ufTried = false;
			int ur = 256, ug = 256, ub = 256;

			for (int j = 0; j < s; j++) {
				int hy = MIN(o.sy * s + j, f->h - 1);
				const uint32 *row = f->pixels + hy * f->w;
				for (int i = 0; i < s; i++) {
					uint32 c = row[MIN(o.sx * s + ((o.flags & kFlagMirrored) ? s - 1 - i : i), f->w - 1)];
					uint32 a = c & 0xFF;
					byte r = tint(c >> 24, rr), g = tint((c >> 16) & 0xFF, rg), b = tint((c >> 8) & 0xFF, rb);

					if (a < 255) {
						if (!ufTried) {
							ufTried = true;
							if (u.entry)
								uf = frameFor(u.entry);
							if (uf && uf->palette) {
								const byte *orig = uf->palette + u.index * 3;
								const byte *ulive = palette + u.index * 3;
								ur = ratio(ulive[0], orig[0]);
								ug = ratio(ulive[1], orig[1]);
								ub = ratio(ulive[2], orig[2]);
							}
						}

						byte br, bg, bb;
						if (uf) {
							int ui = (u.flags & kFlagMirrored) ? s - 1 - i : i;
							uint32 uc = uf->pixels[MIN(u.sy * s + j, uf->h - 1) * uf->w + MIN(u.sx * s + ui, uf->w - 1)];
							br = tint(uc >> 24, ur);
							bg = tint((uc >> 16) & 0xFF, ug);
							bb = tint((uc >> 8) & 0xFF, ub);
						} else {
							br = live[0];
							bg = live[1];
							bb = live[2];
						}

						r = (byte)((r * a + br * (255 - a)) / 255);
						g = (byte)((g * a + bg * (255 - a)) / 255);
						b = (byte)((b * a + bb * (255 - a)) / 255);
					}

					dst[j * W + i] = _format.RGBToColor(r, g, b);
				}
			}
		}
	}

	_system->copyRectToScreen(_out.getPixels(), _out.pitch, 0, 0, _out.w, _out.h);
	evict();

	debug(3, "HD: composed in %u ms", _system->getMillis() - start);
}

void HdRenderer::fillScreen(const Common::Rect &rect, uint32 color) {
	const byte *c = _snapPalette + (color & 0xFF) * 3;
	_system->fillScreen(Common::Rect(rect.left * _scale, rect.top * _scale, rect.right * _scale, rect.bottom * _scale),
	                    _format.RGBToColor(c[0], c[1], c[2]));
}

void HdRenderer::copyScaled(const Graphics::Surface &surface, int x, int y, const byte *palette) {
	Graphics::Surface *conv = nullptr;
	const Graphics::Surface *src = &surface;
	if (surface.format != _format) {
		conv = surface.convertTo(_format, palette);
		src = conv;
	}

	const int s = _scale;
	Graphics::Surface scaled;
	scaled.create(src->w * s, src->h * s, _format);
	for (int sy = 0; sy < src->h; sy++) {
		const uint32 *in = (const uint32 *)src->getBasePtr(0, sy);
		for (int j = 0; j < s; j++) {
			uint32 *o = (uint32 *)scaled.getBasePtr(0, sy * s + j);
			for (int sx = 0; sx < src->w; sx++)
				for (int i = 0; i < s; i++)
					*o++ = in[sx];
		}
	}

	Common::Rect r(x * s, y * s, x * s + scaled.w, y * s + scaled.h);
	Common::Rect clipped = r.findIntersectingRect(Common::Rect(_out.w, _out.h));
	if (!clipped.isEmpty())
		_system->copyRectToScreen(scaled.getBasePtr(clipped.left - r.left, clipped.top - r.top), scaled.pitch,
		                          clipped.left, clipped.top, clipped.width(), clipped.height());

	if (surface.h >= 160)
		dumpVideo(scaled);

	scaled.free();
	if (conv) {
		conv->free();
		delete conv;
	}
}

void HdRenderer::copyFitted(const Graphics::Surface &frame, const Common::Rect &rect) {
	const int s = _scale;
	Common::Rect target(rect.left * s, rect.top * s, rect.right * s, rect.bottom * s);
	target.clip(Common::Rect(_out.w, _out.h));
	if (target.isEmpty() || frame.format.bytesPerPixel != 4)
		return;

	const Graphics::Surface *src = &frame;
	Graphics::Surface resized;
	if (frame.w != rect.width() * s || frame.h != rect.height() * s) {
		// Bilinear resample to our scale (HD movies are made for one scale)
		const int tw = rect.width() * s, th = rect.height() * s;
		resized.create(tw, th, _format);
		for (int y = 0; y < th; y++) {
			int fy = MAX(0, ((2 * y + 1) * frame.h * 128) / th - 128);
			int y0 = MIN(fy >> 8, frame.h - 1), y1 = MIN(y0 + 1, frame.h - 1), wy = fy & 0xFF;
			const uint32 *r0 = (const uint32 *)frame.getBasePtr(0, y0), *r1 = (const uint32 *)frame.getBasePtr(0, y1);
			uint32 *o = (uint32 *)resized.getBasePtr(0, y);
			for (int x = 0; x < tw; x++) {
				int fx = MAX(0, ((2 * x + 1) * frame.w * 128) / tw - 128);
				int x0 = MIN(fx >> 8, frame.w - 1), x1 = MIN(x0 + 1, frame.w - 1), wx = fx & 0xFF;
				uint32 out = 0;
				for (int sh = 0; sh < 32; sh += 8) {
					int a = (r0[x0] >> sh) & 0xFF, b = (r0[x1] >> sh) & 0xFF, c = (r1[x0] >> sh) & 0xFF, d = (r1[x1] >> sh) & 0xFF;
					int top = a * (256 - wx) + b * wx, bottom = c * (256 - wx) + d * wx;
					out |= (uint32)(((top * (256 - wy) + bottom * wy) >> 16) & 0xFF) << sh;
				}
				o[x] = out;
			}
		}
		src = &resized;
	}

	int ox = target.left - rect.left * s, oy = target.top - rect.top * s;
	_system->copyRectToScreen(src->getBasePtr(ox, oy), src->pitch, target.left, target.top, target.width(), target.height());
	dumpVideo(*src);
	resized.free();
}

void HdRenderer::dumpVideo(const Graphics::Surface &frame) {
	// Testing aid: save every 30th movie frame as shown
	static uint32 videoFrames = 0;
	if (!getenv("ASYLUM_HD_DUMP") || videoFrames++ % 30)
		return;

	Common::DumpFile f;
	if (f.open(Common::Path(Common::String::format("%s/vid_%03d.png", getenv("ASYLUM_HD_DUMP"), videoFrames / 30), '/')))
		Image::writePNG(f, frame);
}

Common::SeekableReadStream *HdRenderer::openVideo(const Common::String &name) {
	Common::FSNode vids = _dir.getChild("Vids");
	Common::FSList files;
	if (!vids.isDirectory() || !vids.getChildren(files, Common::FSNode::kListFilesOnly))
		return nullptr;

	for (Common::FSList::const_iterator f = files.begin(); f != files.end(); ++f) {
		if (f->getName().equalsIgnoreCase(name)) {
			debug(1, "HD: playing %s", f->getPath().toString().c_str());
			return f->createReadStream();
		}
	}

	return nullptr;
}

void HdRenderer::setCursor(ResourceId id, uint32 frameIndex, const GraphicFrame *frame, const Common::Point &hotspot, const byte *palette) {
	const Graphics::Surface &s = frame->surface;
	const int sc = _scale;

	++_gen;
	HdFrame *hd = frameFor(entryFor(id, frameIndex));

	_cursor.free();
	_cursor.create(s.w * sc, s.h * sc, _format);

	// Key color: something the art is unlikely to use
	const uint32 key = _format.RGBToColor(255, 0, 255);
	const uint32 nearKey = _format.RGBToColor(254, 0, 254);

	for (int y = 0; y < s.h * sc; y++) {
		uint32 *o = (uint32 *)_cursor.getBasePtr(0, y);
		const byte *row = (const byte *)s.getBasePtr(0, y / sc);
		for (int x = 0; x < s.w * sc; x++) {
			byte idx = row[x / sc];
			uint32 c;
			if (hd) {
				uint32 p = hd->pixels[MIN(y, hd->h - 1) * hd->w + MIN(x, hd->w - 1)];
				c = (p & 0xFF) < 128 ? key : _format.RGBToColor(p >> 24, (p >> 16) & 0xFF, (p >> 8) & 0xFF);
			} else {
				c = idx ? _format.RGBToColor(palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2]) : key;
			}
			if (c == key && (hd ? (hd->pixels[MIN(y, hd->h - 1) * hd->w + MIN(x, hd->w - 1)] & 0xFF) >= 128 : idx != 0))
				c = nearKey;
			o[x] = c;
		}
	}

	CursorMan.replaceCursor(_cursor.getPixels(), _cursor.w, _cursor.h, hotspot.x * sc, hotspot.y * sc, key, false, &_format);
}

} // End of namespace Asylum
