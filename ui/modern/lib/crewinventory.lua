-- Shared, read-only MQACNet inventory snapshots for Compare and Advanced Inventory.
local API = hs.UI
local M = {}
local cache = {}
local roster = {}
local polled = -1

function M.Provider()
  local p = API.plugins.Get("MQACNet")
  return p and p.InventorySupported() and p or nil
end

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
      local snapshot = p.InventoryRows(name)
      if snapshot then
        entry = { revision = revision, name = snapshot.character, rows = snapshot.rows, worn = {} }
        for _, row in ipairs(entry.rows) do
          if row[1] < 23 then entry.worn[row[1]] = row end
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
