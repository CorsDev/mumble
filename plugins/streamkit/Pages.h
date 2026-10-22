// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_PLUGIN_STREAMKIT_PAGES_H_
#define MUMBLE_PLUGIN_STREAMKIT_PAGES_H_

// Note: MSVC limits the length of a single string literal to ~16 KB

namespace Pages {

// The overlay itself, meant to be used as a browser source. User names are untrusted (they come from the server) and
// must therefore only ever be inserted via textContent.
constexpr const char *overlay = R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Mumble Voice Overlay</title>
<style>
:root {
	--avatar-size: 50px;
	--avatar-shadow: 0 2px 6px rgba(0, 0, 0, 0.5);
	--ring-color: #43b581;
	--ring-width: 3px;
	--name-background: rgba(30, 33, 36, 0.9);
	--name-color: #ffffff;
	--name-font-size: 14px;
	--name-radius: 3px;
	--name-gap: 8px;
	--user-gap: 8px;
}
html, body {
	margin: 0;
	padding: 0;
	background: transparent;
	overflow: hidden;
}
[hidden] {
	display: none !important;
}
body {
	font-family: "gg sans", "Whitney", "Noto Sans", "Helvetica Neue", Helvetica, Arial, sans-serif;
}
#users {
	display: flex;
	flex-direction: column;
	align-items: flex-start;
	gap: var(--user-gap);
	padding: calc(var(--ring-width) + 8px);
}
.horizontal #users {
	flex-direction: row;
}
.user {
	display: flex;
	flex-direction: row;
	align-items: center;
}
.horizontal .user {
	flex-direction: column;
}
.user.hidden {
	display: none;
}
.avatar {
	flex: none;
	display: flex;
	align-items: center;
	justify-content: center;
	width: var(--avatar-size);
	height: var(--avatar-size);
	border-radius: 50%;
	overflow: hidden;
	box-shadow: 0 0 0 0 var(--ring-color), var(--avatar-shadow);
	transition: box-shadow 0.1s ease-out;
	color: #ffffff;
	font-size: calc(var(--avatar-size) * 0.38);
	font-weight: 700;
}
.talking .avatar {
	box-shadow: 0 0 0 var(--ring-width) var(--ring-color), var(--avatar-shadow);
}
.avatar img {
	display: block;
	width: 100%;
	height: 100%;
	object-fit: cover;
}
.name {
	margin-left: var(--name-gap);
	padding: 4px 6px;
	border-radius: var(--name-radius);
	background: var(--name-background);
	color: var(--name-color);
	font-size: var(--name-font-size);
	font-weight: 700;
	line-height: 1.2;
	white-space: nowrap;
}
.horizontal .name {
	margin-left: 0;
	margin-top: var(--name-gap);
	max-width: calc(var(--avatar-size) * 2.4);
	overflow: hidden;
	text-overflow: ellipsis;
}
.hide-names .name {
	display: none;
}
</style>
</head>
<body>
<div id="users"></div>
<script>
(() => {
	const params = new URLSearchParams(location.search);
	const flag = (name) => ['1', 'true', 'yes'].includes((params.get(name) || '').toLowerCase());
	const number = (name, min, max) => {
		const value = Number.parseInt(params.get(name), 10);
		return Number.isFinite(value) && value >= min && value <= max ? value : null;
	};

	const size = number('size', 16, 256);
	if (size !== null) {
		document.documentElement.style.setProperty('--avatar-size', size + 'px');
	}
	const speakingOnly = flag('speaking_only');
	const fadeMs = number('fade_ms', 0, 600000) ?? 0;
	document.body.classList.toggle('horizontal', params.get('layout') === 'horizontal');
	document.body.classList.toggle('hide-names', flag('hide_names'));

	const list = document.getElementById('users');
	const entries = new Map();

	const initialsOf = (name) => {
		const words = name.trim().split(/\s+/).filter(Boolean);
		if (words.length > 1) {
			return (Array.from(words[0])[0] + Array.from(words[1])[0]).toUpperCase();
		}
		return Array.from(words[0] || '?').slice(0, 2).join('').toUpperCase();
	};
	const hueOf = (name) => {
		let hash = 0;
		for (const char of name) {
			hash = (hash * 31 + char.codePointAt(0)) >>> 0;
		}
		return hash % 360;
	};

	const setVisible = (entry, visible) => {
		entry.visible = visible;
		entry.element.classList.toggle('hidden', !visible);
	};

	const createEntry = () => {
		const element = document.createElement('div');
		element.className = 'user';
		const avatar = document.createElement('div');
		avatar.className = 'avatar';
		const initials = document.createElement('span');
		const image = document.createElement('img');
		image.alt = '';
		image.hidden = true;
		image.addEventListener('load', () => {
			image.hidden = false;
			initials.hidden = true;
		});
		image.addEventListener('error', () => {
			image.hidden = true;
			initials.hidden = false;
		});
		avatar.append(initials, image);
		const name = document.createElement('div');
		name.className = 'name';
		element.append(avatar, name);

		const entry = { element, avatar, initials, image, name, source: null, visible: true, timer: null };
		setVisible(entry, !speakingOnly);
		return entry;
	};

	const updateEntry = (entry, user) => {
		entry.name.textContent = user.name;
		entry.initials.textContent = initialsOf(user.name);
		entry.avatar.style.backgroundColor = 'hsl(' + hueOf(user.name) + ', 45%, 45%)';

		if (user.avatar !== entry.source) {
			entry.source = user.avatar;
			if (user.avatar) {
				entry.image.src = user.avatar;
			} else {
				entry.image.removeAttribute('src');
				entry.image.hidden = true;
				entry.initials.hidden = false;
			}
		}

		entry.element.classList.toggle('talking', user.talking);

		if (speakingOnly) {
			if (user.talking) {
				clearTimeout(entry.timer);
				entry.timer = null;
				setVisible(entry, true);
			} else if (entry.visible && entry.timer === null) {
				entry.timer = setTimeout(() => {
					entry.timer = null;
					setVisible(entry, false);
				}, fadeMs);
			}
		}
	};

	const render = (state) => {
		const users = (state.users || []).slice().sort(
			(a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }) || a.id - b.id);
		const ids = new Set(users.map((user) => user.id));

		for (const [id, entry] of entries) {
			if (!ids.has(id)) {
				clearTimeout(entry.timer);
				entry.element.remove();
				entries.delete(id);
			}
		}

		users.forEach((user, index) => {
			let entry = entries.get(user.id);
			if (!entry) {
				entry = createEntry();
				entries.set(user.id, entry);
			}
			updateEntry(entry, user);
			if (list.children[index] !== entry.element) {
				list.insertBefore(entry.element, list.children[index] || null);
			}
		});
	};

	// EventSource reconnects on its own, e.g. after Mumble has been restarted
	const events = new EventSource('/events');
	events.onmessage = (event) => render(JSON.parse(event.data));
	events.onerror = () => render({ users: [] });
})();
</script>
</body>
</html>
)html";

