-- STML (the game's text markup) for the gamepad pack: a chat line into coloured pieces, and drawing them
-- word-wrapped with the theme's text. <c "#rrggbb"> colours (nested), links are shown in the link colour; other
-- tags are dropped.
local T = dofile("lib/theme.lua")
local S = {}

-- links' colour: the palette's (a table lib/theme.lua changes in place); the chat sets its own copy's chatLink
S.LINK = T.C.link
local ENT = { lt = "<", gt = ">", amp = "&", quot = "\"", apos = "'", nbsp = " " }

local function entities(t) return (t:gsub("&(%a+);", function(e) return ENT[e:lower()] or ("&" .. e .. ";") end)) end
local function hex(h)
  if #h == 8 then h = h:sub(3) end
  local r, g, b = tonumber(h:sub(1, 2), 16), tonumber(h:sub(3, 4), 16), tonumber(h:sub(5, 6), 16)
  return r and g and b and { r / 255, g / 255, b / 255, 1 } or nil
end

-- line -> { { text, colour } }; color: the colour outside any <c> (the palette's text)
function S.parse(line, color)
  local out, colors, link = {}, { color or T.C.text }, false
  local pos = 1
  while pos <= #line do
    local lt = line:find("<", pos, true)
    local text = line:sub(pos, (lt or #line + 1) - 1)
    if text ~= "" then out[#out + 1] = { entities(text), link and S.LINK or colors[#colors] } end
    if not lt then break end
    local gt = line:find(">", lt, true)
    if not gt then break end
    local tag = line:sub(lt + 1, gt - 1)
    local name = (tag:match("^/?(%a+)") or ""):lower()
    if name == "c" and tag:sub(1, 1) ~= "/" then table.insert(colors, hex(tag:match("#(%x+)") or "") or colors[#colors])
    elseif name == "c" then if #colors > 1 then table.remove(colors) end
    elseif name == "a" then link = tag:sub(1, 1) ~= "/" end
    pos = gt + 1
  end
  return out
end

-- The plain text of a parsed line.
function S.plain(segments)
  local t = {}
  for _, s in ipairs(segments) do t[#t + 1] = s[1] end
  return table.concat(t)
end

-- Lays a parsed line out in `width`: a list of rows, each { { text, colour, x } }. A new line in the text (a chat
-- line can hold several: a server's MOTD) ends the row; an empty one is an empty row; one at the end adds nothing.
function S.layout(segments, width)
  local rows, row, x = {}, {}, 0
  for _, seg in ipairs(segments) do
    local text, from = seg[1], 1
    while true do
      local nl = text:find("\n", from, true)
      for word, space in text:sub(from, (nl or #text + 1) - 1):gmatch("([^ ]*)( *)") do
        if word ~= "" or space ~= "" then
          local ww = ui.CalcTextSize(word)
          if x > 0 and x + ww > width then
            rows[#rows + 1] = row
            row, x = {}, 0
          end
          row[#row + 1] = { word .. space, seg[2], x }
          x = x + ui.CalcTextSize(word .. space)
        end
      end
      if not nl then break end
      rows[#rows + 1] = row
      row, x, from = {}, 0, nl + 1
    end
  end
  if #row > 0 or #rows == 0 then rows[#rows + 1] = row end
  return rows
end

-- A number for the font in use (what ui.CalcTextSize measures with): a kept layout is for one width and font.
function S.fontKey()
  local w, h = ui.CalcTextSize("Wg")
  return w * 4096 + h
end

-- S.layout(segments, width), kept on the parsed line until the width or the font (S.fontKey) change: a line drawn
-- every frame is laid out once.
function S.rows(segments, width, font)
  font = font or S.fontKey()
  if segments.rowsWidth ~= width or segments.rowsFont ~= font then
    segments.rows, segments.rowsWidth, segments.rowsFont = S.layout(segments, width), width, font
  end
  return segments.rows
end

-- A block of STML (<br> and <p> are line breaks) -> its lines.
function S.split(stml)
  local s = (stml or ""):gsub("\r", ""):gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("</?[Pp]>", "\n")
  local out = {}
  for line in (s .. "\n"):gmatch("(.-)\n") do out[#out + 1] = line end
  return out
end

-- Every row of a block of STML laid out in `width`, one list: S.split's lines (or build(text, width)'s rows),
-- each parsed and laid out. The last few texts' rows are kept (by text, width, font and build), so a text drawn
-- every frame, or two drawn by turns, is not split, parsed and laid out again each time.
local blocks, blockOrder = {}, {}
local BLOCKS = 8
local function blockRows(text, width)
  local rows = {}
  for _, line in ipairs(S.split(text)) do
    for _, r in ipairs(S.layout(S.parse(line), width)) do rows[#rows + 1] = r end
  end
  return rows
end
function S.blockRows(text, width, build, font)
  text, build, font = text or "", build or blockRows, font or S.fontKey()
  local e = blocks[text]
  if e and e.width == width and e.font == font and e.build == build then return e.rows end
  if not e then
    e = {}
    blocks[text] = e
    blockOrder[#blockOrder + 1] = text
    if #blockOrder > BLOCKS then blocks[table.remove(blockOrder, 1)] = nil end
  end
  e.width, e.font, e.build, e.rows = width, font, build, build(text, width)
  return e.rows
end

function S.drawRow(row, x, y, alpha)
  for _, p in ipairs(row) do T.draw(x + p[3], y, p[1], p[2], alpha) end
end

return S
