/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file screenshot_type.h Types related to screenshot providers. */

#ifndef SCREENSHOT_TYPE_H
#define SCREENSHOT_TYPE_H

#include "gfx_type.h"
#include "provider_manager.h"

#include <algorithm>

/**
 * Callback function signature for generating lines of pixel data to be written to the screenshot file.
 * @param userdata Pointer to user data.
 * @param buf      Destination buffer.
 * @param y        Line number of the first line to write.
 * @param pitch    Number of pixels to write (1 byte for 8bpp, 4 bytes for 32bpp). @see Colour
 * @param n        Number of lines to write.
 */
using ScreenshotCallback = std::function<void (void *buf, uint y, uint pitch, uint n)>;

/**
 * Compression effort a screenshot provider spends by default, on the usual 0..9 scale where 0
 * stores the image as fast as possible and 9 spends the most time for the smallest file. A caller
 * with a time budget of its own may ask for a different level; the pixels are the same either way.
 */
constexpr uint SCREENSHOT_COMPRESSION_DEFAULT = 6;

/**
 * Clamp a requested compression effort to the range a provider understands.
 * An unsigned value cannot be negative, so only the upper bound has to be clamped.
 * @param compression The requested effort, 0 (fastest) to 9 (smallest).
 * @return The effort a provider may spend, 0..9.
 */
inline uint ClampScreenshotCompression(uint compression)
{
	return std::min<uint>(compression, 9);
}

/** Base interface for a SoundLoader implementation. */
class ScreenshotProvider : public PriorityBaseProvider<ScreenshotProvider> {
public:
	ScreenshotProvider(std::string_view name, std::string_view description, int priority) : PriorityBaseProvider<ScreenshotProvider>(name, description, priority)
	{
		ProviderManager<ScreenshotProvider>::Register(*this);
	}

	/** Unregister ourselves from the screenshot providers. */
	~ScreenshotProvider() override
	{
		ProviderManager<ScreenshotProvider>::Unregister(*this);
	}

	/**
	 * Create and write an image to a file.
	 * @param name The file name to write to.
	 * @param callb The callback that fills a buffer with pixel data.
	 * @param w The width of the image.
	 * @param h The height of the image.
	 * @param pixelformat The number of bits per pixel for the image.
	 * @param palette The palette that is currently being used.
	 * @param compression The compression effort to spend, 0 (fastest, largest) to 9 (slowest,
	 *                    smallest). The PNG provider maps it to the zlib level; a provider whose
	 *                    format has no compression stage ignores it. The default is what a player's
	 *                    screenshot has always used, so only a caller with a time budget of its own
	 *                    passes a different value.
	 * @return Whether writing the image was successful of not.
	 */
	virtual bool MakeImage(std::string_view name, const ScreenshotCallback &callb, uint w, uint h, int pixelformat, const Colour *palette, uint compression = SCREENSHOT_COMPRESSION_DEFAULT) const = 0;
};

#endif /* SCREENSHOT_TYPE_H */
