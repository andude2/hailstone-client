-- Shared, read-only ACNet inventory snapshots for Compare and Advanced Inventory.
-- A peer's snapshot is InventoryDetails' named records when the plugin gives them (inventory.v2:
-- icon, kind, value, tribute, weight, levels, flags, stats, socketed augments), else the
-- inventory.v1 positional rows ({slot, sub, id, icon, count, ac, hp, mana, name}), marked legacy.
local API = hs.UI
local M = {}
local cache = {}
local roster = {}
local polled = -1

function M.Provider()
  local p = API.plugins.Get("ACNet")
  return p and p.InventorySupported() and p or nil
end

-- One peer row as the Advanced Inventory draws it: {slot, sub, item} with item in hs.Item's shape
-- (plus value, tribute, reqLevel, recLevel, flags and stats), from either snapshot form.
local function record(row, legacy)
  -- Positional rows come only from InventoryRows (inventory.v1); InventoryDetails, detailed or
  -- legacy, already names its fields. Tell them apart by shape, not by the legacy flag.
  if row.slot == nil then
    return { slot = row[1], sub = row[2], legacy = true,
      item = { id = row[3], icon = row[4], count = row[5], name = row[9], stackable = (row[5] or 1) > 1,
        stats = { ac = row[6], hp = row[7], mana = row[8] } } }
  end
  local it = row
  local st = it.stats or {}
  return { slot = it.slot, sub = it.sub, legacy = legacy == true,
    item = { name = it.name, id = it.id, icon = it.icon, count = it.count, stackSize = it.stackSize,
      ac = st.ac, hp = st.hp, mana = st.mana,
      stackable = it.stackable, category = it.category, value = it.value, tribute = it.tribute,
      weight = (it.weight or 0) / 10, bagSlots = it.bagSlots, reqLevel = it.reqLevel, recLevel = it.recLevel,
      magic = it.magic, lore = it.lore, noTrade = it.noTrade, temporary = it.temporary, attuned = it.attuned,
      sellable = it.sellable, tradeskill = it.tradeskill, augment = it.augment, noDestroy = it.noDestroy,
      collectible = it.collectible, stats = it.stats or {}, augs = it.augs } }
end
M.Record = record

function M.Peers()
  local now = ui.GetTime()
  if now - polled < 0.25 and now >= polled then return roster end
  polled = now
  local p = M.Provider()
  local peers = p and p.InventoryPeers and p.InventoryPeers() or {}
  roster = {}
  for key in pairs(cache) do
    if peers[key] ~= cache[key].revision then cache[key] = nil end
  end
  for name, revision in pairs(peers) do
    local entry = cache[name]
    if revision ~= 0 and not entry then
      local snapshot, legacy
      if p.InventoryDetails then
        local details = p.InventoryDetails(name)
        if details then
          legacy = details.legacy == true
          snapshot = { character = details.character, rows = {} }
          for _, row in ipairs(details.rows or {}) do snapshot.rows[#snapshot.rows + 1] = record(row, legacy) end
        end
      end
      if not snapshot then
        local raw = p.InventoryRows(name)
        if raw then
          legacy = true
          snapshot = { character = raw.character, rows = {} }
          for _, row in ipairs(raw.rows or {}) do snapshot.rows[#snapshot.rows + 1] = record(row, true) end
        end
      end
      if snapshot then
        entry = { revision = revision, name = snapshot.character, rows = snapshot.rows, legacy = legacy, worn = {} }
        for _, row in ipairs(entry.rows) do
          if row.slot and row.slot < 23 then entry.worn[row.slot] = row end
        end
        cache[name] = entry
      end
    end
    roster[#roster + 1] = { key = name, name = entry and entry.name or name, snapshot = entry }
  end
  table.sort(roster, function(a, b) return a.key < b.key end)
  return roster
end

return M
