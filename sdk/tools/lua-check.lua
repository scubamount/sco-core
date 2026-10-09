-- lua-check.lua: runs a Lua plugin's entry script against a stand-in `sco` table, on a dev
-- machine, without the game. Needs a stock Lua 5.4 interpreter.
--
--   lua5.4 tools/lua-check.lua <plugin folder> [--cap NAME]... [--invoke NAME [ARG]...]
--
-- The script runs with the same globals the sandbox gives it (sco, string, table, math, utf8
-- and the safe base functions); anything else, such as io or os, is nil, so a script that
-- reaches for it fails here as it would in the game. Then game.ready, three ticks and
-- game.exit are dispatched and every command without arguments (plus --invoke) is run.
-- Exit 0 = OK, 1 = a check failed, 2 = bad usage.
--
-- It is not the real runtime: there is no instruction budget and no game. Part of the sco SDK.

local dir, caps, invoke_name, invoke_args = nil, {}, nil, {}
do
  local i = 1
  while i <= #arg do
    local a = arg[i]
    if a == "--cap" and arg[i + 1] then caps[arg[i + 1]] = true; i = i + 1
    elseif a == "--invoke" and arg[i + 1] then
      invoke_name = arg[i + 1]
      for k = i + 2, #arg do invoke_args[#invoke_args + 1] = arg[k] end
      break
    elseif not dir and a:sub(1, 1) ~= "-" then dir = a
    else dir = nil; break end
    i = i + 1
  end
end
if not dir then
  io.stderr:write("usage: lua5.4 lua-check.lua <plugin folder> [--cap NAME]... [--invoke NAME [ARG]...]\n")
  os.exit(2)
end

local failures = 0
local function fail(msg) print("FAIL " .. msg); failures = failures + 1 end

-- plugin.ini: only what this check needs (sco-plugin-check validates the rest).
local ini = {}
do
  local f = io.open(dir .. "/plugin.ini", "rb")
  if not f then fail(dir .. "/plugin.ini: can't open"); print("FAILED"); os.exit(1) end
  for line in f:lines() do
    line = line:gsub("^\239\187\191", ""):gsub("\r$", "")
    line = line:gsub("^[;#].*", ""):gsub("[ \t][;#].*", "")
    local k, v = line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
    if k then ini[k] = v end
  end
  f:close()
end
if ini.kind ~= "lua" then fail("plugin.ini: kind is '" .. tostring(ini.kind) .. "', not lua") end
if not ini.id or not ini.entry then fail("plugin.ini: id and entry are required") end
if failures > 0 then print("FAILED: " .. failures .. " failure(s)"); os.exit(1) end
local id = ini.id
print(dir)

-- ---- stand-in sco table -----------------------------------------------------------------

local subs, commands, order = {}, {}, {}
local arg_types = { int = true, float = true, string = true, bool = true }
local levels = { info = true, warn = true, error = true }

local function str_ok(s, max) return type(s) == "string" and #s >= 1 and #s <= max end

local sco = {}
sco.api_major, sco.api_minor = 1, 0
function sco.host_version() return "lua-check 1.0" end
function sco.has(name) return caps[name] == true end
function sco.status(msg) print("  status  " .. id .. ": " .. tostring(msg)) end
function sco.log(level, msg)
  if not levels[level] then fail("sco.log: level must be info, warn or error, got " .. tostring(level)); return end
  print("  log     [" .. id .. "] " .. level .. ": " .. tostring(msg))
end
-- Limits of the real runtime (plugins/lua/sco_lua.h).
local MAX_EVENTS, MAX_SUBSCRIBERS, MAX_TASKS, MAX_DEPTH = 16, 64, 16, 8
local nevents, tasks, depth = 0, {}, 0
function sco.subscribe(event, fn)
  if type(event) ~= "string" or type(fn) ~= "function" then fail("sco.subscribe(event, fn): bad arguments"); return false, "bad_arg" end
  if #event == 0 or #event > 63 then return false, "bad_arg" end
  local list = subs[event]
  if list then
    for _, f in ipairs(list) do if f == fn then return false, "bad_arg" end end   -- same function twice
    if #list >= MAX_SUBSCRIBERS then return false, "too_many" end
  else
    if nevents >= MAX_EVENTS then return false, "too_many" end
    list = {}
    subs[event] = list
    nevents = nevents + 1
  end
  table.insert(list, fn)
  return true
