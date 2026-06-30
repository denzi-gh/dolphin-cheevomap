"use strict";

const PROTOCOL_VERSION = 1;

const state = {
  protocolVersion: "-",
  sessionId: "0",
  sequence: "0",
  hasCursor: false,
  values: new Map(),
  rows: new Map(),
  filter: "",
  eventSource: null,
};

const els = {
  connection: document.getElementById("connection"),
  protocol: document.getElementById("protocol"),
  session: document.getElementById("session"),
  sequence: document.getElementById("sequence"),
  count: document.getElementById("count"),
  filter: document.getElementById("filter"),
  message: document.getElementById("message"),
  values: document.getElementById("values"),
};

function setConnection(label, className) {
  els.connection.textContent = label;
  els.connection.className = `status ${className}`;
}

function validateMessage(message) {
  if (!message || message.protocol_version !== PROTOCOL_VERSION) {
    setProtocolError("Unsupported protocol.");
    return false;
  }
  return true;
}

function setProtocolError(message) {
  setConnection("Protocol error", "error");
  els.message.textContent = message;
  els.message.hidden = false;
}

function isCanonicalUnsignedDecimal(value) {
  return typeof value === "string" && /^[0-9]+$/.test(value);
}

function normalizeUnsignedDecimalString(value) {
  let offset = 0;
  while (offset + 1 < value.length && value[offset] === "0")
    offset += 1;
  return value.slice(offset);
}

function compareUnsignedDecimalStrings(left, right) {
  if (!isCanonicalUnsignedDecimal(left) || !isCanonicalUnsignedDecimal(right))
    throw new Error("Malformed cursor.");

  const normalizedLeft = normalizeUnsignedDecimalString(left);
  const normalizedRight = normalizeUnsignedDecimalString(right);
  if (normalizedLeft.length < normalizedRight.length)
    return -1;
  if (normalizedLeft.length > normalizedRight.length)
    return 1;
  if (normalizedLeft < normalizedRight)
    return -1;
  if (normalizedLeft > normalizedRight)
    return 1;
  return 0;
}

function compareCursor(leftSession, leftSequence, rightSession, rightSequence) {
  const sessionCompare = compareUnsignedDecimalStrings(leftSession, rightSession);
  if (sessionCompare !== 0)
    return sessionCompare;
  return compareUnsignedDecimalStrings(leftSequence, rightSequence);
}

function validateCursor(message) {
  if (!isCanonicalUnsignedDecimal(message.session_id) ||
      !isCanonicalUnsignedDecimal(message.sequence)) {
    setProtocolError("Malformed state cursor.");
    return null;
  }

  return {
    sessionId: message.session_id,
    sequence: message.sequence,
  };
}

function formatValue(value) {
  if (!value || value.kind === "unavailable")
    return "Unavailable";
  if (value.kind === "bool")
    return value.value ? "true" : "false";
  if (value.kind === "s64" || value.kind === "u64" || value.kind === "string")
    return String(value.value);
  if (value.kind === "f64")
    return Number.isFinite(value.value) ? String(value.value) : "Unavailable";
  return "";
}

function updateSummary() {
  els.protocol.textContent = state.protocolVersion;
  els.session.textContent = state.sessionId;
  els.sequence.textContent = state.sequence;
  els.count.textContent = String(state.values.size);
}

function matchesFilter(id) {
  return state.filter === "" || id.toLowerCase().includes(state.filter);
}

function createCell(text) {
  const cell = document.createElement("td");
  cell.textContent = text;
  return cell;
}

function updateRow(id) {
  const value = state.values.get(id);
  const visible = matchesFilter(id);
  let row = state.rows.get(id);

  if (!visible) {
    if (row)
      row.hidden = true;
    return;
  }

  if (!row) {
    row = document.createElement("tr");
    row.appendChild(createCell(id));
    row.appendChild(createCell(""));
    row.appendChild(createCell(""));
    state.rows.set(id, row);
  }

  row.children[1].textContent = value.kind;
  row.children[2].textContent = formatValue(value);
  row.hidden = false;
}

function sortRows() {
  for (const id of Array.from(state.values.keys()).sort()) {
    const row = state.rows.get(id);
    if (row && row.parentNode !== els.values)
      els.values.appendChild(row);
    else if (row)
      els.values.appendChild(row);
  }
}

