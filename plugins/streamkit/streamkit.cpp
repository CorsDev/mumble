// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Serves a StreamKit-like voice overlay (for use as a browser source in e.g. OBS) that shows the users in the local
// user's channel, their avatars and who is currently talking.

#include "MumblePlugin.h"
#include "Pages.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int defaultPort         = 47830;
constexpr const char *portEnvName = "MUMBLE_STREAMKIT_PORT";

struct Avatar {
	std::string data;
	std::string mimeType;
	unsigned int revision;
};

mumble_api_t mumbleAPI;
mumble_plugin_id_t ownID;

// The state below is written from Mumble's main thread (plugin callbacks) and read by the HTTP server's worker
// threads. The workers never call the Mumble API themselves: API calls from other threads block until the main thread
// executes them, which would deadlock when mumble_shutdown waits for the workers to finish.
std::mutex stateMutex;
std::condition_variable stateChanged;
std::uint64_t stateVersion = 0;
bool shuttingDown          = false;
bool synchronized          = false;
std::string channelName;
std::vector< mumble_userid_t > channelUsers;
std::unordered_map< mumble_userid_t, std::string > userNames;
std::unordered_set< mumble_userid_t > talkingUsers;
std::unordered_map< mumble_userid_t, Avatar > avatars;
// Users whose avatar is known, including those that don't have one
std::unordered_set< mumble_userid_t > fetchedAvatars;
unsigned int nextAvatarRevision = 1;

std::unique_ptr< httplib::Server > server;
std::thread serverThread;
int serverPort = defaultPort;

void log(const std::string &message) {
	mumbleAPI.log(ownID, message.c_str());
}

// Must be called with stateMutex held
void notifyStateChanged() {
	++stateVersion;
	stateChanged.notify_all();
}

