"use strict";

const PROTOCOL_VERSION = 1;

const ApplyResult = Object.freeze({
  Applied: "applied",
  Ignored: "ignored",
  Error: "error",
  ResyncRequested: "resync-requested",
});

const state = {
  protocolVersion: "-",
  sessionId: "0",
  sequence: "0",
  hasCursor: false,
  values: new Map(),
  rows: new Map(),
  filter: "",
  eventSource: null,
  connectionMode: "disconnected",
};

let snapshotFetchInFlight = false;

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

function setConnection(label, className, mode = className) {
  els.connection.textContent = label;
  els.connection.className = `status ${className}`;
  state.connectionMode = mode;
}

function setResynchronizing() {
  setConnection("Resynchronizing", "reconnecting", "resynchronizing");
  els.message.textContent = "Resynchronizing dashboard state.";
  els.message.hidden = false;
}

function validateMessage(message) {
  if (!message || message.protocol_version !== PROTOCOL_VERSION) {
    setProtocolError("Unsupported protocol.");
    return false;
  }
  return true;
}

function setProtocolError(message) {
  setConnection("Protocol error", "error", "protocol-error");
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
  // Snapshot/update application returns a result so connection events never
  // infer success from DOM text or CSS state.
  if (!validateMessage(message))
    return ApplyResult.Error;
  const cursor = validateCursor(message);
  if (!cursor)
    return ApplyResult.Error;

  if (state.hasCursor &&
      compareCursor(cursor.sessionId, cursor.sequence, state.sessionId, state.sequence) < 0) {
    return ApplyResult.Ignored;
  }

  state.protocolVersion = String(message.protocol_version);
  state.sessionId = cursor.sessionId;
  state.sequence = cursor.sequence;
  state.hasCursor = true;
  replaceValues(message.values);
  updateSummary();
  return ApplyResult.Applied;
}

function applyUpdate(message) {
  if (!validateMessage(message))
    return ApplyResult.Error;
  const cursor = validateCursor(message);
  if (!cursor)
    return ApplyResult.Error;

  if (state.hasCursor) {
    const sessionCompare = compareUnsignedDecimalStrings(cursor.sessionId, state.sessionId);
    if (sessionCompare < 0)
      return ApplyResult.Ignored;
    if (sessionCompare === 0 &&
        compareUnsignedDecimalStrings(cursor.sequence, state.sequence) <= 0) {
      return ApplyResult.Ignored;
    }
    if (sessionCompare > 0 && !message.full) {
      setResynchronizing();
      fetchSnapshot();
      return ApplyResult.ResyncRequested;
    }
  } else if (!message.full) {
    setResynchronizing();
    fetchSnapshot();
    return ApplyResult.ResyncRequested;
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
  return ApplyResult.Applied;
}

async function fetchSnapshot() {
  if (snapshotFetchInFlight)
    return;

  snapshotFetchInFlight = true;
  try {
    const response = await fetch("/api/v1/snapshot", {cache: "no-store"});
    if (!response.ok) {
      setConnection("Snapshot failed", "error");
      return;
    }

    let message;
    try {
      message = await response.json();
    } catch (_) {
      setProtocolError("Malformed snapshot message.");
      return;
    }

    const result = applySnapshot(message);
    if (result === ApplyResult.Applied) {
      if (state.eventSource && state.eventSource.readyState === EventSource.OPEN)
        setConnection("Connected", "connected");
      else
        setConnection("Synchronized", "connected");
    }
  } catch (_) {
    setConnection("Snapshot failed", "error");
  } finally {
    snapshotFetchInFlight = false;
  }
}

function connectEvents() {
  if (state.eventSource)
    state.eventSource.close();

  setConnection("Reconnecting", "reconnecting");
  state.eventSource = new EventSource("/api/v1/events");

  state.eventSource.addEventListener("snapshot", event => {
    let result = ApplyResult.Error;
    try {
      result = applySnapshot(JSON.parse(event.data));
    } catch (_) {
      setProtocolError("Malformed snapshot message.");
      return;
    }
    if (result === ApplyResult.Applied || result === ApplyResult.Ignored)
      setConnection("Connected", "connected");
  });
  state.eventSource.addEventListener("update", event => {
    let result = ApplyResult.Error;
    try {
      result = applyUpdate(JSON.parse(event.data));
    } catch (_) {
      setProtocolError("Malformed update message.");
      return;
    }
    if (result === ApplyResult.Applied || result === ApplyResult.Ignored)
      setConnection("Connected", "connected");
  });
  state.eventSource.addEventListener("error", () => {
    if (state.connectionMode === "protocol-error" || state.connectionMode === "resynchronizing")
      return;
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
