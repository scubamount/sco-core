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
function sco.subscribe(event, fn)
  if type(event) ~= "string" or type(fn) ~= "function" then fail("sco.subscribe(event, fn): bad arguments"); return false, "bad_arg" end
  subs[event] = subs[event] or {}
  table.insert(subs[event], fn)
  return true
end
function sco.unsubscribe(event, fn)
  for i, f in ipairs(subs[event] or {}) do
    if f == fn then table.remove(subs[event], i); return true end
  end
  return false, "not_found"
end
function sco.run_on_game_thread(fn)
  if type(fn) ~= "function" then fail("sco.run_on_game_thread: fn must be a function"); return false, "bad_arg" end
  sco._tasks = sco._tasks or {}
  table.insert(sco._tasks, fn)
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
function sco.invoke(name, ...)
  local c = commands[name]
  if not c then return false, "not_found" end
  local ok, reply = pcall(c.def.fn, ...)
  if not ok then return false, "error: " .. tostring(reply) end
  return true, reply
end
function sco.list_commands()
  local out = {}
  for _, n in ipairs(order) do out[#out + 1] = { name = n, title = commands[n].def.title } end
  return out
end

-- ---- sandbox ----------------------------------------------------------------------------

local function copy(t) local r = {} for k, v in pairs(t) do r[k] = v end return r end
local env = {
  sco = sco, string = copy(string), table = copy(table), math = copy(math), utf8 = copy(utf8),
  assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs, pcall = pcall,
  select = select, tonumber = tonumber, tostring = tostring, type = type, xpcall = xpcall,
  rawequal = rawequal, rawget = rawget, rawlen = rawlen, rawset = rawset,
  setmetatable = setmetatable, getmetatable = getmetatable, print = function(...) sco.log("info", table.concat({...}, " ")) end,
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
    local tasks = sco._tasks or {}
    sco._tasks = {}
    for _, fn in ipairs(tasks) do call("task", fn) end
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
