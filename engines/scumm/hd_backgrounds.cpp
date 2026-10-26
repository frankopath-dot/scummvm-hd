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

#include "common/debug.h"
#include "common/file.h"
#include "common/stream.h"
#include "common/system.h"
#include "graphics/yuv_to_rgb.h"
#include "image/png.h"

#ifdef USE_VPX
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#endif

#include "scumm/hd_backgrounds.h"

namespace Scumm {

bool HdBackgrounds::HdImage::load(const Common::FSNode &hdFile, const Common::FSNode &origFile, byte *palette) {
	free();
	if (!hdFile.exists() || !origFile.exists())
		return false;

	Image::PNGDecoder origDec, hdDec;
	Common::SeekableReadStream *s = origFile.createReadStream();
	bool ok = s && origDec.loadStream(*s);
	delete s;
	const Graphics::Surface *o = ok ? origDec.getSurface() : nullptr;
	if (!o || o->format.bytesPerPixel != 1) {
		warning("HD backgrounds: %s is not an indexed PNG", origFile.getName().c_str());
		return false;
	}
	s = hdFile.createReadStream();
	ok = s && hdDec.loadStream(*s);
	delete s;
	const Graphics::Surface *h2 = ok ? hdDec.getSurface() : nullptr;
	if (!h2 || h2->w != o->w * kScale || h2->h != o->h * kScale) {
		warning("HD backgrounds: %s is not %dx the size of the original", hdFile.getName().c_str(), kScale);
		return false;
	}

	w = o->w;
	h = o->h;
	orig = new byte[w * h];
	for (int y = 0; y < h; ++y)
		memcpy(orig + y * w, o->getBasePtr(0, y), w);
	if (palette) {
		memset(palette, 0, 3 * 256);
		const Graphics::Palette &pal = origDec.getPalette();
		pal.grab(palette, 0, MIN<uint>(pal.size(), 256));
	}
	hd = h2->convertTo(Graphics::PixelFormat::createFormatRGBA32(false));
	return true;
}

void HdBackgrounds::HdImage::free() {
	if (hd) {
		hd->free();
		delete hd;
		hd = nullptr;
	}
	delete[] orig;
	orig = nullptr;
	w = h = 0;
}

struct HdBackgrounds::Video {
	Common::SeekableReadStream *stream = nullptr;
	int headerSize = 32;
	int frame = -1;             // last decoded frame
	bool show = false;          // the decoded frame belongs to the frame on screen
	Common::Array<bool> noHd;   // frames left to the original pixels
	Graphics::Surface image;    // last decoded frame in the screen format
	byte *clean = nullptr;      // the SMUSH frame before text was drawn on it
	bool haveClean = false;
	Common::Array<byte> data;
#ifdef USE_VPX
	vpx_codec_ctx_t codec;
	bool codecOk = false;

	bool initCodec() {
		if (codecOk)
			vpx_codec_destroy(&codec);
		vpx_codec_dec_cfg_t cfg = {};
		cfg.threads = 4;
		codecOk = vpx_codec_dec_init(&codec, &vpx_codec_vp9_dx_algo, &cfg, 0) == VPX_CODEC_OK;
		return codecOk;
	}
#endif