// Lets the user pick the overlay's options and builds the URL to use in OBS
constexpr const char *config = R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mumble Voice Overlay</title>
<style>
:root {
	color-scheme: dark;
	--background: #1e1f22;
	--surface: #2b2d31;
	--border: #3f4147;
	--text: #f2f3f5;
	--muted: #b5bac1;
	--accent: #43b581;
}
* {
	box-sizing: border-box;
}
body {
	margin: 0;
	background: var(--background);
	color: var(--text);
	font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
}
main {
	max-width: 760px;
	margin: 0 auto;
	padding: 24px 16px 48px;
}
h1 {
	margin: 0 0 4px;
	font-size: 24px;
}
h2 {
	margin: 0 0 12px;
	font-size: 16px;
}
p {
	margin: 0 0 12px;
	color: var(--muted);
}
section {
	margin-top: 20px;
	padding: 16px;
	border: 1px solid var(--border);
	border-radius: 8px;
	background: var(--surface);
}
.status::before {
	content: "";
	display: inline-block;
	width: 8px;
	height: 8px;
	margin-right: 8px;
	border-radius: 50%;
	background: var(--muted);
}
.status.connected::before {
	background: var(--accent);
}
.options {
	display: grid;
	grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
	gap: 12px 16px;
}
label {
	display: flex;
	align-items: center;
	gap: 8px;
}
label.field {
	flex-direction: column;
	align-items: stretch;
	gap: 4px;
	color: var(--muted);
}
input, select, button {
	font: inherit;
	color: var(--text);
}
input[type="number"], input[type="text"], select {
	width: 100%;
	padding: 6px 8px;
	border: 1px solid var(--border);
	border-radius: 4px;
	background: var(--background);
}
input:disabled {
	opacity: 0.5;
}
.url {
	display: flex;
	gap: 8px;
}
button {
	flex: none;
	padding: 6px 14px;
	border: none;
	border-radius: 4px;
	background: var(--accent);
	color: #ffffff;
	font-weight: 600;
	cursor: pointer;
}
iframe {
	display: block;
	width: 100%;
	height: 320px;
	border: 1px solid var(--border);
	border-radius: 4px;
	background: repeating-conic-gradient(#3a3c42 0% 25%, #2f3136 0% 50%) 0 0 / 24px 24px;
	/* Must match the overlay's color scheme, or the browser paints an opaque background behind it */
	color-scheme: normal;
}
code {
	padding: 1px 4px;
	border-radius: 3px;
	background: var(--background);
	font-size: 13px;
}
</style>
</head>
<body>
<main>
	<h1>Mumble Voice Overlay</h1>
	<p id="status" class="status">Waiting for Mumble...</p>

	<section>
		<h2>Options</h2>
		<div class="options">
			<label class="field">Layout
				<select id="layout">
					<option value="vertical">Vertical</option>
					<option value="horizontal">Horizontal</option>
				</select>
			</label>
			<label class="field">Avatar size (px)
				<input id="size" type="number" min="16" max="256" value="50">
			</label>
			<label><input id="names" type="checkbox" checked> Show names</label>
			<label><input id="speaking" type="checkbox"> Only show speaking users</label>
			<label class="field">Hide after (ms)
				<input id="fade" type="number" min="0" max="600000" step="100" value="0" disabled>
			</label>
		</div>
	</section>

	<section>
		<h2>Browser source URL</h2>
		<div class="url">
			<input id="url" type="text" readonly>
			<button id="copy" type="button">Copy</button>
		</div>
		<p style="margin-top: 12px">In OBS, add a <em>Browser</em> source and paste this URL. The overlay's look can be
		adjusted through the source's custom CSS, e.g. <code>:root { --ring-color: #5865f2; --avatar-size: 64px; }</code>.
		Available variables: <code>--avatar-size</code>, <code>--avatar-shadow</code>, <code>--ring-color</code>,
		<code>--ring-width</code>, <code>--name-background</code>, <code>--name-color</code>,
		<code>--name-font-size</code>, <code>--name-radius</code>, <code>--name-gap</code> and
		<code>--user-gap</code>.</p>
	</section>

	<section>
		<h2>Preview</h2>
		<iframe id="preview" title="Overlay preview"></iframe>
	</section>
</main>
<script>
(() => {
	const $ = (id) => document.getElementById(id);
	const layout = $('layout');
	const size = $('size');
	const names = $('names');
	const speaking = $('speaking');
	const fade = $('fade');
	const url = $('url');
	const preview = $('preview');

	const update = () => {
		const params = new URLSearchParams();
		if (layout.value === 'horizontal') {
			params.set('layout', 'horizontal');
		}
		const avatarSize = Number.parseInt(size.value, 10);
		if (Number.isFinite(avatarSize) && avatarSize !== 50) {
			params.set('size', String(Math.min(256, Math.max(16, avatarSize))));
		}
		if (!names.checked) {
			params.set('hide_names', '1');
		}
		fade.disabled = !speaking.checked;
		if (speaking.checked) {
			params.set('speaking_only', '1');
			const fadeMs = Number.parseInt(fade.value, 10);
			if (Number.isFinite(fadeMs) && fadeMs > 0) {
				params.set('fade_ms', String(fadeMs));
			}
		}

		const query = params.toString();
		const overlayUrl = location.origin + '/overlay' + (query ? '?' + query : '');
		url.value = overlayUrl;
		if (preview.src !== overlayUrl) {
			preview.src = overlayUrl;
		}
	};

	for (const input of [layout, size, names, speaking, fade]) {
		input.addEventListener('input', update);
		input.addEventListener('change', update);
	}

	$('copy').addEventListener('click', async () => {
		try {
			await navigator.clipboard.writeText(url.value);
		} catch (error) {
			url.select();
			document.execCommand('copy');
		}
		$('copy').textContent = 'Copied';
		setTimeout(() => {
			$('copy').textContent = 'Copy';
		}, 1500);
	});

	const status = $('status');
	const events = new EventSource('/events');
	events.onmessage = (event) => {
		const state = JSON.parse(event.data);
		status.classList.toggle('connected', state.connected);
		if (!state.connected) {
			status.textContent = 'Mumble is not connected to a server';
			return;
		}
		const count = state.users.length;
		status.textContent = 'Connected to channel "' + state.channel + '" with ' + count
			+ (count === 1 ? ' user' : ' users');
	};
	events.onerror = () => {
		status.classList.remove('connected');
		status.textContent = 'Lost connection to Mumble';
	};

	update();
})();
</script>
</body>
</html>
)html";

} // namespace Pages

#endif // MUMBLE_PLUGIN_STREAMKIT_PAGES_H_
