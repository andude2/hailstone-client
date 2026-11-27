-- Cast bar: replaces the game's CastingWindow (shown while casting a spell or clicking an item with a cast time).
-- A small borderless frame like the air meter: the spell's name over a bar that fills as the cast completes (the
-- casting gauge, EQType 7, counts the time left down from 1). Moves by dragging; its right-click menu can keep the
-- empty bar visible between casts, and Layout mode shows it so it can be placed while idle.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "CastingWindow"
local ALWAYS_SETTING = "casting.alwaysShow"
local LEGACY_SETTING = "player.alwaysShowCastBar"

local NO_TRAIL = { noTrail = true }
local state = { casting = false }

local function alwaysShow()
  local v = T.Setting(ALWAYS_SETTING)
  if v ~= "" then return v == "1" end
  return T.Setting(LEGACY_SETTING) == "1"
end

local WINDOW_OPTS = {
  size = function() return 240, 38 end,
  shown = function(casting)
    state.casting = casting
    return casting or hud.Unlocked() or alwaysShow()
  end,
  menu = function()
    local always = alwaysShow()
    if ui.MenuItem("Always show cast bar", always) then
      local nextVal = always and "0" or "1"
      ui.SetSetting(ALWAYS_SETTING, nextVal)
      ui.SetSetting(LEGACY_SETTING, nextVal)
    end
  end,
}

local function drawWindow(x, y, w, h, s)
  local pad = 8 * s
  local _, th = T.TextSize("Ag", T.font.small)
  local done
  if state.casting then
    local left, _, raw = API.character.Gauge(7)
    done = (left and raw ~= -1) and (1 - left) or 0
    T.Text(x + pad, y + 4 * s, T.Fit(API.character.Label(134) or "", w - pad * 2, T.font.small), C.text, T.font.small)
  elseif hud.Unlocked() then
    T.Text(x + pad, y + 4 * s, "Cast Bar", C.muted, T.font.small)
  end
  local by = y + 4 * s + th + 3 * s
  T.Bar("casting.bar", x + pad, by, x + w - pad, math.min(by + 9 * s, y + h - 5 * s), s, done, C.cast, NO_TRAIL)
end

ui.window("Casting", function()
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Cast Bar", WINDOW_OPTS, drawWindow)
end)