	~Video() {
#ifdef USE_VPX
		if (codecOk)
			vpx_codec_destroy(&codec);
#endif
		delete stream;
		image.free();
		delete[] clean;
	}
};

HdBackgrounds::HdBackgrounds(OSystem *system, const Common::FSNode &dir, int width, int height)
	: _system(system), _dir(dir), _w(width), _h(height), _room(-1), _screenRoom(-1), _cursor(nullptr), _video(nullptr) {
	_format = _system->getScreenFormat();
	_out.create(_w * kScale, _h * kScale, _format);
	_screen = new byte[_w * _h]();
	_back = new byte[_w * _h]();
	_why = new byte[_w * _h]();
	_roomPos = new int16[_w * _h * 2];
	_screenOwners.resize(_w * _h);
	for (int i = 0; i < _w * _h * 2; ++i)
		_roomPos[i] = -1;
	memset(_palette, 0, sizeof(_palette));
	memset(_origPalette, 0, sizeof(_origPalette));
	for (int i = 0; i < 256; ++i) {
		_palette32[i] = _format.RGBToColor(0, 0, 0);
		_tintOk[i] = false;
		_cycled[i] = false;
	}
}

HdBackgrounds::~HdBackgrounds() {
	stopVideo();
	_out.free();
	_bg.free();
	clearObjects();
	delete[] _screen;
	delete[] _back;
	delete[] _why;
	delete[] _roomPos;
	for (HdCel &c : _cels)
		c.free();
	delete[] _cursor;
}

Common::FSNode HdBackgrounds::findRoomFile(const Common::FSNode &dir, int room) const {
	Common::FSList files;
	if (!dir.getChildren(files, Common::FSNode::kListFilesOnly))
		return Common::FSNode();
	Common::String prefix = Common::String::format("%04d_", room);
	for (const Common::FSNode &f : files) {
		Common::String name = f.getName();
		if (name.hasPrefix(prefix) && name.hasSuffixIgnoreCase(".png"))
			return f;
	}
	return Common::FSNode();
}

void HdBackgrounds::clearObjects() {
	for (HdObject *o : _objects) {
		o->image.free();
		delete o;
	}
	_objects.clear();
}

void HdBackgrounds::loadRoom(int room) {
	_bg.free();
	clearObjects();
	_room = room;	// remember the attempt, so a missing room is only looked up once

	if (!_bg.load(findRoomFile(_dir, room), findRoomFile(_dir.getChild("original"), room), _origPalette)) {
		debug(1, "HD backgrounds: no HD image for room %d", room);
		return;
	}
	debug(1, "HD backgrounds: loaded room %d (%dx%d)", room, _bg.w, _bg.h);
	updateTints();
}

void HdBackgrounds::objectDrawn(int room, int obj, int state, int x, int y, int w, int h) {
	if (room != _room)
		loadRoom(room);
	if (!_bg.hd)
		return;

	for (uint i = 0; i < _objects.size(); ++i) {
		HdObject *o = _objects[i];
		if (o->obj != obj)
			continue;
		if (o->state == state && o->x == x && o->y == y)
			return;
		o->image.free();
		delete o;
		_objects.remove_at(i);
		break;
	}

	HdObject *o = new HdObject();
	o->obj = obj;
	o->state = state;
	o->x = x;
	o->y = y;
	Common::String name = Common::String::format("%04d_%04d_IM%02X.png", room, obj, state);
	Common::FSNode objDir = _dir.getChild("objects");
	if (o->image.load(objDir.getChild(name), objDir.getChild("original").getChild(name), nullptr) &&
			(o->image.w != w || o->image.h != h)) {
		warning("HD backgrounds: %s is %dx%d, object is %dx%d", name.c_str(), o->image.w, o->image.h, w, h);
		o->image.free();
	}
	debug(1, "HD backgrounds: object %s at %d,%d%s", name.c_str(), x, y, o->image.hd ? "" : " (no HD image)");
	_objects.push_back(o);
}

void HdBackgrounds::setCycledColors(const byte *used) {
	for (int i = 0; i < 256; ++i)
		_cycled[i] = used[i] != 0;
	if (_bg.hd)
		updateTints();
}

// Per palette index, find the per-channel factor that turns the room's
// original color into the current one, so fades, lighting changes and flashes
// (the power room's lightning turns whole blue ramps into one light blue)
// carry over to the HD image. Color-cycled indices are recolored the same way,
// which animates them in HD. Indices the factor can't reach (a channel more
// than 4x brighter) are left to the original pixels, as is the (171, 0, 171)
// placeholder color The Dig uses for areas filled at runtime.
void HdBackgrounds::updateTints() {
	for (int i = 0; i < 256; ++i) {
		const byte *o = _origPalette + i * 3;
		const byte *c = _palette + i * 3;
		bool reachable = true;
		for (int ch = 0; ch < 3; ++ch) {
			const uint k = (c[ch] + 4) * 256 / (o[ch] + 4);
			reachable &= k <= 1024;
			_tint[i][ch] = MIN<uint>(k, 1024);
		}
		const bool placeholder = o[0] == 171 && o[1] == 0 && o[2] == 171;
		_tintOk[i] = !placeholder && (_cycled[i] || reachable);
	}
}

void HdBackgrounds::setPalette(const byte *colors, uint first, uint num) {
	if (!memcmp(_palette + first * 3, colors, num * 3))
		return;
	memcpy(_palette + first * 3, colors, num * 3);
	for (uint i = first; i < first + num; ++i)
		_palette32[i] = _format.RGBToColor(_palette[i * 3], _palette[i * 3 + 1], _palette[i * 3 + 2]);
	if (_bg.hd)
		updateTints();
	render(0, 0, _w, _h);
}

void HdBackgrounds::blit(const byte *src, int pitch, int x, int y, int w, int h, int room, int roomX, int roomY, const byte *back) {
	if (x < 0) { w += x; src -= x; if (back) back -= x; if (roomX >= 0) roomX -= x; x = 0; }
	if (y < 0) { h += y; src -= y * pitch; if (back) back -= y * pitch; if (roomY >= 0) roomY -= y; y = 0; }
	w = MIN(w, _w - x);
	h = MIN(h, _h - y);
	if (w <= 0 || h <= 0)
		return;

	if (room >= 0 && room != _room)
		loadRoom(room);
	if (room >= 0)
		_screenRoom = room;

	// The owners are copied along with the pixels: the game may draw the next frame's actors on the virtual
	// screen before the screen is rendered again (a palette cycle re-renders it all), so the live owners
	// can be newer than the screen pixels.
	// The rectangle can run past the end of the owner map (the virtual screen was resized since the owners were
	// set up, e.g. during a fade after a cutscene), so each pixel is checked.
	const bool fromVs = _vsPixels && src >= _vsPixels && src < _vsPixels + _owners.size();
	for (int j = 0; j < h; ++j) {
		Owner *so = &_screenOwners[(y + j) * _w + x];
		for (int i = 0; i < w; ++i) {
			const size_t off = fromVs ? (size_t)(src + j * pitch + i - _vsPixels) : _owners.size();
			so[i] = off < _owners.size() ? _owners[off] : Owner();
		}
		memcpy(_screen + (y + j) * _w + x, src + j * pitch, w);
		memcpy(_back + (y + j) * _w + x, (back ? back : src) + j * pitch, w);
		int16 *pos = _roomPos + ((y + j) * _w + x) * 2;
		for (int i = 0; i < w; ++i) {
			*pos++ = roomX < 0 ? -1 : roomX + i;
			*pos++ = roomY < 0 ? -1 : roomY + j;
		}
	}
	render(x, y, w, h);
}

bool HdBackgrounds::startVideo(const char *filename) {
	stopVideo();
#ifdef USE_VPX
	Common::String name(filename);
	for (const char *sep = "/\\:"; *sep; ++sep) {
		size_t p = name.findLastOf(*sep);
		if (p != Common::String::npos)
			name = name.substr(p + 1);
	}
	size_t dot = name.findLastOf('.');
	if (dot != Common::String::npos)
		name = name.substr(0, dot);
	name.toUppercase();

	Common::FSNode dir = _dir.getChild("video");
	Common::FSNode file = dir.getChild(name + ".ivf");
	if (!file.exists()) {
		debug(1, "HD backgrounds: no HD video for %s", name.c_str());
		return false;
	}
	Video *v = new Video();
	v->stream = file.createReadStream();
	byte header[32];
	if (!v->stream || v->stream->read(header, 32) != 32 || memcmp(header, "DKIF", 4) || memcmp(header + 8, "VP90", 4) ||
			READ_LE_UINT16(header + 12) != _w * kScale || READ_LE_UINT16(header + 14) != _h * kScale || !v->initCodec()) {
		warning("HD backgrounds: %s is not a %dx%d VP9 IVF file", file.getName().c_str(), _w * kScale, _h * kScale);
		delete v;
		return false;
	}
	v->headerSize = READ_LE_UINT16(header + 6);
	v->stream->seek(v->headerSize);
	v->image.create(_w * kScale, _h * kScale, _format);
	v->clean = new byte[_w * _h];

	Common::SeekableReadStream *list = dir.getChild(name + ".nohd").createReadStream();
	if (list) {
		Common::String text = list->readString(0, list->size());
		delete list;
		for (const char *p = text.c_str(); *p; ) {
			while (*p && !Common::isDigit(*p))
				++p;
			if (!*p)
				break;
			int n = atoi(p);
			if (n >= (int)v->noHd.size())
				v->noHd.resize(n + 1);
			v->noHd[n] = true;
			while (Common::isDigit(*p))
				++p;
		}
	}
	_video = v;
	debug(1, "HD backgrounds: playing HD video %s", file.getName().c_str());
	return true;
#else
	return false;
#endif
}

void HdBackgrounds::stopVideo() {
	delete _video;
	_video = nullptr;
}

void HdBackgrounds::videoClean(const byte *frame, int w, int h) {
	if (!_video)
		return;
	_video->haveClean = w == _w && h == _h;
	if (_video->haveClean)
		memcpy(_video->clean, frame, _w * _h);
}

bool HdBackgrounds::decodeVideoFrame() {
#ifdef USE_VPX
	Video *v = _video;
	byte header[12];
	if (v->stream->read(header, 12) != 12)
		return false;
	uint32 size = READ_LE_UINT32(header);
	v->data.resize(size);
	if (v->stream->read(v->data.data(), size) != size ||
			vpx_codec_decode(&v->codec, v->data.data(), size, nullptr, 0) != VPX_CODEC_OK)
		return false;
	vpx_codec_iter_t iter = nullptr;
	vpx_image_t *img = vpx_codec_get_frame(&v->codec, &iter);
	if (!img || img->fmt != VPX_IMG_FMT_I420 || (int)img->d_w != v->image.w || (int)img->d_h != v->image.h)
		return false;
	YUVToRGBMan.convert420(&v->image, Graphics::YUVToRGBManager::kScaleITU, img->planes[0], img->planes[1], img->planes[2],
	                       img->d_w, img->d_h, img->stride[0], img->stride[1]);
	++v->frame;
	return true;
#else
	return false;
#endif
}

void HdBackgrounds::videoFrame(int frame) {
	Video *v = _video;
	if (!v)
		return;
#ifdef USE_VPX
	if (frame < v->frame) {
		// Played back from an earlier frame: start decoding over
		v->stream->seek(v->headerSize);
		v->initCodec();
		v->frame = -1;
	}
#endif
	while (v->frame < frame && decodeVideoFrame())
		;
	v->show = v->frame == frame && v->haveClean && !(frame < (int)v->noHd.size() && v->noHd[frame]);
}

void HdBackgrounds::HdCel::free() {
	for (Graphics::Surface **sp : { &hd, &orig }) {
		if (*sp) {
			(*sp)->free();
			delete *sp;
			*sp = nullptr;
		}
	}
}

void HdBackgrounds::setVirtScreen(const byte *pixels, int pitch, int height) {
	if (pixels == _vsPixels && _owners.size() == (uint)(pitch * height))
		return;
	_vsPixels = pixels;
	_owners.clear();
	_owners.resize(pitch * height);
}

uint32 HdBackgrounds::celBegin(int costume, uint32 akcd, bool mirror) {
	static const uint kMaxCels = 600;   // HD cels are up to a few hundred KB each
	const uint64 key = ((uint64)costume << 32) | akcd;
	uint32 slot;
	if (_celSlots.contains(key)) {
		slot = _celSlots[key];
	} else {
		if (_cels.size() >= kMaxCels) {
			// Cache full: start over. Owner records of the old cels become invalid through the generation.
			for (HdCel &c : _cels)
				c.free();
			_cels.clear();
			_celSlots.clear();
			_celGen = (_celGen + 1) & 0x7FF;
			if (!_celGen)
				_celGen = 1;
		}
		slot = _cels.size();
		_cels.push_back(HdCel());
		_celSlots[key] = slot;
		HdCel &c = _cels.back();
		c.gen = _celGen;

		Common::String name = Common::String::format("%08x.png", akcd);
		Common::FSNode dir = _dir.getChild("costumes");
		Common::String num = Common::String::format("%d", costume);
		Common::FSNode hdFile = dir.getChild(num).getChild(name);
		Common::FSNode origFile = dir.getChild("original").getChild(num).getChild(name);
		if (hdFile.exists() && origFile.exists()) {
			for (int k = 0; k < 2; ++k) {
				Image::PNGDecoder dec;
				Common::SeekableReadStream *st = (k ? origFile : hdFile).createReadStream();
				if (st && dec.loadStream(*st) && dec.getSurface())
					(k ? c.orig : c.hd) = dec.getSurface()->convertTo(Graphics::PixelFormat::createFormatRGBA32(true));
				delete st;
			}
			if (!c.hd || !c.orig || c.hd->w != c.orig->w * kScale || c.hd->h != c.orig->h * kScale) {
				warning("HD costumes: %d/%s is not %dx its original", costume, name.c_str(), kScale);
				c.free();
			}
		}
		debug(2, "HD costumes: %d/%s%s", costume, name.c_str(), c.hd ? "" : " (none)");
	}
	debug(3, "HD cel draw %d/%08x", costume, akcd);
	if (!_cels[slot].hd)
		return 0;
	return ((uint32)_cels[slot].gen << 20 | (slot + 1)) | (mirror ? 0x80000000 : 0);
}

const HdBackgrounds::Owner *HdBackgrounds::validOwner(int i, int j) const {
	if (i < 0 || j < 0 || i >= _w || j >= _h)
		return nullptr;
	const Owner &o = _screenOwners[j * _w + i];
	const byte idx = _screen[j * _w + i];
	if ((!o.rec && !o.shade) || o.color != idx || _back[j * _w + i] == idx)
		return nullptr;
	return &o;
}

// For shaded screen pixel i, j: the palette colors under it (c[0..2]) and the shadow palette's result (c[3..5]),
// averaged over the shaded pixels around it. The shadow palette picks the nearest of 256 colors for every
// pixel, so neighbors that should get the same shading come out a little different, which a 4x scale shows.
bool HdBackgrounds::shadeColors(int i, int j, uint *c) const {
	const Owner *own = validOwner(i, j);
	if (!own || !own->shade)
		return false;
	uint n = 0;
	for (int k = 0; k < 6; ++k)
		c[k] = 0;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			const Owner *o = validOwner(i + dx, j + dy);
			if (!o || !o->shade)
				continue;
			const byte *u = _palette + o->under * 3, *r = _palette + _screen[(j + dy) * _w + i + dx] * 3;
			for (int ch = 0; ch < 3; ++ch) {
				c[ch] += u[ch];
				c[3 + ch] += r[ch];
			}
			++n;
		}
	}
	for (int k = 0; k < 6; ++k)
		c[k] = c[k] * 16 / n;   // 1/16 steps
	return true;
}

