// ContextSnap tab bridge — MV3 service worker.
//
// Protocol (v1, see schemas/native-messaging.schema.json):
//
//   host -> extension : hello | collect_tabs | restore_tabs | ping | config | error
//   extension -> host : hello | tabs | restore_ack | error | pong | browser_info
//
// The browser frames every message with a 4-byte little-endian length prefix;
// we only deal with JSON objects of the shape { type, payload }.
//
// Design notes
// * The worker stays event-driven: it reconnects lazily and keeps a single
//   long-lived port, because MV3 workers are terminated aggressively.
// * Restored tabs are created discarded ("lazy") whenever the browser allows
//   it, so reopening 200 tabs does not spawn 200 renderers.
// * Nothing is ever sent anywhere except to the local native host.

const HOST_ID = "com.contextsnap.native_host";
const PROTOCOL_VERSION = 1;
const KEEPALIVE_ALARM = "contextsnap-keepalive";
const RECONNECT_BASE_MS = 1000;
const RECONNECT_MAX_MS = 60000;

const api = typeof browser !== "undefined" ? browser : chrome;
const isFirefox = typeof browser !== "undefined" && Boolean(browser.runtime.getBrowserInfo);

let port = null;
let reconnectDelay = RECONNECT_BASE_MS;
let reconnectTimer = null;

const defaultSettings = {
  enabled: true,
  includeIncognito: false,
  includeTabTitles: true,
  restoreLazily: true,
};

async function settings() {
  const stored = await api.storage.local.get(defaultSettings);
  return { ...defaultSettings, ...stored };
}

function browserName() {
  if (isFirefox) {
    return "firefox";
  }
  const agent = navigator.userAgent;
  if (agent.includes("Edg/")) return "edge";
  if (agent.includes("OPR/")) return "opera";
  if (agent.includes("Brave")) return "brave";
  if (agent.includes("Vivaldi")) return "vivaldi";
  if (agent.includes("Chrome/")) return "chrome";
  return "chromium";
}

/// Chromium reports window state as a string already; normalise Firefox's.
function windowState(window) {
  switch (window.state) {
    case "maximized":
    case "minimized":
    case "fullscreen":
    case "docked":
      return window.state;
    default:
      return "normal";
  }
}

async function groupTitles() {
  // tabGroups is Chromium-only and may be unavailable in Firefox.
  if (!api.tabGroups || !api.tabGroups.query) {
    return new Map();
  }
  try {
    const groups = await api.tabGroups.query({});
    return new Map(groups.map((group) => [group.id, group.title || ""]));
  } catch (error) {
    return new Map();
  }
}

function serialiseTab(tab, groups, config) {
  const payload = {
    id: String(tab.id),
    index: tab.index,
    url: tab.pendingUrl || tab.url || "",
    title: config.includeTabTitles ? tab.title || "" : "",
    pinned: Boolean(tab.pinned),
    active: Boolean(tab.active),
    audible: Boolean(tab.audible),
    muted: Boolean(tab.mutedInfo && tab.mutedInfo.muted),
    discarded: Boolean(tab.discarded),
    incognito: Boolean(tab.incognito),
  };
  if (typeof tab.openerTabId === "number") {
    payload.opener_index = tab.openerTabId;
  }
  if (tab.cookieStoreId) {
    payload.cookieStoreId = tab.cookieStoreId;
  }
  if (typeof tab.groupId === "number" && tab.groupId >= 0 && groups.has(tab.groupId)) {
    payload.groupTitle = groups.get(tab.groupId);
  }
  return payload;
}

async function collectTabs(requestId) {
  const config = await settings();
  const groups = await groupTitles();
  const windows = await api.windows.getAll({ populate: true });

  const payloadWindows = [];
  for (const window of windows) {
    if (window.incognito && !config.includeIncognito) {
      continue;
    }
    if (window.type && window.type !== "normal" && window.type !== "popup") {
      continue;  // Skip devtools and app windows.
    }
    payloadWindows.push({
      id: String(window.id),
      focused: Boolean(window.focused),
      incognito: Boolean(window.incognito),
      state: windowState(window),
      left: window.left ?? 0,
      top: window.top ?? 0,
      width: window.width ?? 0,
      height: window.height ?? 0,
      tabs: (window.tabs || []).map((tab) => serialiseTab(tab, groups, config)),
    });
  }

  send("tabs", {
    requestId: requestId ?? null,
    browser: browserName(),
    profile: "default",
    protocol: PROTOCOL_VERSION,
    windows: payloadWindows,
  });
}

