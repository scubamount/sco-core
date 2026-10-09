-- notebook: the sco SDK's storage example.
--
-- sco.store is the plugin's own database (the host service sco.storage): key-value and SQL,
-- kept across launches, invisible to other plugins. Calls return their value, or nil, the
-- error name ("bad_arg", "unavailable", ...) and the host's message. On a host without
-- storage every call answers "unavailable".

local store = sco.store

-- Raises with the error name and message: a command that raises fails with that reply.
local function check(value, err, msg)
  if err ~= nil then error("storage: " .. err .. (msg and (": " .. msg) or ""), 2) end
  return value
end

local function notes(n) return n .. (n == 1 and " note" or " notes") end

local function count()
  return tonumber(check(store.get("count")) or "0")
end

-- notebook.add <text>: keeps a note. The note and the counter change together or not at all.
sco.register_command{
  name  = "notebook.add",
  title = "Add a note",
  args  = {{ name = "text", type = "string", help = "The note" }},
  fn = function(text)
    local n = count() + 1
    check(store.begin())
    local ok, err, msg = store.put(string.format("note.%06d", n), text)
    if ok then ok, err, msg = store.put("count", tostring(n)) end
    if not ok then
      store.rollback()
      check(nil, err, msg)
    end
    check(store.commit())
    return "note " .. n .. " saved"
  end,
}

-- notebook.list: every note, oldest first (keys sort by their bytes: note.000001, ...).
sco.register_command{
  name  = "notebook.list",
  title = "List notes",
  fn = function()
    local list = {}
    for _, key in ipairs(check(store.keys("note."))) do
      list[#list + 1] = check(store.get(key))
    end
    if #list == 0 then return notes(0) end
    return notes(#list) .. ": " .. table.concat(list, " | ")
  end,
}

-- notebook.clear: removes every note.
sco.register_command{
  name  = "notebook.clear",
  title = "Clear notes",
  fn = function()
    local keys = check(store.keys("note."))
    check(store.begin())
    for _, key in ipairs(keys) do store.delete(key) end
    store.delete("count")
    check(store.commit())
    return "removed " .. notes(#keys)
  end,
}

if store.available() then
  sco.status("Notebook ready")
else
  sco.log("warn", "no storage on this host: notes can't be kept")
end
