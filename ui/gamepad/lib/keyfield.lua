-- The on-screen keyboard typing straight into a field with the pad alone (get / set; lib/keyboard.lua keeps its own text).
-- A types the focused key; while the keyboard is shown, X deletes, Y types a space and LB is shift.
--
--   local K = dofile("lib/keyfield.lua")
--   local kb = K.new()
--   if K.draw(kb, get, set, x0, x1, { newline = true }) == "done" then ... end   -- at the layout cursor
-- get() returns the text, set(text) replaces it. K.focus(kb) puts the pad's focus on the keys next frame.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local KB = dofile("lib/keyboard.lua") -- (its keys and what shift makes of them)
local K = {}

-- The keys' button ids, made once (not each frame).
local KEY_IDS = {}
for r, row in ipairs(KB.KEYS) do
  KEY_IDS[r] = {}
  for k in ipairs(row) do KEY_IDS[r][k] = "##key" .. r .. "_" .. k end
end

function K.new() return { shift = false, focus = false } end
function K.focus(kb) kb.focus = true end

-- The hints a window shows while its keyboard is up.
K.hints = { { "a", "Type" }, { "x", "Delete" }, { "y", "Space" }, { "lb", "Shift" } }

-- Draws the keys at the cursor from x0 to x1 (screen); returns "done" when Done was pressed.
function K.draw(kb, get, set, x0, x1, opts)
  opts = opts or {}
  local wx, wy = ui.GetWindowPos()
  local function put(s) set((get() or "") .. s) end
  local gap = T.px(8)
  local kh = T.px(opts.keyHeight or 60)
  local kw = (x1 - x0 - 9 * gap) / 10
  local result
  for r, row in ipairs(KB.KEYS) do
    ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
    for k, key in ipairs(row) do
      if k > 1 then ui.SameLine(0, gap) end
      if kb.focus and r == 2 and k == 1 then
        ui.SetKeyboardFocusHere()
        kb.focus = false
      end
      local label = KB.label(key, kb.shift)
      if T.button(KEY_IDS[r][k], label, kw, kh, {}, pad) then
        put(label)
        kb.shift = false
      end
    end
  end
  ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
  local n = opts.newline and 5 or 4
  local ew = (x1 - x0 - (n - 1) * gap) / n
  if T.button("##kshift", "Shift", ew, kh, { primary = kb.shift }, pad) then kb.shift = not kb.shift end
  ui.SameLine(0, gap)
  if T.button("##kspace", "Space", ew, kh, {}, pad) then put(" ") end
  ui.SameLine(0, gap)
  if opts.newline then
    if T.button("##knl", "New Line", ew, kh, {}, pad) then put("\n") end
    ui.SameLine(0, gap)
  end
  if T.button("##kdel", "Delete", ew, kh, {}, pad) then set(T.dropChar(get() or "")) end
  ui.SameLine(0, gap)
  if T.button("##kdone", "Done", ew, kh, { primary = true }, pad) then result = "done" end
  -- the pad's shortcuts while the window has the focus
  if ui.IsWindowFocused(3) then
    if pad.menuPressed("x") then set(T.dropChar(get() or "")) end
    if pad.menuPressed("y") then put(" ") end
    if pad.menuPressed("lb") then kb.shift = not kb.shift end
  end
  return result
end

return K
