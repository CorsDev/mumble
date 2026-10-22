// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "IPCUtils.h"

#include <system_error>

#ifndef _WIN32
#	include <cstddef>
#	include <cstdlib>
#	include <string>

#	include <unistd.h>
#endif

namespace Mumble {

std::filesystem::path getRuntimeDirectory() {
#ifdef _WIN32
	return {};
#else
	std::filesystem::path runtimeDir;

	const char *xdgRuntimeDir = std::getenv("XDG_RUNTIME_DIR");
	if (xdgRuntimeDir != nullptr && xdgRuntimeDir[0] != '\0') {
		runtimeDir = std::filesystem::path(xdgRuntimeDir) / "info.mumble.Mumble";
	} else {
#	ifdef __APPLE__
		// macOS has no /run/user. Its per-user temporary directory is private to the user as well.
		const std::size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
		if (size > 0) {
			std::string userTempDir(size, '\0');
			confstr(_CS_DARWIN_USER_TEMP_DIR, userTempDir.data(), size);
			userTempDir.resize(size - 1);
			runtimeDir = std::filesystem::path(userTempDir) / "info.mumble.Mumble";
		}
#	endif
		if (runtimeDir.empty()) {
			runtimeDir = std::filesystem::path("/run/user") / std::to_string(getuid()) / "info.mumble.Mumble";
		}
	}

	// A failure is not an error here: the caller notices when it can't use the endpoint.
	std::error_code ec;
	std::filesystem::create_directories(runtimeDir, ec);

	return runtimeDir;
#endif
}

std::filesystem::path getOverlayPipePath() {
#ifdef _WIN32
	return "MumbleOverlayPipe";
#else
	return getRuntimeDirectory() / "MumbleOverlayPipe";
#endif
}

#ifdef _WIN32
std::wstring getOverlayPipeDevicePath() {
	return LR"(\\.\pipe\)" + getOverlayPipePath().wstring();
}
#endif

std::filesystem::path getSocketPath(std::string_view basename) {
#ifdef _WIN32
	return basename;
#else
	return getRuntimeDirectory() / (std::string(basename) + "Socket");
#endif
}

} // namespace Mumble
