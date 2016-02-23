/*
 * Copyright (C) 2003-2015 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Generic.hxx"
#include "TagId3.hxx"
#include "ApeTag.hxx"
#include "fs/Path.hxx"
#include "input/InputStream.hxx"
#include "util/Error.hxx"

/**
 * Attempts to scan APE or ID3 tags from the specified file.
 */
bool
ScanGenericTags(InputStream &is, const TagHandler &handler, void *ctx)
{
	if (tag_ape_scan2(is, handler, ctx))
		return true;

	if (!is.Rewind(IgnoreError()))
		return false;

	return tag_id3_scan(is, handler, ctx);
}

/**
 * Attempts to scan APE or ID3 tags from the specified file.
 */
bool
ScanGenericTags(Path path, const TagHandler &handler, void *ctx)
{
	return tag_ape_scan2(path, handler, ctx) ||
		tag_id3_scan(path, handler, ctx);
}