// Must be called with stateMutex held
std::string stateToJson() {
	nlohmann::json users = nlohmann::json::array();

	for (mumble_userid_t id : channelUsers) {
		const auto nameIt   = userNames.find(id);
		const auto avatarIt = avatars.find(id);

		nlohmann::json user = {
			{ "id", id },
			{ "name", nameIt != userNames.end() ? nameIt->second : std::string() },
			{ "talking", talkingUsers.count(id) > 0 },
			{ "avatar", nullptr },
		};
		if (avatarIt != avatars.end()) {
			user["avatar"] = "/avatar/" + std::to_string(id) + "?v=" + std::to_string(avatarIt->second.revision);
		}

		users.push_back(std::move(user));
	}

	const nlohmann::json state = {
		{ "connected", synchronized },
		{ "channel", channelName },
		{ "users", std::move(users) },
	};

	return state.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string mimeTypeForFormat(const std::string &format) {
	if (format == "png") {
		return "image/png";
	} else if (format == "jpeg" || format == "jpg") {
		return "image/jpeg";
	} else if (format == "gif") {
		return "image/gif";
	} else if (format == "bmp") {
		return "image/bmp";
	}

	return "application/octet-stream";
}

void clearState() {
	std::lock_guard< std::mutex > lock(stateMutex);

	synchronized = false;
	channelName.clear();
	channelUsers.clear();
	userNames.clear();
	talkingUsers.clear();
	avatars.clear();
	fetchedAvatars.clear();

	notifyStateChanged();
}

void fetchAvatar(mumble_connection_t connection, mumble_userid_t userID) {
	const uint8_t *texture = nullptr;
	size_t textureSize     = 0;
	const char *format     = nullptr;

	if (mumbleAPI.getUserTexture(ownID, connection, userID, &texture, &textureSize, &format) != MUMBLE_STATUS_OK) {
		// Either the texture is still being synchronized (mumble_onUserTextureChanged will be called once it has
		// arrived) or the user is gone
		return;
	}

	std::lock_guard< std::mutex > lock(stateMutex);

	fetchedAvatars.insert(userID);

	if (texture) {
		avatars[userID] = { std::string(reinterpret_cast< const char * >(texture), textureSize),
							mimeTypeForFormat(format), nextAvatarRevision++ };

		mumbleAPI.freeMemory(ownID, texture);
		mumbleAPI.freeMemory(ownID, format);
	} else {
		avatars.erase(userID);
	}

	notifyStateChanged();
}

void refreshChannel(mumble_connection_t connection) {
	mumble_userid_t localUser;
	if (mumbleAPI.getLocalUserID(ownID, connection, &localUser) != MUMBLE_STATUS_OK) {
		return;
	}

	mumble_channelid_t channel;
	if (mumbleAPI.getChannelOfUser(ownID, connection, localUser, &channel) != MUMBLE_STATUS_OK) {
		return;
	}

	std::string name;
	const char *rawName = nullptr;
	if (mumbleAPI.getChannelName(ownID, connection, channel, &rawName) == MUMBLE_STATUS_OK) {
		name = rawName;
		mumbleAPI.freeMemory(ownID, rawName);
	}

	mumble_userid_t *rawUsers = nullptr;
	size_t userCount          = 0;
	if (mumbleAPI.getUsersInChannel(ownID, connection, channel, &rawUsers, &userCount) != MUMBLE_STATUS_OK) {
		return;
	}

	std::vector< mumble_userid_t > users(rawUsers, rawUsers + userCount);
	if (rawUsers) {
		mumbleAPI.freeMemory(ownID, rawUsers);
	}

	std::unordered_map< mumble_userid_t, std::string > names;
	for (mumble_userid_t id : users) {
		const char *userName = nullptr;
		if (mumbleAPI.getUserName(ownID, connection, id, &userName) == MUMBLE_STATUS_OK) {
			names[id] = userName;
			mumbleAPI.freeMemory(ownID, userName);
		}
	}

	std::vector< mumble_userid_t > missingAvatars;
	{
		std::lock_guard< std::mutex > lock(stateMutex);

		channelName  = std::move(name);
		channelUsers = users;
		userNames    = std::move(names);

		for (mumble_userid_t id : users) {
			if (fetchedAvatars.count(id) == 0) {
				missingAvatars.push_back(id);
			}
		}

		notifyStateChanged();
	}

	for (mumble_userid_t id : missingAvatars) {
		fetchAvatar(connection, id);
	}
}

int readPort() {
	std::string value;
#ifdef _WIN32
	char *buffer  = nullptr;
	size_t length = 0;
	if (_dupenv_s(&buffer, &length, portEnvName) == 0 && buffer) {
		value = buffer;
	}
	free(buffer);
#else
	if (const char *env = std::getenv(portEnvName)) {
		value = env;
	}
#endif

	if (value.empty()) {
		return defaultPort;
	}

	try {
		const int port = std::stoi(value);
		if (port > 0 && port <= std::numeric_limits< std::uint16_t >::max()) {
			return port;
		}
	} catch (const std::exception &) {
	}

	log(std::string("Ignoring invalid port in ") + portEnvName + ": " + value);

	return defaultPort;
}

void setupRoutes(httplib::Server &srv) {
	const std::string port = std::to_string(serverPort);

	// Rejecting foreign Host headers protects against DNS rebinding, through which arbitrary websites could otherwise
	// read the overlay's data
	srv.set_pre_routing_handler(
		[allowedHosts = std::array< std::string, 2 >{ "127.0.0.1:" + port, "localhost:" + port }](
			const httplib::Request &req, httplib::Response &res) {
			const std::string host = req.get_header_value("Host");
			if (host != allowedHosts[0] && host != allowedHosts[1]) {
				res.status = httplib::StatusCode::Forbidden_403;
				return httplib::Server::HandlerResponse::Handled;
			}

			return httplib::Server::HandlerResponse::Unhandled;
		});

	srv.Get("/", [](const httplib::Request &, httplib::Response &res) {
		res.set_content(Pages::config, "text/html; charset=utf-8");
	});

	srv.Get("/overlay", [](const httplib::Request &, httplib::Response &res) {
		res.set_content(Pages::overlay, "text/html; charset=utf-8");
	});

	srv.Get("/state", [](const httplib::Request &, httplib::Response &res) {
		std::string body;
		{
			std::lock_guard< std::mutex > lock(stateMutex);
			body = stateToJson();
		}

		res.set_header("Cache-Control", "no-store");
		res.set_content(body, "application/json");
	});

	srv.Get("/events", [](const httplib::Request &, httplib::Response &res) {
		res.set_header("Cache-Control", "no-store");
		res.set_chunked_content_provider("text/event-stream", [sentVersion = std::optional< std::uint64_t >()](
																  size_t, httplib::DataSink &sink) mutable {
			std::unique_lock< std::mutex > lock(stateMutex);
			stateChanged.wait_for(lock, std::chrono::seconds(15),
								  [&sentVersion]() { return shuttingDown || sentVersion != stateVersion; });

			if (shuttingDown) {
				return false;
			}

			std::string message;
			if (sentVersion != stateVersion) {
				sentVersion = stateVersion;
				message     = "data: " + stateToJson() + "\n\n";
			} else {
				// A comment line that keeps idle connections from timing out
				message = ": keep-alive\n\n";
			}
			lock.unlock();

			return sink.write(message.data(), message.size());
		});
	});

	srv.Get(R"(/avatar/(\d{1,10}))", [](const httplib::Request &req, httplib::Response &res) {
		const unsigned long long id = std::stoull(req.matches[1].str());

		std::lock_guard< std::mutex > lock(stateMutex);

		const auto it = id <= std::numeric_limits< mumble_userid_t >::max()
							? avatars.find(static_cast< mumble_userid_t >(id))
							: avatars.end();
		if (it == avatars.end()) {
			res.status = httplib::StatusCode::NotFound_404;
			return;
		}

		res.set_header("Cache-Control", "no-cache");
		res.set_header("X-Content-Type-Options", "nosniff");
		res.set_content(it->second.data, it->second.mimeType);
	});
}

bool startServer() {
	server = std::make_unique< httplib::Server >();
	// Keeps the shutdown from waiting for idle keep-alive connections for long
	server->set_keep_alive_timeout(1);
	setupRoutes(*server);

	if (!server->bind_to_port("127.0.0.1", serverPort)) {
		server.reset();
		return false;
	}

	serverThread = std::thread([]() { server->listen_after_bind(); });

	return true;
}

void stopServer() {
	{
		std::lock_guard< std::mutex > lock(stateMutex);
		shuttingDown = true;
	}
	stateChanged.notify_all();

	if (server) {
		server->stop();
	}
	if (serverThread.joinable()) {
		serverThread.join();
	}
	server.reset();
}

} // namespace

