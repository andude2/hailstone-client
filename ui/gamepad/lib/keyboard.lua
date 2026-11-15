-- An on-screen keyboard for the gamepad pack's windows (the chat window's keys, as a piece other windows reuse): a
-- name to add, a message of the day, a /who name. The window that shows it keeps its state:
--
--   local K = dofile("lib/keyboard.lua")
--   local kb = K.new()                      -- kb.open, kb.text
--   K.open(kb, "Add a friend", "")          -- shows it with that prompt and starting text
--   if kb.open then
--     local done = K.draw(kb, pad, x0, y0, width)   -- at the cursor; "ok" (Done), "cancel" (Cancel) or nil
--   end
--
-- A types the key under the focus, X deletes, Y types a space, LB is shift, Done accepts; B cancels (the window
-- calls K.cancel when its W.finish asks to close while the keyboard is up). The first key takes the pad's focus.
-- (Start is left alone: it opens the main menu.)
local T = dofile("lib/theme.lua")
local C = T.C
local K = {}

-- The keys (the chat window's keys too: K.KEYS, K.label). The last row has what slash commands and chat need
-- ("/safelock", "#who", "@name", "%t"); shift gives the letters' capitals and the other keys' second characters.
local KEYS = {
  { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
  { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
  { "a", "s", "d", "f", "g", "h", "j", "k", "l", "'" },
  { "z", "x", "c", "v", "b", "n", "m", ",", ".", "?" },
  { "/", "#", "@", "%", ":", "-", "!", "(", ")", "+" },
}
local SHIFTED = {
  ["1"] = "!", ["2"] = "@", ["3"] = "#", ["4"] = "$", ["5"] = "%", ["6"] = "^", ["7"] = "&", ["8"] = "*", ["9"] = "(",
  ["0"] = ")", ["'"] = "\"", [","] = ";", ["."] = ">", ["?"] = "<",
  ["/"] = "\\", ["#"] = "~", ["@"] = "&", ["%"] = "*", [":"] = "=", ["-"] = "_", ["!"] = "|", ["("] = "[", [")"] = "]",
  ["+"] = "$",
}
K.KEYS = KEYS

-- What a key types (and shows) with shift on or off.
function K.label(key, shift)
  if not shift then return key end
  return SHIFTED[key] or key:upper()
end

-- The keys' button ids, made once (not each frame).
local KEY_IDS = {}
for r, row in ipairs(KEYS) do
  KEY_IDS[r] = {}
  for k in ipairs(row) do KEY_IDS[r][k] = "##kb" .. r .. "_" .. k end
end

-- The window's hints while the keyboard is up.
K.HINTS = { { "a", "Type" }, { "x", "Delete" }, { "y", "Space" }, { "lb", "Shift" }, { "b", "Cancel" } }

function K.new() return { open = false, text = "", prompt = "", shift = false, max = 256 } end

-- Opens it; `max` the most characters (the edit box's limit), `capital` starts with shift on (a name).
function K.open(kb, prompt, text, max, capital)
  kb.open, kb.prompt, kb.text, kb.max, kb.shift = true, prompt or "", text or "", max or 256, capital == true
  kb.focusFirst = true
end

function K.cancel(kb) kb.open = false end

-- Draws it at x0, y0 (screen), `w` wide; returns "ok" or "cancel" when one was chosen.
function K.draw(kb, pad, x0, y0, w)
  T.frame()
  local wx, wy = ui.GetWindowPos()
  local result
  -- the prompt and the typed text
  T.font(20)
  T.draw(x0, y0, kb.prompt, C.dim)
  local _, th = ui.CalcTextSize("Ag")
  T.unfont()
  local fy = y0 + th + T.px(6)
  T.rect(x0, fy, x0 + w, fy + T.px(56), C.sunken, 1, T.px(10))
  T.outline(x0, fy, x0 + w, fy + T.px(56), C.accent, 0.6, T.px(10))
  T.font(24)
  local caret = (math.floor(ui.GetTime() * 2) % 2 == 0) and "_" or " "
  T.textIn(x0 + T.px(16), fy, x0 + w - T.px(16), fy + T.px(56), kb.text .. caret, C.text, 0)
  T.unfont()
  local y = fy + T.px(56) + T.px(12)
  local kw = (w - 9 * T.px(8)) / 10
  for r, row in ipairs(KEYS) do
    ui.SetCursorPos(x0 - wx, y - wy)
    for k, key in ipairs(row) do
      if k > 1 then ui.SameLine(0, T.px(8)) end
      if kb.focusFirst and r == 2 and k == 1 then
        ui.SetKeyboardFocusHere()
        kb.focusFirst = false
      end
      local label = K.label(key, kb.shift)
      if T.button(KEY_IDS[r][k], label, kw, T.px(60), {}, pad) and #kb.text < kb.max then
        kb.text = kb.text .. label
        kb.shift = false
      end
    end
    y = y + T.px(60) + T.px(8)
  end
  ui.SetCursorPos(x0 - wx, y - wy)
  local bw = (w - 4 * T.px(8)) / 5
  if T.button("##kbshift", "Shift", bw, T.px(60), { primary = kb.shift, glyph = "lb" }, pad) then kb.shift = not kb.shift end
  ui.SameLine(0, T.px(8))
  if T.button("##kbspace", "Space", bw, T.px(60), { glyph = "y" }, pad) and #kb.text < kb.max then kb.text = kb.text .. " " end
  ui.SameLine(0, T.px(8))
  if T.button("##kbdel", "Delete", bw, T.px(60), { glyph = "x" }, pad) then kb.text = T.dropChar(kb.text) end
  ui.SameLine(0, T.px(8))
  if T.button("##kbcancel", "Cancel", bw, T.px(60), {}, pad) then result = "cancel" end
  ui.SameLine(0, T.px(8))
  if T.button("##kbok", "Done", bw, T.px(60), { primary = true }, pad) then result = "ok" end
  -- the pad's shortcuts while the window has the focus
  if ui.IsWindowFocused(3) then
    if pad.menuPressed("x") then kb.text = T.dropChar(kb.text) end
    if pad.menuPressed("y") and #kb.text < kb.max then kb.text = kb.text .. " " end
    if pad.menuPressed("lb") then kb.shift = not kb.shift end
  end
  if result then kb.open = false end
  return result
end

return K
