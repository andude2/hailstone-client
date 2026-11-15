-- Loose text matching for the search fields (the Options window's). Pure Lua, no ui.* calls: tests/lua_scripts_test.cpp
-- loads this file. A query is split into words and every word must be found in the text (a label and its tooltip):
--   - as it stands (case does not matter), or
--   - as the first letters of words that follow each other ("fov" finds "Field of View"), two letters or more, or
--   - as letters of one word in order, from its first letter ("vlme" finds "volume"), four letters or more, or
--   - in a word of the same length give or take one letter that differs by one letter ("volime" finds "volume"),
--     five letters or more.
-- The loose rules wait for longer words so that a short query does not match nearly everything: with three letters
-- in order "mus" found "Mouse", and one letter off "rate" found "Race".
local fuzzy = {}

-- True when `word` is `candidate` with one letter changed, added or left out.
local function oneEdit(word, candidate)
  local a, b = #word, #candidate
  if math.abs(a - b) > 1 then return false end
  local i, j, edits = 1, 1, 0
  while i <= a and j <= b do
    if word:byte(i) == candidate:byte(j) then
      i, j = i + 1, j + 1
    else
      edits = edits + 1
      if edits > 1 then return false end
      if a > b then i = i + 1
      elseif b > a then j = j + 1
      else i, j = i + 1, j + 1 end
    end
  end
  return edits + (a - i + 1) + (b - j + 1) <= 1
end

-- True when the letters of `word` are in `candidate` in order, starting with its first letter.
local function inOrder(word, candidate)
  if word:byte(1) ~= candidate:byte(1) then return false end
  local at = 1
  for i = 1, #candidate do
    if candidate:byte(i) == word:byte(at) then at = at + 1 end
  end
  return at > #word
end

-- True when the letters of `word` are the first letters of words of `text` that follow each other.
local function initials(word, words)
  for start = 1, #words - #word + 1 do
    local k = 1
    while k <= #word and words[start + k - 1]:byte(1) == word:byte(k) do k = k + 1 end
    if k > #word then return true end
  end
  return false
end

local function wordFound(word, text, words)
  if text:find(word, 1, true) then return true end
  if #word < 2 then return false end
  if initials(word, words) then return true end
  if #word < 4 then return false end
  for _, candidate in ipairs(words) do
    if inOrder(word, candidate) then return true end
    if #word >= 5 and oneEdit(word, candidate) then return true end
  end
  return false
end

-- True when every word of `query` is found in the label and tooltip; an empty query matches everything.
function fuzzy.match(query, label, tooltip)
  query = (query or ""):lower()
  if not query:find("%S") then return true end
  local text = ((label or "") .. " " .. (tooltip or "")):lower()
  local words = {}
  for w in text:gmatch("%w+") do words[#words + 1] = w end
  for word in query:gmatch("%S+") do
    if not wordFound(word, text, words) then return false end
  end
  return true
end

return fuzzy