mumble_error_t mumble_init(mumble_plugin_id_t id) {
	ownID = id;

	{
		std::lock_guard< std::mutex > lock(stateMutex);
		shuttingDown = false;
	}
	clearState();

	serverPort = readPort();
	if (!startServer()) {
		log("Unable to listen on port " + std::to_string(serverPort) + ". Choose another one via the " + portEnvName
			+ " environment variable.");

		return MUMBLE_EC_GENERIC_ERROR;
	}

	log("Open http://127.0.0.1:" + std::to_string(serverPort) + "/ to set up the overlay");

	// The plugin may have been enabled while already being connected to a server
	mumble_connection_t connection;
	bool isSynchronized = false;
	if (mumbleAPI.getActiveServerConnection(ownID, &connection) == MUMBLE_STATUS_OK
		&& mumbleAPI.isConnectionSynchronized(ownID, connection, &isSynchronized) == MUMBLE_STATUS_OK
		&& isSynchronized) {
		mumble_onServerSynchronized(connection);
	}

	return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
	stopServer();
}

MumbleStringWrapper mumble_getName() {
	static const char *name = "StreamKit Overlay";

	MumbleStringWrapper wrapper;
	wrapper.data           = name;
	wrapper.size           = std::strlen(name);
	wrapper.needsReleasing = false;

	return wrapper;
}