// The HD alpha of the shading cel of o (the glass, the beam, the shadow) at its cel pixel sx, sy, which can be
// just outside the original shape where the HD cel's smooth edge spills. False without an HD cel there.
bool HdBackgrounds::shadeAlpha(const Owner &o, int sx, int sy, byte *alpha) const {
	const uint32 slot = (o.shadeRec & 0xFFFFF) - 1;
	if (!o.shadeRec || slot >= _cels.size() || _cels[slot].gen != ((o.shadeRec >> 20) & 0x7FF) || !_cels[slot].hd)
		return false;
	const HdCel &c = _cels[slot];
	if (sx < 0 || sy < 0 || sx >= c.orig->w || sy >= c.orig->h)
		return false;
	const bool mirror = o.shadeRec & 0x80000000;
	bool any = false;
	for (int dy = 0; dy < kScale; ++dy) {
		for (int dx = 0; dx < kScale; ++dx) {
			byte a, r, g, b;
			c.hd->format.colorToARGB(*(const uint32 *)c.hd->getBasePtr(sx * kScale + (mirror ? kScale - 1 - dx : dx), sy * kScale + dy), a, r, g, b);
			alpha[dy * kScale + dx] = a;
			any |= a != 0;
		}
	}
	return any;
}

// dst = base + (dst - base) * alpha, for a kScale x kScale block
void HdBackgrounds::blendBlock(const uint32 *base, const byte *alpha, uint32 *dst, int dstPitch) const {
	for (int dy = 0; dy < kScale; ++dy) {
		for (int dx = 0; dx < kScale; ++dx) {
			const uint a = alpha[dy * kScale + dx];
			uint32 &d = dst[dy * dstPitch + dx];
			byte r, g, b, br, bg, bb;
			_format.colorToRGB(d, r, g, b);
			_format.colorToRGB(base[dy * kScale + dx], br, bg, bb);
			d = _format.RGBToColor((r * a + br * (255 - a)) / 255, (g * a + bg * (255 - a)) / 255, (b * a + bb * (255 - a)) / 255);
		}
	}
}

