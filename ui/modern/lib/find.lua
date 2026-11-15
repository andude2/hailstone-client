-- The Find window's state and look (eq.FindNpcs; Hailstone's Find window, the map's Find page), shared by the map
-- (map.lua) and the minimap (minimap.lua), so both mark the same NPCs: those of the kinds not left out that match the
-- search.
--
--   local FL = dofile("lib/find.lua")
--   FL.state             -- { search, sel (the selected NPC's id), hover (the hovered row's id), why, menu }
--   FL.CATS              -- eq.FindCategories(): { { key, label }, ... }
--   FL.rows()            -- the rows shown, nearest first (kept a moment, shared: do not change it)
--   FL.hidden()          -- the kinds left out, as a set of keys (find.hidden in ui/settings/modern.txt)
--   FL.setHidden(key, hide)
--   FL.marks()           -- whether the maps mark every listed NPC (map.npcMarks; the selected and hovered always)
--   FL.drawMark(sx, sy, row, s, lit, label, T) -- a diamond in the kind's colour, named (with T, lib/theme) when `label`
local FL = {}

FIND_STATE = FIND_STATE or { search = "", sel = nil, hover = nil, why = nil, menu = nil }
FL.state = FIND_STATE

FL.CATS = eq.FindCategories and eq.FindCategories() or {}
FL.SHORT = { merchant = "Merchants", banker = "Bankers", guildmaster = "Guildmasters", special = "Special",
  service = "Services", quest = "Quests", poi = "Points" }
FL.COLORS = {
  merchant = { 1.00, 0.80, 0.36 }, banker = { 0.40, 0.90, 0.55 }, guildmaster = { 0.48, 0.78, 1.00 },
  special = { 1.00, 0.55, 0.30 }, service = { 0.80, 0.55, 1.00 }, quest = { 1.00, 0.92, 0.25 },
  poi = { 0.92, 0.92, 0.92 },
}
local TEXT = {}
for key, c in pairs(FL.COLORS) do TEXT[key] = { c[1], c[2], c[3], 1 } end
FL.TEXT = TEXT

-- parsed again only when the setting changes
local hiddenRaw, hiddenSet = nil, {}
function FL.hidden()
  local raw = ui.GetSetting("find.hidden") or ""
  if raw ~= hiddenRaw then
    hiddenRaw, hiddenSet = raw, {}
    for key in raw:gmatch("[^,]+") do hiddenSet[key] = true end
  end
  return hiddenSet
end

function FL.setHidden(key, hide)
  local set, parts = FL.hidden(), {}
  for _, c in ipairs(FL.CATS) do
    local h = set[c[1]]
    if c[1] == key then h = hide end
    if h then parts[#parts + 1] = c[1] end
  end
  ui.SetSetting("find.hidden", table.concat(parts, ","))
end

-- The rows are kept (in FIND_STATE, so the map and the minimap, each with its own copy of this file, share them)
-- until the zone, the search or the kinds left out change, or ROWS_REREAD seconds pass (the game makes its list again
-- every 200 ms for the distances and positions). The table returned is shared: read it, do not change it.
local ROWS_REREAD = 0.2
function FL.rows()
  -- a new zone: its spawn ids are its own, so the old selection would name another NPC
  local zone = select(2, eq.ZoneName()) or ""
  if zone ~= FIND_STATE.zone then
    FIND_STATE.zone, FIND_STATE.sel, FIND_STATE.hover = zone, nil, nil
  end
  local hidden = FL.hidden()
  local now = ui.GetTime()
  local c = FIND_STATE.rowsCache
  if c and c.zone == zone and c.search == FIND_STATE.search and c.hidden == hiddenRaw
      and now - c.at < ROWS_REREAD and now >= c.at then
    return c.rows
  end
  local out = {}
  for _, r in ipairs(eq.FindNpcs and eq.FindNpcs(FIND_STATE.search) or {}) do
    if not hidden[r.category] then out[#out + 1] = r end
  end
  FIND_STATE.rowsCache = { zone = zone, search = FIND_STATE.search, hidden = hiddenRaw, at = now, rows = out }
  return out
end

function FL.marks() return ui.GetSetting("map.npcMarks") == "1" end

function FL.drawMark(sx, sy, r, s, lit, label, T)
  local col = FL.COLORS[r.category] or FL.COLORS.poi
  local d = (lit and 6 or 4.5) * s
  ui.DrawTriangleFilled(sx, sy - d - 1, sx - d - 1, sy + 1, sx + d + 1, sy + 1, 0, 0, 0, 0.55)
  ui.DrawTriangleFilled(sx, sy + d + 1, sx - d - 1, sy - 1, sx + d + 1, sy - 1, 0, 0, 0, 0.55)
  ui.DrawTriangleFilled(sx, sy - d, sx - d, sy, sx + d, sy, col[1], col[2], col[3], 1)
  ui.DrawTriangleFilled(sx, sy + d, sx - d, sy, sx + d, sy, col[1], col[2], col[3], 1)
  if label and T then T.Text(sx + d + 3 * s, sy - 8 * s, r.name, TEXT[r.category] or TEXT.poi, T.font.small) end
end

return FL