mumble_version_t mumble_getAPIVersion() {
	return MUMBLE_PLUGIN_API_VERSION;
}

void mumble_registerAPIFunctions(void *apiStruct) {
	mumbleAPI = MUMBLE_API_CAST(apiStruct);
}

void mumble_releaseResource(const void *pointer) {
	// This function should never be called
	(void) pointer;

	std::terminate();
}

mumble_version_t mumble_getVersion() {
	return { 1, 0, 0 };
}

MumbleStringWrapper mumble_getAuthor() {
	static const char *author = "Mumble Developers";

	MumbleStringWrapper wrapper;
	wrapper.data           = author;
	wrapper.size           = std::strlen(author);
	wrapper.needsReleasing = false;

	return wrapper;
}

MumbleStringWrapper mumble_getDescription() {
	static const char *description =
		"Serves a voice overlay for streaming software (e.g. as an OBS browser source) that shows the users in your "
		"channel, their avatars and who is talking";

	MumbleStringWrapper wrapper;
	wrapper.data           = description;
	wrapper.size           = std::strlen(description);
	wrapper.needsReleasing = false;

	return wrapper;
}

void mumble_onServerSynchronized(mumble_connection_t connection) {
	{
		std::lock_guard< std::mutex > lock(stateMutex);
		synchronized = true;
	}

	refreshChannel(connection);
}

void mumble_onServerDisconnected(mumble_connection_t connection) {
	(void) connection;

	clearState();
}

void mumble_onChannelEntered(mumble_connection_t connection, mumble_userid_t userID,
							 mumble_channelid_t previousChannelID, mumble_channelid_t newChannelID) {
	(void) userID;
	(void) previousChannelID;
	(void) newChannelID;

	if (synchronized) {
		refreshChannel(connection);
	}
}

void mumble_onChannelExited(mumble_connection_t connection, mumble_userid_t userID, mumble_channelid_t channelID) {
	(void) userID;
	(void) channelID;

	if (synchronized) {
		refreshChannel(connection);
	}
}

void mumble_onChannelRenamed(mumble_connection_t connection, mumble_channelid_t channelID) {
	(void) channelID;

	if (synchronized) {
		refreshChannel(connection);
	}
}

void mumble_onUserRemoved(mumble_connection_t connection, mumble_userid_t userID) {
	{
		std::lock_guard< std::mutex > lock(stateMutex);

		talkingUsers.erase(userID);
		avatars.erase(userID);
		fetchedAvatars.erase(userID);
	}

	if (synchronized) {
		refreshChannel(connection);
	}
}

void mumble_onUserTalkingStateChanged(mumble_connection_t connection, mumble_userid_t userID,
									  mumble_talking_state_t talkingState) {
	(void) connection;

	const bool talking =
		talkingState == MUMBLE_TS_TALKING || talkingState == MUMBLE_TS_WHISPERING || talkingState == MUMBLE_TS_SHOUTING;

	std::lock_guard< std::mutex > lock(stateMutex);

	const bool changed = talking ? talkingUsers.insert(userID).second : talkingUsers.erase(userID) > 0;
	if (changed) {
		notifyStateChanged();
	}
}

void mumble_onUserTextureChanged(mumble_connection_t connection, mumble_userid_t userID) {
	bool inChannel;
	{
		std::lock_guard< std::mutex > lock(stateMutex);

		fetchedAvatars.erase(userID);
		inChannel = std::find(channelUsers.begin(), channelUsers.end(), userID) != channelUsers.end();
	}

	// Avatars of users outside of the channel are fetched once they join it, which avoids requesting the textures
	// of everyone on the server
	if (synchronized && inChannel) {
		fetchAvatar(connection, userID);
	}
}
