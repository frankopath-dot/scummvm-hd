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

#ifndef ASYLUM_SYSTEM_HD_H
#define ASYLUM_SYSTEM_HD_H

#include "common/array.h"
#include "common/fs.h"
#include "common/hashmap.h"
#include "common/rect.h"
#include "graphics/pixelformat.h"
#include "graphics/surface.h"

#include "asylum/shared.h"

class OSystem;

namespace Asylum {

struct GraphicFrame;

/**
 * Optional HD renderer.
 *
 * The engine keeps rendering into its 640x480 8-bit back buffer. For every
 * pixel a graphic resource writes there, we remember which resource frame and
 * which source pixel it came from. When the back buffer is presented, each such
 * pixel is replaced by the matching scale x scale block of the externally
 * upscaled frame (<dir>/RES.ppp/iiii_fff.png, 4x), tinted by how far the live
 * palette has moved from the palette the frame was extracted with
 * (<dir>/RES.ppp/iiii.pal, or the pack's <dir>/RES.ppp/_palette.bin). Pixels nobody claimed (lines, fills, video)
 * are drawn as plain enlarged pixels.
 *
 * Enabled with the game config keys hd_dir=<dir> and hd_scale=2|3|4.
 */
class HdRenderer {
public:
	HdRenderer(OSystem *system, const Common::FSNode &dir, int scale);
	~HdRenderer();

	/** Switch the backend to the HD screen mode; false if no 32-bit mode is available. */
	bool initMode();
	int scale() const { return _scale; }

	// Recording (called by Screen around its blits)
	void recordDraw(ResourceId id, uint32 frameIndex, const GraphicFrame *frame, const Common::Rect &src, const Common::Rect &dst, bool colorKey, const byte *backBuffer);
	void recordTranslucent(const GraphicFrame *frame, const Common::Rect &src, const Common::Rect &dst, bool mirrored, const byte *backBuffer);
	void invalidate(const Common::Rect &rect);
	void invalidateAll();

	// Output
	void present(const byte *backBuffer, const byte *palette);
	void paletteChanged(const byte *palette);
	void fillScreen(const Common::Rect &rect, uint32 color);
	void copyScaled(const Graphics::Surface &surface, int x, int y, const byte *palette = nullptr);
	void setCursor(ResourceId id, uint32 frameIndex, const GraphicFrame *frame, const Common::Point &hotspot, const byte *palette);

	/** Testing aid: scene to start in (ASYLUM_HD_SCENE), or -1. */
	static int testScene();

	/** Testing aid: movie to play at startup (ASYLUM_HD_VIDEO), or -1. */
	static int testVideo();

private:
	enum {
		kWidth = 640,
		kHeight = 480,
		kFlagTranslucent = 1,
		kFlagMirrored = 2
	};

	struct Owner {
		uint16 entry;   // 0 = none
		uint16 sx, sy;  // source pixel in the original frame
		byte index;     // palette index the frame has there
		byte flags;
	};

	struct HdFrame {
		int w, h;            // size at our scale
		uint32 *pixels;      // RGBA, 8 bits each, alpha in the low byte
		const byte *palette; // extraction palette of its pack
		uint32 lastUse;
		uint32 bytes;
	};

	struct Entry {
		ResourceId id;
		uint32 frameIndex;
	};

	OSystem *_system;
	Common::FSNode _dir;
	int _scale;
	Graphics::PixelFormat _format;

	Owner *_top, *_under;
	Owner *_snapTop, *_snapUnder;
	byte *_snapBuffer;
	byte _snapPalette[256 * 3];
	bool _hasSnapshot;

	Common::Array<Entry> _entries; // index 0 unused
	Common::HashMap<uint64, uint16> _entryIds;
	Common::Array<HdFrame *> _resolved;
	Common::Array<uint32> _resolvedGen;
	uint32 _gen;

	Common::HashMap<uint64, HdFrame *> _cache;
	uint64 _cacheBytes;
	Common::HashMap<uint32, byte *> _packPalettes;

	Graphics::Surface _out;
	Graphics::Surface _cursor;

	static uint64 key(ResourceId id, uint32 frameIndex) { return ((uint64)id << 16) | (frameIndex & 0xFFFF); }
	uint16 entryFor(ResourceId id, uint32 frameIndex);
	HdFrame *frameFor(uint16 entry);
	HdFrame *load(ResourceId id, uint32 frameIndex);
	const byte *readPalette(uint32 key, const Common::FSNode &node);
	const byte *resourcePalette(uint32 pack, uint16 index);
	void evict();
	void compose(const byte *buffer, const Owner *top, const Owner *under, const byte *palette);

	// Testing aid: ASYLUM_HD_DUMP=<dir> saves every ASYLUM_HD_DUMP_EVERY-th frame
	// (HD and original) and quits after ASYLUM_HD_DUMP_MAX of them
	void dump(const byte *buffer, const byte *palette);
	uint32 _presentCount;
	uint32 _dumpCount;
};

} // End of namespace Asylum

#endif // ASYLUM_SYSTEM_HD_H
