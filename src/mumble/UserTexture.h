// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_USERTEXTURE_H_
#define MUMBLE_MUMBLE_USERTEXTURE_H_

#include <QtCore/QByteArray>

/// Converts a user texture (avatar) as received from the server into a regular image.
/// Textures in the legacy format (zlib-compressed raw 600x60 ARGB32) are converted to PNG.
///
/// @param texture The texture data. It is replaced by the converted image if a conversion takes place.
/// @param format Set to the image format of the texture (e.g. "png") if it is valid
/// @returns Whether the texture is a valid image
bool normalizeUserTexture(QByteArray &texture, QByteArray &format);

#endif // MUMBLE_MUMBLE_USERTEXTURE_H_