// Recolor the HD block of shaded pixel i, j the way the shadow palette turned the color under it into the
// one on screen: darker channels are multiplied, brighter ones screened, so the block's texture stays and its
// flat color lands on the new one. Unless flat is set, the colors are interpolated towards the neighbors'
// across the block.
void HdBackgrounds::shadeBlock(int i, int j, bool flat, uint32 *dst, int dstPitch) const {
	uint nb[3][3][6];
	if (!shadeColors(i, j, nb[1][1]))
		return;
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			if ((dx || dy) && (flat || !shadeColors(i + dx, j + dy, nb[dy + 1][dx + 1])))
				memcpy(nb[dy + 1][dx + 1], nb[1][1], sizeof(nb[1][1]));
	const int n = 2 * kScale;
	for (int dy = 0; dy < kScale; ++dy) {
		const int oy = 2 * dy + 1 - kScale, wy = ABS(oy), sy = oy < 0 ? 0 : 2;
		for (int dx = 0; dx < kScale; ++dx) {
			const int ox = 2 * dx + 1 - kScale, wx = ABS(ox), sx = ox < 0 ? 0 : 2;
			uint32 &d = dst[dy * dstPitch + dx];
			byte c[3];
			_format.colorToRGB(d, c[0], c[1], c[2]);
			for (int ch = 0; ch < 3; ++ch) {
				uint v[2];
				for (int k = 0; k < 2; ++k)
					v[k] = ((n - wx) * (n - wy) * nb[1][1][3 * k + ch] + wx * (n - wy) * nb[1][sx][3 * k + ch] +
					        (n - wx) * wy * nb[sy][1][3 * k + ch] + wx * wy * nb[sy][sx][3 * k + ch]) / (n * n);
				const uint u = v[0], r = v[1];   // 1/16 steps
				if (r <= u)
					c[ch] = c[ch] * (r + 16) / (u + 16);
				else
					c[ch] = 255 - (255 - c[ch]) * (4096 - r) / (4096 - u);
			}
			d = _format.RGBToColor(c[0], c[1], c[2]);
		}
	}
}