/// Creates one browser window per snapshot window, then fills it with tabs.
async function restoreTabs(payload) {
  const config = await settings();
  const lazy = payload.lazyLoad !== false && config.restoreLazily;
  let restored = 0;

  for (const window of payload.windows || []) {
    const tabs = (window.tabs || []).slice().sort((a, b) => (a.index || 0) - (b.index || 0));
    if (tabs.length === 0) {
      continue;
    }

    const createData = {
      url: tabs[0].url,
      focused: false,
      incognito: Boolean(window.incognito) && config.includeIncognito,
    };
    if (window.state === "normal") {
      // Only positioned windows accept explicit geometry.
      createData.left = window.left ?? undefined;
      createData.top = window.top ?? undefined;
      createData.width = window.width ?? undefined;
      createData.height = window.height ?? undefined;
    } else if (window.state) {
      createData.state = window.state;
    }

    let created;
    try {
      created = await api.windows.create(createData);
      restored += 1;
    } catch (error) {
      send("error", { message: `window create failed: ${error}`, requestId: payload.requestId });
      continue;
    }

    for (const tab of tabs.slice(1)) {
      try {
        const options = {
          windowId: created.id,
          url: tab.url,
          index: tab.index,
          pinned: Boolean(tab.pinned),
          active: false,
        };
        // Chromium honours `active: false` plus discard; Firefox has
        // `discarded` + `title` on tabs.create.
        if (lazy && isFirefox) {
          options.discarded = true;
          if (tab.title) {
            options.title = tab.title;
          }
        }
        const createdTab = await api.tabs.create(options);
        restored += 1;
        if (lazy && !isFirefox && api.tabs.discard) {
          // Discard after creation; Chromium cannot create a discarded tab.
          api.tabs.discard(createdTab.id).catch(() => {});
        }
        if (tab.muted && api.tabs.update) {
          api.tabs.update(createdTab.id, { muted: true }).catch(() => {});
        }
      } catch (error) {
        send("error", { message: `tab create failed: ${error}`, requestId: payload.requestId });
      }
    }

    const activeTab = tabs.find((tab) => tab.active);
    if (activeTab && api.tabs.query) {
      try {
        const candidates = await api.tabs.query({ windowId: created.id, index: activeTab.index });
        if (candidates.length > 0) {
          await api.tabs.update(candidates[0].id, { active: true });
        }
      } catch (error) {
        /* Not fatal: the window is already restored. */
      }
    }
  }

  send("restoreResult", { requestId: payload.requestId ?? null, tabsRestored: restored });
}

function send(type, payload) {
  if (!port) {
    return;
  }
  try {
    port.postMessage({ type, payload });
  } catch (error) {
    console.warn("ContextSnap: send failed", error);
    teardown();
  }
}

async function handleMessage(message) {
  if (!message || typeof message.type !== "string") {
    return;
  }
  const payload = message.payload || {};
  switch (message.type) {
    case "hello":
      send("hello", {
        extension: api.runtime.getManifest().version,
        browser: browserName(),
        protocol: PROTOCOL_VERSION,
        capabilities: {
          tabGroups: Boolean(api.tabGroups),
          discard: Boolean(api.tabs.discard) || isFirefox,
          incognito: (await settings()).includeIncognito,
        },
      });
      break;
    case "collect_tabs":
      await collectTabs(payload.requestId);
      break;
    case "restore_tabs":
      await restoreTabs(payload);
      break;
    case "ping":
      send("pong", { requestId: payload.requestId ?? null });
      break;
    case "config":
      await api.storage.local.set({
        includeIncognito: Boolean(payload.includeIncognito),
        restoreLazily: payload.lazyLoad !== false,
      });
      break;
    case "error":
      console.warn("ContextSnap host error:", payload.message || payload);
      break;
    default:
      send("error", { message: `unknown message type: ${message.type}` });
  }
}

function teardown() {
  if (port) {
    try {
      port.disconnect();
    } catch (error) {
      /* already gone */
    }
  }
  port = null;
}

function scheduleReconnect() {
  if (reconnectTimer) {
    return;
  }
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, reconnectDelay);
  reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX_MS);
}

async function connect() {
  const config = await settings();
  if (!config.enabled || port) {
    return;
  }
  try {
    port = api.runtime.connectNative(HOST_ID);
  } catch (error) {
    console.warn("ContextSnap: native host unavailable", error);
    scheduleReconnect();
    return;
  }

  port.onMessage.addListener((message) => {
    reconnectDelay = RECONNECT_BASE_MS;
    handleMessage(message).catch((error) => {
      send("error", { message: String(error) });
    });
  });

  port.onDisconnect.addListener(() => {
    const error = api.runtime.lastError;
    if (error) {
      console.warn("ContextSnap: host disconnected", error.message);
    }
    port = null;
    scheduleReconnect();
  });

  // Announce ourselves; the daemon replies with its own hello.
  send("hello", {
    extension: api.runtime.getManifest().version,
    browser: browserName(),
    protocol: PROTOCOL_VERSION,
  });
}

api.runtime.onStartup.addListener(() => connect());
api.runtime.onInstalled.addListener(() => {
  api.storage.local.get(defaultSettings).then((stored) => {
    api.storage.local.set({ ...defaultSettings, ...stored });
  });
  connect();
});

// MV3 workers are unloaded when idle; an alarm brings us back so the daemon
// can reach the browser even after a long idle period.
if (api.alarms) {
  api.alarms.create(KEEPALIVE_ALARM, { periodInMinutes: 1 });
  api.alarms.onAlarm.addListener((alarm) => {
    if (alarm.name === KEEPALIVE_ALARM) {
      connect();
    }
  });
}

api.runtime.onMessage.addListener((message, _sender, respond) => {
  if (message && message.type === "contextsnap:status") {
    respond({ connected: Boolean(port), host: HOST_ID, protocol: PROTOCOL_VERSION });
    return true;
  }
  if (message && message.type === "contextsnap:reconnect") {
    teardown();
    reconnectDelay = RECONNECT_BASE_MS;
    connect();
    respond({ ok: true });
    return true;
  }
  return false;
});

connect();
