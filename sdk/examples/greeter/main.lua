-- greeter: the sco SDK's Lua example plugin.
--
-- The script gets the `sco` table (it mirrors sco_api.h) plus string, table, math and utf8.
-- There is no io, os, package, debug or FFI, and a callback that runs too long is stopped
-- and the script disabled. Each function returns what the C call returns: true on success,
-- or false and the result name ("bad_arg", "unavailable", ...).

local greeted = 0

-- greeter.greet <name> [shout]: replies "Hi, <name>" (or "HI, <NAME>!").
-- Runs on the game thread. Return the reply text; error() turns into a failed command.
local ok, err = sco.register_command{
  name  = "greeter.greet",
  title = "Greet",
  help  = "Says hi on the status line",
  args  = {
    { name = "name",  type = "string", help = "Who to greet" },
    { name = "shout", type = "bool",   help = "Upper case" },
  },
  fn = function(name, shout)
    greeted = greeted + 1
    local text = "Hi, " .. name
    if shout then text = string.upper(text) .. "!" end
    return text
  end,
}
if not ok then
  sco.log("error", "register_command failed: " .. tostring(err))
  return
end

sco.subscribe("game.ready", function()
  -- Capabilities depend on the game build: check before offering a feature.
  if sco.has("teleport") then
    sco.log("info", "teleport is available")
  else
    sco.log("warn", "teleport is not available on this game build")
  end
end)

sco.subscribe("game.exit", function()
  sco.log("info", string.format("greeted %d times", greeted))
end)

sco.status("Greeter ready")