// Draw the HD block for cel pixel sx, sy over what dst holds (the scenery under it), tinted per channel by
// the current palette color idx against the cel's original color at the owner's pixel (o, on screen at oi, oj).
// With edge set, sx, sy is just outside the original cel's shape (a neighbor of a costume pixel): only the HD
// cel's soft edge spilling there is drawn, so the silhouette isn't cut along the original pixels.
bool HdBackgrounds::celBlock(const Owner &o, int oi, int oj, int sx, int sy, byte idx, bool edge, uint32 *dst, int dstPitch) const {
	const uint32 slot = (o.rec & 0xFFFFF) - 1;
	if (slot >= _cels.size() || _cels[slot].gen != ((o.rec >> 20) & 0x7FF) || !_cels[slot].hd)
		return false;
	const HdCel &c = _cels[slot];
	if (o.sx >= c.orig->w || o.sy >= c.orig->h || sx < 0 || sy < 0 || sx >= c.orig->w || sy >= c.orig->h)
		return false;
	byte oa, orr, og, ob;
	c.orig->format.colorToARGB(*(const uint32 *)c.orig->getBasePtr(o.sx, o.sy), oa, orr, og, ob);
	if (!oa || (orr > 200 && ob > 200 && og < 128))   // transparent, or the magenta shadow color (255, 87, 255)
		return false;
	if (edge) {
		byte ea, er, eg, eb;
		c.orig->format.colorToARGB(*(const uint32 *)c.orig->getBasePtr(sx, sy), ea, er, eg, eb);
		if (ea)   // inside the original shape: that pixel is drawn by its own owner, or covered by something else
			return false;
	}
	const byte *cur = _palette + idx * 3;
	const bool mirror = o.rec & 0x80000000;

	// A scaled actor (the tram arriving and leaving): a screen pixel stands for a run of cel pixels, from the one
	// after the previous screen pixel's up to its own (the scaling skips the others), told by the neighbors'
	// owners. The HD block averages that part of the HD cel instead of showing one cel pixel's block.
	auto sameCel = [&](int qi, int qj) -> const Owner * {
		if (qi < 0 || qj < 0 || qi >= _w || qj >= _h)
			return nullptr;
		const Owner *q = &_screenOwners[qj * _w + qi];
		return !q->shade && q->rec == o.rec && q->color == _screen[qj * _w + qi] ? q : nullptr;
	};
	const int back = mirror ? 1 : -1;   // screen direction of the previous cel column
	int spanX = 1, spanY = 1;
	const Owner *q;
	if ((q = sameCel(oi + back, oj)) && q->sy == o.sy && q->sx < o.sx)
		spanX = o.sx - q->sx;
	else if ((q = sameCel(oi - back, oj)) && q->sy == o.sy && q->sx > o.sx)
		spanX = q->sx - o.sx;
	if ((q = sameCel(oi, oj - 1)) && q->sx == o.sx && q->sy < o.sy)
		spanY = o.sy - q->sy;
	else if ((q = sameCel(oi, oj + 1)) && q->sx == o.sx && q->sy > o.sy)
		spanY = q->sy - o.sy;
	spanX = MIN(spanX, 64);
	spanY = MIN(spanY, 64);
	const bool scaled = spanX > 1 || spanY > 1;
	if (scaled && edge)   // the soft edge spill assumes neighboring cel pixels
		return false;

	// Scale4x cels hold exact copies of the cel's colors: their own pixel's and, in the corners, nearby cel
	// pixels' (up to two pixels away, Scale2x twice). Such a color is replaced by the color that cel pixel has
	// on screen now, which follows palette changes and actor palettes that map the costume to other colors.
	struct { uint32 rgb; const byte *cur; } exact[13];
	int nn = 0;
	exact[nn].rgb = orr << 16 | og << 8 | ob;
	exact[nn++].cur = cur;
	// The 3x3 cel pixels around the owner's, in screen orientation: original and current color (for the
	// Real-ESRGAN cels below); neighbors that aren't on screen take the owner pixel's.
	int nbOrig[3][3][3], nbCur[3][3][3];
	for (int y3 = 0; y3 < 3; ++y3)
		for (int x3 = 0; x3 < 3; ++x3)
			for (int ch = 0; ch < 3; ++ch) {
				nbOrig[y3][x3][ch] = ch == 0 ? orr : ch == 1 ? og : ob;
				nbCur[y3][x3][ch] = cur[ch];
			}
	static const int nb12[12][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
	                                 { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 } };
	for (int n = 0; n < 12; ++n) {
		const int qx = o.sx + nb12[n][0], qy = o.sy + nb12[n][1];
		const int qi = oi + (mirror ? -nb12[n][0] : nb12[n][0]), qj = oj + nb12[n][1];
		if (qi < 0 || qj < 0 || qi >= _w || qj >= _h || qx < 0 || qy < 0 || qx >= c.orig->w || qy >= c.orig->h)
			continue;
		// not validOwner(): a costume pixel that has the scenery's color still has its own color
		const Owner *q = &_screenOwners[qj * _w + qi];
		if (q->shade || q->rec != o.rec || q->sx != qx || q->sy != qy || q->color != _screen[qj * _w + qi])
			continue;
		byte qa, qr, qg, qb;
		c.orig->format.colorToARGB(*(const uint32 *)c.orig->getBasePtr(qx, qy), qa, qr, qg, qb);
		if (!qa)
			continue;
		exact[nn].rgb = qr << 16 | qg << 8 | qb;
		exact[nn++].cur = _palette + _screen[qj * _w + qi] * 3;
		if (n < 8 && !edge) {
			int *no = nbOrig[nb12[n][1] + 1][qi - oi + 1], *nc = nbCur[nb12[n][1] + 1][qi - oi + 1];
			no[0] = qr; no[1] = qg; no[2] = qb;
			for (int ch = 0; ch < 3; ++ch)
				nc[ch] = exact[nn - 1].cur[ch];
		}
	}
	auto findExact = [&](uint32 rgb) -> const byte * {
		for (int n = 0; n < nn; ++n)
			if (exact[n].rgb == rgb)
				return exact[n].cur;
		return nullptr;
	};

	// Other HD colors (Real-ESRGAN cels): the HD pixel's detail (its difference from the cel's original color)
	// on top of the color the cel has on screen now, both interpolated across the block from the neighboring
	// cel pixels. That follows fades, flashes (the power room's lightning) and recolored palette entries, which
	// a per-channel factor can't (a channel that was near 0), and the detail is scaled with the brightness.
	const int n2 = 2 * kScale;
	for (int dy = 0; dy < kScale; ++dy) {
		for (int dx = 0; dx < kScale; ++dx) {
			byte a, r, g, b, br, bg, bb;
			if (!scaled) {
				const int hx = sx * kScale + (mirror ? kScale - 1 - dx : dx), hy = sy * kScale + dy;
				c.hd->format.colorToARGB(*(const uint32 *)c.hd->getBasePtr(hx, hy), a, r, g, b);
			} else {
				const int x0 = mirror ? (sx + 1) * kScale - (dx + 1) * spanX : (sx + 1 - spanX) * kScale + dx * spanX;
				const int y0 = (sy + 1 - spanY) * kScale + dy * spanY;
				const int stepX = (spanX + 3) / 4, stepY = (spanY + 3) / 4;   // at most 4x4 samples
				uint sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
				for (int y = y0; y < y0 + spanY; y += stepY) {
					for (int x = x0; x < x0 + spanX; x += stepX) {
						++n;
						if (x < 0 || y < 0 || x >= c.hd->w || y >= c.hd->h)
							continue;
						byte pa, pr, pg, pb;
						c.hd->format.colorToARGB(*(const uint32 *)c.hd->getBasePtr(x, y), pa, pr, pg, pb);
						sa += pa;
						sr += pr * pa;
						sg += pg * pa;
						sb += pb * pa;
					}
				}
				a = sa / n;
				if (sa) {
					r = sr / sa;
					g = sg / sa;
					b = sb / sa;
				}
			}
			if (!a)
				continue;
			uint32 &d = dst[dy * dstPitch + dx];
			_format.colorToRGB(d, br, bg, bb);
			if (const byte *e = findExact(r << 16 | g << 8 | b)) {
				r = e[0]; g = e[1]; b = e[2];
			} else {
				const int ox = 2 * dx + 1 - kScale, wx = ABS(ox), qx = ox < 0 ? 0 : 2;
				const int oy = 2 * dy + 1 - kScale, wy = ABS(oy), qy = oy < 0 ? 0 : 2;
				const int w00 = (n2 - wx) * (n2 - wy), w01 = wx * (n2 - wy), w10 = (n2 - wx) * wy, w11 = wx * wy;
				int O[3], C[3];
				for (int ch = 0; ch < 3; ++ch) {
					O[ch] = (w00 * nbOrig[1][1][ch] + w01 * nbOrig[1][qx][ch] + w10 * nbOrig[qy][1][ch] + w11 * nbOrig[qy][qx][ch]) / (n2 * n2);
					C[ch] = (w00 * nbCur[1][1][ch] + w01 * nbCur[1][qx][ch] + w10 * nbCur[qy][1][ch] + w11 * nbCur[qy][qx][ch]) / (n2 * n2);
				}
				const int lumO = O[0] * 3 + O[1] * 6 + O[2], lumC = C[0] * 3 + C[1] * 6 + C[2];
				const int sc = CLIP((lumC + 160) * 256 / (lumO + 160), 64, 512);   // detail scale, 1/256
				const byte h[3] = { r, g, b };
				r = CLIP(C[0] + (h[0] - O[0]) * sc / 256, 0, 255);
				g = CLIP(C[1] + (h[1] - O[1]) * sc / 256, 0, 255);
				b = CLIP(C[2] + (h[2] - O[2]) * sc / 256, 0, 255);
			}
			d = _format.RGBToColor((r * a + br * (255 - a)) / 255, (g * a + bg * (255 - a)) / 255, (b * a + bb * (255 - a)) / 255);
		}
	}
	return true;
}