end
function sco.unsubscribe(event, fn)
  local list = subs[event]
  for i, f in ipairs(list or {}) do
    if f == fn then
      table.remove(list, i)
      if #list == 0 then subs[event] = nil; nevents = nevents - 1 end
      return true
    end
  end
  return false, "not_found"
end
function sco.run_on_game_thread(fn)
  if type(fn) ~= "function" then fail("sco.run_on_game_thread: fn must be a function"); return false, "bad_arg" end
  if #tasks >= MAX_TASKS then return false, "too_many" end
  table.insert(tasks, fn)
  return true
end
function sco.register_command(c)
  if type(c) ~= "table" then fail("sco.register_command: takes one table"); return false, "bad_arg" end
  local prefix = id .. "."
  if type(c.name) ~= "string" or c.name:sub(1, #prefix) ~= prefix or #c.name == #prefix or #c.name > 63 then
    fail("sco.register_command: name must be '" .. prefix .. "<action>', at most 63 bytes"); return false, "bad_arg"
  end
  if commands[c.name] then fail("sco.register_command: duplicate " .. c.name); return false, "bad_arg" end
  if not str_ok(c.title, 63) or type(c.fn) ~= "function" then
    fail("sco.register_command " .. c.name .. ": title (1-63 bytes) and fn are required"); return false, "bad_arg"
  end
  if c.help ~= nil and (type(c.help) ~= "string" or #c.help > 255) then fail(c.name .. ": help is at most 255 bytes"); return false, "bad_arg" end
  local args = c.args or {}
  if #args > 16 then fail(c.name .. ": at most 16 args"); return false, "bad_arg" end
  for i, a in ipairs(args) do
    if not str_ok(a.name, 31) or not arg_types[a.type] then
      fail(c.name .. ": arg " .. i .. " needs a name (1-31 bytes) and a type: int, float, string or bool"); return false, "bad_arg"
    end
  end
  commands[c.name] = { def = c, args = args }
  table.insert(order, c.name)
  return true
end
-- As the runtime: arguments must match the command's types exactly (int takes a Lua integer), a
-- failing command gives false, "bad_arg" and its error text, nesting stops at 8.
local function type_ok(ty, v)
  if ty == "int" then return math.type(v) == "integer" end
  if ty == "float" then return type(v) == "number" end
  if ty == "string" then return type(v) == "string" end
  return type(v) == "boolean"
end
function sco.invoke(name, ...)
  if type(name) ~= "string" then error("bad argument #1 to 'invoke' (string expected, got " .. type(name) .. ")", 2) end
  local c = commands[name]
  if not c then return false, "not_found" end
  local n = select("#", ...)
  if n ~= #c.args then return false, "bad_arg" end
  for i, a in ipairs(c.args) do
    if not type_ok(a.type, (select(i, ...))) then return false, "bad_arg" end
  end
  if c.def.capability ~= nil and not caps[c.def.capability] then return false, "unavailable", "" end
  if depth >= MAX_DEPTH then return false, "too_many", "calls nested too deep" end
  depth = depth + 1
  local ok, reply = pcall(c.def.fn, ...)
  depth = depth - 1
  if ok and reply ~= nil and type(reply) ~= "string" then
    ok, reply = false, "reply must be a string or nil, not " .. type(reply)
  end
  if not ok then return false, "bad_arg", tostring(reply) end
  return true, reply or ""
end
function sco.list_commands()
  local out = {}
  for _, n in ipairs(order) do
    local d = commands[n].def
    local args = {}
    for i, a in ipairs(commands[n].args) do args[i] = { name = a.name, type = a.type, help = a.help } end
    out[#out + 1] = { name = n, title = d.title, help = d.help, capability = d.capability, args = args }
  end
  return out
end

-- ---- sandbox ----------------------------------------------------------------------------

local function copy(t) local r = {} for k, v in pairs(t) do r[k] = v end return r end
local safe_string = copy(string)
safe_string.dump = nil                 -- the runtime drops string.dump
-- The runtime refuses finalizers: Lua runs __gc outside the step budget.
local function safe_setmetatable(t, mt)
  if type(mt) == "table" and rawget(mt, "__gc") ~= nil then error("__gc is not allowed in sco-lua", 2) end
  return setmetatable(t, mt)
end
-- The runtime's print: tostring of every argument, tab-separated, logged at info.
local function safe_print(...)
  local t = table.pack(...)
  for i = 1, t.n do t[i] = tostring(t[i]) end
  sco.log("info", table.concat(t, "\t", 1, t.n))
end
local env = {
  sco = sco, string = safe_string, table = copy(table), math = copy(math), utf8 = copy(utf8),
  assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs, pcall = pcall,
  select = select, tonumber = tonumber, tostring = tostring, type = type, xpcall = xpcall,
  rawequal = rawequal, rawget = rawget, rawlen = rawlen, rawset = rawset,
  setmetatable = safe_setmetatable, getmetatable = getmetatable, print = safe_print,
}
env._G = env

local path = dir .. "/" .. ini.entry
local f = io.open(path, "rb")
if not f then fail("entry '" .. ini.entry .. "' not found"); print("FAILED: 1 failure(s)"); os.exit(1) end
local src = f:read("a")
f:close()
-- Text only: the sandbox refuses precompiled chunks.
local chunk, err = load(src, "@" .. ini.entry, "t", env)
if not chunk then fail(err); print("FAILED: " .. failures .. " failure(s)"); os.exit(1) end

local function call(what, fn, ...)
  local ok, e = pcall(fn, ...)
  if not ok then fail(what .. ": " .. tostring(e)) end
  return ok
end

local function dispatch(event, data)
  for _, fn in ipairs(subs[event] or {}) do call("event " .. event, fn, event, data) end
end

if call("load " .. ini.entry, chunk) then
  print("  load    ok: " .. #order .. " command(s)")
  dispatch("game.ready")
  for t = 1, 3 do
    local now = tasks
    tasks = {}
    for _, fn in ipairs(now) do call("task", fn) end
    dispatch("tick", t * 100)
  end

  local function parse(text, ty)
    if ty == "string" then return text end
    if ty == "int" then return math.tointeger(tonumber(text)) end
    if ty == "float" then return tonumber(text) end
    if ty == "bool" then if text == "1" or text == "true" then return true elseif text == "0" or text == "false" then return false end end
    return nil
  end

  for _, name in ipairs(order) do
    local c = commands[name]
    print(string.format("  command %s \"%s\" (%d arg%s)", name, c.def.title, #c.args, #c.args == 1 and "" or "s"))
    local values
    if name == invoke_name then
      if #invoke_args ~= #c.args then fail("--invoke " .. name .. ": needs " .. #c.args .. " argument(s)")
      else
        values = {}
        for i, a in ipairs(c.args) do
          local v = parse(invoke_args[i], a.type)
          if v == nil then fail("--invoke " .. name .. ": bad " .. a.type .. " '" .. invoke_args[i] .. "'"); values = nil; break end
          values[i] = v
        end
      end
    elseif #c.args == 0 then values = {} end
    if values then
      local ok, reply = pcall(c.def.fn, table.unpack(values, 1, #c.args))
      if not ok then fail(name .. ": " .. tostring(reply))
      elseif reply ~= nil and type(reply) ~= "string" then fail(name .. ": reply must be a string or nil")
      else print("  invoke  " .. name .. " -> ok \"" .. (reply or "") .. "\"") end
    end
  end
  if invoke_name and not commands[invoke_name] then fail("--invoke " .. invoke_name .. ": no such command") end
  dispatch("game.exit")
end

print((failures > 0 and "FAILED" or "OK") .. ": " .. failures .. " failure(s)")
os.exit(failures > 0 and 1 or 0)
