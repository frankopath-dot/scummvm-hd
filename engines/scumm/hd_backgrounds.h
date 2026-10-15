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

#ifndef SCUMM_HD_BACKGROUNDS_H
#define SCUMM_HD_BACKGROUNDS_H

#include "common/array.h"
#include "common/fs.h"
#include "graphics/surface.h"

class OSystem;

namespace Scumm {

/**
 * Experimental renderer that outputs the 8-bit game screen at kScale times
 * its size in true color, replacing room background and object pixels with
 * externally upscaled images.
 *
 * A screen pixel is scenery when the front and back buffers agree there (no
 * actor or text on top). Scenery pixels show the HD room image when they still
 * have the original room image's palette index at that room position, or the
 * HD image of an object drawn there when they match that object's original
 * image. Everything else is shown as an enlarged original pixel. Palette
 * changes are followed by tinting the HD images per palette index.
 *
 * The HD directory holds <room>_<name>.png at kScale times the room size and
 * original/<room>_<name>.png, the indexed original room image it was made from.
 * Its objects/ subdirectory holds <room>_<object>_IM<state>.png in the same way.
 */
class HdBackgrounds {
public:
	static const int kScale = 4;

	HdBackgrounds(OSystem *system, const Common::FSNode &dir, int width, int height);
	~HdBackgrounds();

	void setPalette(const byte *colors, uint first, uint num);

	/** Mark the palette indices animated by color cycling in the current room. */
	void setCycledColors(const byte *used);

	/** Note that an object image was drawn into the room at room position x, y. */
	void objectDrawn(int room, int obj, int state, int x, int y, int w, int h);

	/**
	 * Blit 8-bit pixels. For room pixels, roomX/roomY give the room position of
	 * the top-left pixel and back points at the matching back buffer pixels.
	 */
	void blit(const byte *src, int pitch, int x, int y, int w, int h,
	          int room = -1, int roomX = -1, int roomY = -1, const byte *back = nullptr);

	/**
	 * SMUSH video replacement: <dir>/video/<NAME>.ivf holds one VP9 frame at
	 * kScale times the size per FRME chunk of <NAME>.SAN; <NAME>.nohd lists
	 * frames to leave to the original pixels. Returns whether one was found.
	 */
	bool startVideo(const char *filename);
	void stopVideo();
	/** The SMUSH player decoded a frame object into frame; text drawn later is not in it. */
	void videoClean(const byte *frame, int w, int h);
	/** The SMUSH player finished frame number frame. */
	void videoFrame(int frame);

	/** Testing aid: room from SCUMMVM_HD_ROOM to jump to, returned once (0: none). */
	static int takeTestRoom();

	/** Convert an 8-bit cursor to an enlarged cursor in the screen format. */
	const byte *convertCursor(const byte *src, int &w, int &h, int &hotX, int &hotY, byte transColor, uint32 &transOut);

private:
	/** An upscaled image with the indexed original it was made from. */
	struct HdImage {
		Graphics::Surface *hd = nullptr;
		byte *orig = nullptr;
		int w = 0, h = 0;

		bool load(const Common::FSNode &hdFile, const Common::FSNode &origFile, byte *palette);
		void free();
	};

	struct HdObject {
		int obj, state, x, y;
		HdImage image;
	};

	void loadRoom(int room);
	void clearObjects();
	void updateTints();
	void render(int x, int y, int w, int h);
	bool hdPixel(const HdImage &img, int ix, int iy, byte idx, const uint16 *field, int fieldPitch, uint32 *dst, int dstPitch) const;
	void smoothTint(int i, int j, uint16 *k) const;
	void scaleIndices(int x, int y, int w, int h);
	Common::FSNode findRoomFile(const Common::FSNode &dir, int room) const;

	struct Video;
	bool decodeVideoFrame();

	OSystem *_system;
	Common::FSNode _dir;
	int _w, _h;
	Graphics::PixelFormat _format;
	Graphics::Surface _out;

	byte *_screen;      // 8-bit copy of what is on screen
	byte *_back;        // matching back buffer pixels (scenery without actors)
	int16 *_roomPos;    // per screen pixel: room x, room y (or -1)

	byte _palette[3 * 256];
	uint32 _palette32[256];

	int _room;          // room the HD data was last looked up for (-1: none yet)
	int _screenRoom;    // room the room positions in _roomPos refer to
	HdImage _bg;
	Common::Array<HdObject *> _objects;   // objects drawn in the current room, latest last
	byte _origPalette[3 * 256];
	uint16 _tint[256][3];
	bool _tintOk[256];
	bool _cycled[256];
	byte *_cursor;
	Common::Array<uint16> _field;   // smoothed tints for the area being rendered
	Common::Array<byte> _scaled1, _scaled2;   // Scale2x / Scale4x palette indices of that area
	Video *_video;      // active SMUSH replacement, or nullptr
	byte *_why;         // testing aid: per screen pixel, how it was rendered (see render())
};

} // End of namespace Scumm

#endif