// Write the kScale x kScale HD block for image pixel ix, iy if the image has
// palette index idx there. field points at the smoothed tint of the screen
// pixel being drawn; the tint is interpolated towards the neighboring pixels'
// tints across the block.
bool HdBackgrounds::hdPixel(const HdImage &img, int ix, int iy, byte idx, const uint16 *field, int fieldPitch, uint32 *dst, int dstPitch) const {
	if (!img.hd || ix < 0 || iy < 0 || ix >= img.w || iy >= img.h || img.orig[iy * img.w + ix] != idx)
		return false;
	const Graphics::PixelFormat &hdFormat = img.hd->format;
	const int hdPitch = img.hd->pitch / 4;
	const uint32 *hp = (const uint32 *)img.hd->getPixels() + iy * kScale * hdPitch + ix * kScale;
	for (int dy = 0; dy < kScale; ++dy) {
		// Distance of the sub-pixel center from the block center, in 1/(2 * kScale)
		const int oy = 2 * dy + 1 - kScale, wy = ABS(oy);
		const uint16 *rowY = field + (oy < 0 ? -fieldPitch : fieldPitch);
		for (int dx = 0; dx < kScale; ++dx) {
			const int ox = 2 * dx + 1 - kScale, wx = ABS(ox);
			const int sx = ox < 0 ? -3 : 3;
			const int n = 2 * kScale;
			uint k[3];
			for (int ch = 0; ch < 3; ++ch)
				k[ch] = ((n - wx) * (n - wy) * field[ch] + wx * (n - wy) * field[sx + ch] +
				         (n - wx) * wy * rowY[ch] + wx * wy * rowY[sx + ch]) / (n * n);
			byte r, g, b;
			hdFormat.colorToRGB(hp[dy * hdPitch + dx], r, g, b);
			dst[dy * dstPitch + dx] = _format.RGBToColor(
				MIN<uint>(255, r * k[0] >> 8), MIN<uint>(255, g * k[1] >> 8), MIN<uint>(255, b * k[2] >> 8));
		}
	}
	return true;
}

// Average the tints of screen pixel i, j and its neighbors. Dithered areas mix
// palette indices that may be tinted differently (e.g. when only some of them
// are color-cycled); tinting each HD block by its own index would bring the
// dither pattern back.
void HdBackgrounds::smoothTint(int i, int j, uint16 *k) const {
	const byte idx = _screen[CLIP(j, 0, _h - 1) * _w + CLIP(i, 0, _w - 1)];
	uint sum[3] = { 0, 0, 0 }, n = 0;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			const int ni = CLIP(i + dx, 0, _w - 1), nj = CLIP(j + dy, 0, _h - 1);
			const byte nidx = _screen[nj * _w + ni];
			const uint16 *t = _tintOk[nidx] ? _tint[nidx] : _tint[idx];
			sum[0] += t[0];
			sum[1] += t[1];
			sum[2] += t[2];
			++n;
		}
	}
	for (int ch = 0; ch < 3; ++ch)
		k[ch] = sum[ch] / n;
}

static void scale2x(const byte *src, int w, int h, byte *dst) {
	const int dw = w * 2;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const byte p = src[y * w + x];
			const byte a = src[MAX(y - 1, 0) * w + x], d = src[MIN(y + 1, h - 1) * w + x];
			const byte c = src[y * w + MAX(x - 1, 0)], b = src[y * w + MIN(x + 1, w - 1)];
			byte *o = dst + 2 * y * dw + 2 * x;
			if (c == a && c != d && a != b) o[0] = a; else o[0] = p;
			if (a == b && a != c && b != d) o[1] = b; else o[1] = p;
			if (d == c && d != b && c != a) o[dw] = c; else o[dw] = p;
			if (b == d && b != a && d != c) o[dw + 1] = d; else o[dw + 1] = p;
		}
	}
}

// Scale4x the palette indices of the area plus a two pixel border, for the
// pixels that are not replaced by HD images (actors, text, videos).
void HdBackgrounds::scaleIndices(int x, int y, int w, int h) {
	const int bw = w + 4, bh = h + 4;
	Common::Array<byte> src(bw * bh);
	for (int j = 0; j < bh; ++j)
		for (int i = 0; i < bw; ++i)
			src[j * bw + i] = _screen[CLIP(y + j - 2, 0, _h - 1) * _w + CLIP(x + i - 2, 0, _w - 1)];
	_scaled1.resize(bw * bh * 4);
	_scaled2.resize(bw * bh * 16);
	scale2x(src.data(), bw, bh, _scaled1.data());
	scale2x(_scaled1.data(), bw * 2, bh * 2, _scaled2.data());
}