function refreshMessage() {
  let visibleCount = 0;
  for (const id of state.values.keys()) {
    if (matchesFilter(id))
      visibleCount += 1;
  }

  if (state.values.size === 0) {
    els.message.textContent = "No values are currently available.";
    els.message.hidden = false;
  } else if (visibleCount === 0) {
    els.message.textContent = "No values match the current filter.";
    els.message.hidden = false;
  } else {
    els.message.hidden = true;
  }
}

function replaceValues(values) {
  state.values = new Map(Object.entries(values || {}));
  for (const [id, row] of state.rows) {
    if (!state.values.has(id)) {
      row.remove();
      state.rows.delete(id);
    }
  }
  for (const id of state.values.keys())
    updateRow(id);
  sortRows();
  refreshMessage();
}

function applySnapshot(message) {
  if (!validateMessage(message))
    return;
  const cursor = validateCursor(message);
  if (!cursor)
    return;

  if (state.hasCursor &&
      compareCursor(cursor.sessionId, cursor.sequence, state.sessionId, state.sequence) < 0) {
    return;
  }

  state.protocolVersion = String(message.protocol_version);
  state.sessionId = cursor.sessionId;
  state.sequence = cursor.sequence;
  state.hasCursor = true;
  replaceValues(message.values);
  updateSummary();
}

function applyUpdate(message) {
  if (!validateMessage(message))
    return;
  const cursor = validateCursor(message);
  if (!cursor)
    return;

  if (state.hasCursor) {
    const sessionCompare = compareUnsignedDecimalStrings(cursor.sessionId, state.sessionId);
    if (sessionCompare < 0)
      return;
    if (sessionCompare === 0 &&
        compareUnsignedDecimalStrings(cursor.sequence, state.sequence) <= 0) {
      return;
    }
    if (sessionCompare > 0 && !message.full) {
      setConnection("Resynchronizing", "reconnecting");
      els.message.textContent = "Resynchronizing dashboard state.";
      els.message.hidden = false;
      fetchSnapshot();
      return;
    }
  } else if (!message.full) {
    setConnection("Resynchronizing", "reconnecting");
    els.message.textContent = "Resynchronizing dashboard state.";
    els.message.hidden = false;
    fetchSnapshot();
    return;
  }

  state.protocolVersion = String(message.protocol_version);
  state.sessionId = cursor.sessionId;
  state.sequence = cursor.sequence;
  state.hasCursor = true;

  if (message.full) {
    replaceValues(message.values);
  } else {
    for (const id of message.removed || []) {
      state.values.delete(id);
      const row = state.rows.get(id);
      if (row) {
        row.remove();
        state.rows.delete(id);
      }
    }
    for (const [id, value] of Object.entries(message.values || {})) {
      state.values.set(id, value);
      updateRow(id);
    }
    sortRows();
    refreshMessage();
  }

  updateSummary();
}

async function fetchSnapshot() {
  try {
    const response = await fetch("/api/v1/snapshot", {cache: "no-store"});
    if (!response.ok) {
      setConnection("Snapshot failed", "error");
      return;
    }
    applySnapshot(await response.json());
  } catch (_) {
    setConnection("Snapshot failed", "error");
  }
}

function connectEvents() {
  if (state.eventSource)
    state.eventSource.close();

  setConnection("Reconnecting", "reconnecting");
  state.eventSource = new EventSource("/api/v1/events");

  state.eventSource.addEventListener("open", () => {
    setConnection("Connected", "connected");
  });
  state.eventSource.addEventListener("snapshot", event => {
    try {
      applySnapshot(JSON.parse(event.data));
      setConnection("Connected", "connected");
    } catch (_) {
      setProtocolError("Malformed snapshot message.");
    }
  });
  state.eventSource.addEventListener("update", event => {
    try {
      applyUpdate(JSON.parse(event.data));
      if (els.connection.className !== "status error" &&
          els.connection.textContent !== "Resynchronizing")
        setConnection("Connected", "connected");
    } catch (_) {
      setProtocolError("Malformed update message.");
    }
  });
  state.eventSource.addEventListener("error", () => {
    setConnection("Reconnecting", "reconnecting");
  });
}

els.filter.addEventListener("input", () => {
  state.filter = els.filter.value.trim().toLowerCase();
  for (const id of state.values.keys())
    updateRow(id);
  sortRows();
  refreshMessage();
});

window.addEventListener("beforeunload", () => {
  if (state.eventSource)
    state.eventSource.close();
});

setConnection("Disconnected", "disconnected");
updateSummary();
fetchSnapshot();
connectEvents();