void HdBackgrounds::render(int x, int y, int w, int h) {
	const bool haveHd = _bg.hd && _screenRoom == _room;
	const int dstPitch = _out.pitch / 4;

	scaleIndices(x, y, w, h);
	const int scaledPitch = (w + 4) * kScale;

	// Smoothed tints for the area plus a one pixel border
	const int fieldPitch = (w + 2) * 3;
	if (haveHd) {
		_field.resize(fieldPitch * (h + 2));
		for (int j = 0; j < h + 2; ++j)
			for (int i = 0; i < w + 2; ++i)
				smoothTint(x + i - 1, y + j - 1, &_field[j * fieldPitch + i * 3]);
	}

	const bool video = _video && _video->show;
	const int videoPitch = video ? _video->image.pitch / 4 : 0;

	for (int j = y; j < y + h; ++j) {
		for (int i = x; i < x + w; ++i) {
			const byte idx = _screen[j * _w + i];
			uint32 *dst = (uint32 *)_out.getBasePtr(i * kScale, j * kScale);
			if (video && _video->clean[j * _w + i] == idx) {
				// SMUSH video pixel without text on it: HD video frame
				const uint32 *vp = (const uint32 *)_video->image.getBasePtr(i * kScale, j * kScale);
				for (int dy = 0; dy < kScale; ++dy)
					memcpy(dst + dy * dstPitch, vp + dy * videoPitch, kScale * 4);
				_why[j * _w + i] = 7;
				continue;
			}
			const int16 rx = _roomPos[(j * _w + i) * 2];
			const int16 ry = _roomPos[(j * _w + i) * 2 + 1];

			// A costume pixel with an HD cel: draw the scenery under it (the back buffer pixel), then the cel
			const Owner *own = validOwner(i, j);
			const byte sidx = own ? _back[j * _w + i] : idx;

			// HD scenery for back buffer index sidx at this pixel; why: 5 room, 6 object
			auto scenery = [&](byte sidx, byte &why) {
				if (!haveHd || rx < 0 || ry < 0 || !_tintOk[sidx])
					return false;
				const uint16 *field = &_field[(j - y + 1) * fieldPitch + (i - x + 1) * 3];
				// Objects first: where an object image covers the room, its pixels can share palette indices
				// with the room image under it, and the HD room (which shows the room's original content
				// there) would turn the object into a mosaic of both.
				for (int o = _objects.size() - 1; o >= 0; --o) {
					if (hdPixel(_objects[o]->image, rx - _objects[o]->x, ry - _objects[o]->y, sidx, field, fieldPitch, dst, dstPitch)) {
						why = 6;
						return true;
					}
				}
				if (hdPixel(_bg, rx, ry, sidx, field, fieldPitch, dst, dstPitch)) {
					why = 5;
					return true;
				}
				return false;
			};

			bool done = false;
			byte &why = _why[j * _w + i];
			if (own && own->shade) {
				// Shadow, tinted glass or glow: draw what is under it in HD (the scenery, or an HD costume pixel
				// on top of it), then recolor that the way the shadow palette did
				const byte back = _back[j * _w + i];
				byte w;
				done = (own->under == back || own->rec) && scenery(back, w);
				why = 9;
				if (own->under != back) {
					if (!done)
						for (int dy = 0; dy < kScale; ++dy)
							for (int dx = 0; dx < kScale; ++dx)
								dst[dy * dstPitch + dx] = _palette32[back];
					done = own->rec && celBlock(*own, i, j, own->sx, own->sy, own->under, false, dst, dstPitch);
					why = done ? 10 : own->rec ? 11 : 12;
				}
				if (!done)
					for (int dy = 0; dy < kScale; ++dy)
						for (int dx = 0; dx < kScale; ++dx)
							dst[dy * dstPitch + dx] = _palette32[own->under];
				// Shade in proportion to the shading cel's HD alpha, so its edges are as smooth as the HD cel's.
				// Without HD alpha (the costume shadow color is left out of the HD cels) the whole block.
				uint32 base[kScale * kScale];
				for (int dy = 0; dy < kScale; ++dy)
					memcpy(base + dy * kScale, dst + dy * dstPitch, kScale * 4);
				shadeBlock(i, j, false, dst, dstPitch);
				byte alpha[kScale * kScale];
				if (shadeAlpha(*own, own->ssx, own->ssy, alpha))
					blendBlock(base, alpha, dst, dstPitch);
				continue;
			}

			why = !haveHd ? 0 : (rx < 0 || ry < 0) ? 1 : !_tintOk[sidx] ? 2 : _back[j * _w + i] != sidx ? 3 : 4;
			if (why == 4)
				done = scenery(sidx, why);
			if (own) {
				if (!done) {
					for (int dy = 0; dy < kScale; ++dy)
						for (int dx = 0; dx < kScale; ++dx)
							dst[dy * dstPitch + dx] = _palette32[sidx];
				}
				if (celBlock(*own, i, j, own->sx, own->sy, idx, false, dst, dstPitch)) {
					why = 8;
					continue;
				}
				done = false;   // no HD pixel there (e.g. a shadow): show the original costume pixel
			}
			if (!done) {
				const byte *s = &_scaled2[(j - y + 2) * kScale * scaledPitch + (i - x + 2) * kScale];
				for (int dy = 0; dy < kScale; ++dy)
					for (int dx = 0; dx < kScale; ++dx)
						dst[dy * dstPitch + dx] = _palette32[s[dy * scaledPitch + dx]];
			}
			// Scenery next to an HD costume pixel: let the HD cel's soft edge spill over it
			if (!own && _back[j * _w + i] == idx) {
				static const int nb[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
				for (int n = 0; n < 4; ++n) {
					const Owner *o = validOwner(i + nb[n][0], j + nb[n][1]);
					if (!o || o->shade)
						continue;
					// this pixel is one cel pixel away from the owner's, the other way round if mirrored
					const int ddx = -nb[n][0] * ((o->rec & 0x80000000) ? -1 : 1), ddy = -nb[n][1];
					if (celBlock(*o, i + nb[n][0], j + nb[n][1], o->sx + ddx, o->sy + ddy, _screen[(j + nb[n][1]) * _w + i + nb[n][0]], true, dst, dstPitch))
						break;
				}
			}
			// Next to a shaded pixel: the shading cel's soft HD edge spills over this one
			static const int nb4[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for (int n = 0; n < 4; ++n) {
				const Owner *o = validOwner(i + nb4[n][0], j + nb4[n][1]);
				if (!o || !o->shade || !o->shadeRec)
					continue;
				const int ddx = -nb4[n][0] * ((o->shadeRec & 0x80000000) ? -1 : 1), ddy = -nb4[n][1];
				const int cx = o->ssx + ddx, cy = o->ssy + ddy;
				byte alpha[kScale * kScale], ca, cr, cg, cb;
				if (!shadeAlpha(*o, cx, cy, alpha))   // also checks that the cel is still in the cache
					continue;
				const HdCel &c = _cels[(o->shadeRec & 0xFFFFF) - 1];
				// inside the original shape: drawn by that cel itself, or that cel pixel isn't a shading one
				c.orig->format.colorToARGB(*(const uint32 *)c.orig->getBasePtr(cx, cy), ca, cr, cg, cb);
				if (ca)
					continue;
				uint32 base[kScale * kScale];
				for (int dy = 0; dy < kScale; ++dy)
					memcpy(base + dy * kScale, dst + dy * dstPitch, kScale * 4);
				shadeBlock(i + nb4[n][0], j + nb4[n][1], true, dst, dstPitch);
				blendBlock(base, alpha, dst, dstPitch);
				break;
			}
		}
	}
	_system->copyRectToScreen(_out.getBasePtr(x * kScale, y * kScale), _out.pitch,
	                          x * kScale, y * kScale, w * kScale, h * kScale);

	// Testing aid: SCUMMVM_HD_DUMP=<dir> saves the full frame every 3 seconds (SCUMMVM_HD_DUMP_MS: other interval).
	static const char *dumpDir = getenv("SCUMMVM_HD_DUMP");
	static uint32 lastDump = 0;
	static int dumpCount = 0;
	static const uint32 dumpMs = getenv("SCUMMVM_HD_DUMP_MS") ? atoi(getenv("SCUMMVM_HD_DUMP_MS")) : 3000;
	if (dumpDir && _system->getMillis() - lastDump > dumpMs) {
		lastDump = _system->getMillis();
		Common::DumpFile f;
		Common::Path path(Common::String::format("%s/frame_%03d.png", dumpDir, dumpCount++), '/');
		if (f.open(path, true))
			Image::writePNG(f, _out);
		debug(3, "HD dump frame_%03d", dumpCount - 1);
		// why map: 0 no HD room, 1 not room pixel, 2 color changed, 3 actor/text,
		// 4 no matching image pixel, 5 HD background, 6 HD object, 7 HD video, 8 HD costume, 9 shaded scenery,
		// 10 shaded HD costume, 11 shaded costume without HD pixel, 12 shaded other pixel (shadow/glass/glow)
		static const byte whyPal[13 * 3] = { 0,0,0, 60,60,60, 255,0,255, 255,0,0, 255,255,0, 0,160,0, 0,120,255, 0,255,255, 255,128,0,
		                                     255,255,255, 255,190,120, 160,0,0, 128,0,128 };
		Graphics::Surface whySurf;
		whySurf.init(_w, _h, _w, _why, Graphics::PixelFormat::createFormatCLUT8());
		Common::DumpFile wf;
		Common::Path wpath(Common::String::format("%s/why_%03d.png", dumpDir, dumpCount - 1), '/');
		if (wf.open(wpath, true))
			Image::writePNG(wf, whySurf, whyPal, 13);
		// and the original frame
		Graphics::Surface origSurf;
		origSurf.init(_w, _h, _w, _screen, Graphics::PixelFormat::createFormatCLUT8());
		Common::DumpFile of;
		Common::Path opath(Common::String::format("%s/orig_%03d.png", dumpDir, dumpCount - 1), '/');
		if (of.open(opath, true))
			Image::writePNG(of, origSurf, _palette, 256);
	}
}

int HdBackgrounds::takeTestRoom() {
	static bool taken = false;
	const char *room = getenv("SCUMMVM_HD_ROOM");
	if (taken || !room)
		return 0;
	taken = true;
	return atoi(room);
}

const char *HdBackgrounds::testStates() {
	return getenv("SCUMMVM_HD_STATE");
}

int HdBackgrounds::testCameraX() {
	static const int x = getenv("SCUMMVM_HD_CAMX") ? atoi(getenv("SCUMMVM_HD_CAMX")) : -1;
	return x;
}

int HdBackgrounds::testRoom() {
	static const int room = getenv("SCUMMVM_HD_ROOM") ? atoi(getenv("SCUMMVM_HD_ROOM")) : -1;
	return room;
}

int HdBackgrounds::testCameraY() {
	static const int y = getenv("SCUMMVM_HD_CAMY") ? atoi(getenv("SCUMMVM_HD_CAMY")) : -1;
	return y;
}

int HdBackgrounds::testCostume() {
	const char *costume = getenv("SCUMMVM_HD_COSTUME");
	return costume ? atoi(costume) : 0;
}

int HdBackgrounds::testWalkRoom() {
	static const int room = getenv("SCUMMVM_HD_WALK") && getenv("SCUMMVM_HD_ROOM") ? atoi(getenv("SCUMMVM_HD_ROOM")) : -1;
	return room;
}

const byte *HdBackgrounds::convertCursor(const byte *src, int &w, int &h, int &hotX, int &hotY, byte transColor, uint32 &transOut) {
	const int sw = w * kScale, sh = h * kScale;
	delete[] _cursor;
	_cursor = new byte[sw * sh * 4];
	uint32 *dst = (uint32 *)_cursor;
	transOut = _format.ARGBToColor(0, 255, 0, 255);
	for (int y = 0; y < sh; ++y) {
		for (int x = 0; x < sw; ++x) {
			const byte c = src[(y / kScale) * w + x / kScale];
			*dst++ = (c == transColor) ? transOut : _palette32[c];
		}
	}
	w = sw;
	h = sh;
	hotX *= kScale;
	hotY *= kScale;
	return _cursor;
}

} // End of namespace Scumm
