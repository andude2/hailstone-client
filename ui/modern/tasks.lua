-- Compact multi-quest journal. Objectives retain the server's order, individual hidden rows and real group
-- separators; no numbered phases or parent/child relationships are inferred. Tracking is shared with the HUD;
-- reward previews and abandonment remain the selected task's native actions.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local Q = dofile("lib/task_journal.lua")
local T = hud.T
local C = T.color
local WINDOW, TABS = "TaskWnd", "TaskSubwindows"
local PAGE_ACTIVE, PAGE_MEMBERS, PAGE_HISTORY = 0, 1, 2
local state = { category = "active", listW = 200 }
local KIND = { [0] = "Task", [1] = "Shared task", [2] = "Quest" }

local function setCategory(category)
  state.category, state.categoryPref = category, category
  ui.SetSetting("tasks.category", category)
  API.windows.SetTabPage(WINDOW, TABS, category == "history" and PAGE_HISTORY or PAGE_ACTIVE)
end

local function find(tasks, id)
  for _, task in ipairs(tasks) do if task.id == id then return task end end
end

local function pick(task)
  if task and API.tasks.Open(task.id) then
    state.selected, state.nativeId = task.id, task.id
    Q.invalidate()
  end
end

-- Follow a task opened by the tracker, without taking a history selection away while its page is open.
local function sync(tasks)
  local restoredCategory = false
  local requestedMembers = API.windows.TabPage(WINDOW, TABS) == PAGE_MEMBERS
  local native = find(tasks, tasks.selectedId)
  local category = ui.GetSetting("tasks.category")
  -- Earlier versions used Shared as a quest filter. The native page is a roster, not another quest list.
  if category == "shared" then category = "active"; ui.SetSetting("tasks.category", category) end
  if not state.loaded or category ~= state.categoryPref then
    state.categoryPref = category
    if category == "active" or category == "history" then
      state.category = category
      API.windows.SetTabPage(WINDOW, TABS, category == "history" and PAGE_HISTORY or PAGE_ACTIVE)
      if native and category == "active" then state.selected = native.id end
      restoredCategory = true
    end
  end
  if not state.loaded then
    state.loaded = true
    local test = tonumber(ui.GetSetting("test.tab." .. WINDOW .. "." .. TABS) or "")
    if test then API.windows.SetTabPage(WINDOW, TABS, test); restoredCategory = false end
  end
  local page = API.windows.TabPage(WINDOW, TABS) or PAGE_ACTIVE
  if requestedMembers or page == PAGE_MEMBERS then
    state.openMembers = true
    setCategory("active")
  elseif page == PAGE_HISTORY then
    state.category = "history"
  elseif native and not restoredCategory and (state.nativeId ~= native.id or state.category == "history") then
    state.selected = native.id
    state.category = "active"
  end
  state.nativeId = tasks.selectedId
  if page ~= PAGE_ACTIVE and page ~= PAGE_HISTORY then API.windows.SetTabPage(WINDOW, TABS, PAGE_ACTIVE) end
  if state.category ~= "history" then
    local task = find(tasks, state.selected)
    if not task then
      local selected
      for _, row in ipairs(tasks) do if Q.isTracked(row) then selected = row; break end end
      selected = selected or tasks[1]
      state.selected = selected and selected.id or nil
      pick(selected)
    end
  end
end

local function button(label, id, selected, s)
  if selected then ui.PushStyleColor(ui.Col.Button, C.accent[1], C.accent[2], C.accent[3], 0.24) end
  local hit = ui.Button(label .. "##" .. id, 0, 25 * s)
  if selected then ui.PopStyleColor() end
  return hit
end

local CATEGORY_TABS = { { "Active", "active", "category.active" }, { "History", "history", "category.history" } }
local function categories(tasks, history, s)
  for i, tab in ipairs(CATEGORY_TABS) do
    if i > 1 then ui.SameLine(0, 5 * s) end
    if button(tab[1] .. "  " .. (i == 1 and #tasks or #history), tab[3], state.category == tab[2], s) then
      setCategory(tab[2])
      if tab[2] ~= "history" then
        local selected = find(tasks, state.selected)
        if not selected then
          for _, task in ipairs(tasks) do if Q.isTracked(task) then selected = task; break end end
        end
        selected = selected or tasks[1]
        state.selected = selected and selected.id or nil
        pick(selected)
      end
    end
  end
  -- This roster belongs to the current shared task, independently of the selected quest or History page.
  local members = API.windows.ListInfo(WINDOW, "STASK_MemberList") or 0
  local label = "Shared members  " .. members
  local width = T.TextSize(label, T.font.normal) + 24 * s
  ui.SameLine(0, 8 * s)
  local x, y = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x + math.max(0, ui.GetContentRegionAvail() - width), y)
  if button(label, "sharedMembers", false, s) then state.openMembers = true end
  if ui.IsItemHovered() then ui.SetTooltip("View shared-task members and manage invitations") end
  ui.Dummy(0, 3 * s)
end

local function memberRow(rows, name)
  if name then for _, row in ipairs(rows) do if row.texts[1] == name then return row.row end end end
  return -1
end

-- The native roster's selection set to `row` (-1: none), clicked only when it is not that already: a click drops
-- every list the frame has read, so one each frame for nothing made the windows build their lists again.
local function selectMember(row)
  local _, _, current = API.windows.ListInfo(WINDOW, "STASK_MemberList")
  if (current or -1) ~= row then API.windows.ListClick(WINDOW, "STASK_MemberList", row, "left", 0) end
end

local function sharedMembers(tasks, s)
  local popup = "##sharedMembers"
  if state.openMembers then
    state.openMembers, state.memberName = nil, nil
    API.windows.ListClick(WINDOW, "STASK_MemberList", -1, "left", 0)
    ui.OpenPopup(popup)
  end
  local io = ui.GetIO()
  ui.SetNextWindowSize(math.min(420 * s, (io.width or 1280) - 20 * s), 0, ui.Cond.Always)
  if not ui.BeginPopup(popup) then return end
  ui.PushFont(T.font.normal)
  ui.Text("Shared task members")
  local shared
  for _, task in ipairs(tasks) do if task.type == 1 then shared = task; break end end
  local sharedId = shared and shared.id or nil
  if state.memberTaskId ~= sharedId then state.memberName = nil; state.memberTaskId = sharedId end
  if shared then ui.TextWrapped(shared.title or "") end
  ui.Separator()

  -- Membership updates do not change the task revision. Re-read the native roster, and retain selection by
  -- name so a departing member cannot shift Remove/Make leader onto a different person at the same row index.
  local rows = K.Rows(WINDOW, "STASK_MemberList")
  local selected = memberRow(rows, state.memberName)
  selectMember(selected)
  if #rows == 0 then
    state.memberName = nil
    K.Empty(shared and "Waiting for member details." or "You are not in a shared task.")
  else
    local hasTemplates = false
    for _, row in ipairs(rows) do
      if (row.texts[3] or "") ~= "" then hasTemplates = true; break end
    end
    W.List(WINDOW, "STASK_MemberList", 0, math.min(210 * s, (28 + #rows * 22) * s), {
      hide = { [3] = not hasTemplates }, columns = { { "Member", 0 }, { "Role", 70 }, { "Template", 130 } },
    })
    local _, _, picked = API.windows.ListInfo(WINDOW, "STASK_MemberList")
    state.memberName = picked and rows[picked + 1] and rows[picked + 1].texts[1] or nil
  end
  ui.Separator()
  if API.windows.ChildState(WINDOW, "STASK_AddPlayerButton") == false then
    K.Muted("Only the shared-task leader can manage members.")
  end
  local h = 25 * s
  W.Button(WINDOW, "STASK_AddPlayerButton", "Add target", { height = h,
    tooltip = "Invite your current player target to the shared task" })
  ui.SameLine(0, 6 * s)
  -- Re-resolve immediately before the member actions; native selection is only an index.
  selected = memberRow(K.Rows(WINDOW, "STASK_MemberList"), state.memberName)
  selectMember(selected)
  ui.BeginDisabled(selected < 0)
  W.Button(WINDOW, "STASK_RemovePlayerButton", "Remove member", { height = h, danger = true })
  ui.SameLine(0, 6 * s)
  W.Button(WINDOW, "STASK_MakeLeaderButton", "Make leader", { height = h })
  ui.EndDisabled()
  W.Button(WINDOW, "STASK_InviteRaidButton", "Invite raid", { height = h })
  ui.SameLine(0, 6 * s)
  W.Button(WINDOW, "STASK_InviteExpedButton", "Invite expedition", { height = h })
  ui.SameLine(0, 12 * s)
  if ui.Button("Close##closeSharedMembers", 0, h) then ui.CloseCurrentPopup() end
  ui.PopFont()
  ui.EndPopup()
end

local function card(task, s)
  local tracked = Q.isTracked(task)
  local done, total = Q.stats(task)
  local title = task.title or ""
  local clicked = K.Card("quest." .. task.id, task.id == state.selected, 43, function(x, y, w)
    local markW = tracked and 11 * s or 0
    if tracked then ui.DrawCircleFilled(x + 3 * s, y + 11 * s, 2.5 * s, C.accent[1], C.accent[2], C.accent[3], 1) end
    T.Text(x + markW, y + 3 * s, T.Fit(title, w - markW - 6 * s, T.font.small), C.text, T.font.small)
    local count = string.format("%d/%d", done, total)
    local countW = T.TextSize(count, T.font.small)
    local timed = (task.duration or 0) > 0
    local here = Q.inZone(task)
    local meta = timed and (Q.time(task.left) .. " left") or here and "In this zone" or (KIND[task.type] or "Task")
    if task.type == 1 and (timed or here) then meta = "Shared: " .. meta end
    local color = timed and ((task.left or 0) <= 120 and C.danger or C.gold) or here and C.accent or C.muted
    T.Text(x, y + 23 * s, T.Fit(meta, w - countW - 12 * s, T.font.small), color, T.font.small)
    T.Text(x + w - 6 * s, y + 23 * s, count, C.muted, T.font.small, "right")
  end)
  if ui.IsItemHovered() then ui.SetTooltip(title .. (tracked and "\nTracked" or "")) end
  if clicked then pick(task) end
end

local trackedList, otherList = {}, {} -- the list's two groups, emptied and filled again each frame
local function taskList(tasks, needle, s)
  local tracked, other = trackedList, otherList
  for k = #tracked, 1, -1 do tracked[k] = nil end
  for k = #other, 1, -1 do other[k] = nil end
  for _, task in ipairs(tasks) do
    if needle == "" or (task.title or ""):lower():find(needle, 1, true) then
      local group = Q.isTracked(task) and tracked or other
      group[#group + 1] = task
    end
  end
  if #tracked + #other == 0 then K.Empty(needle == "" and "No tasks here." or "No matching tasks."); return end
  if #tracked > 0 then
    K.Muted("Tracked", C.accent)
    for _, task in ipairs(tracked) do card(task, s) end
  end
  if #other > 0 then
    if #tracked > 0 then ui.Dummy(0, 4 * s) end
    K.Muted(#tracked > 0 and "Other active" or "Active quests")
    for _, task in ipairs(other) do card(task, s) end
  end
end

local function historyList(rows, selected, needle, s)
  local shown = 0
  for _, row in ipairs(rows) do
    local title = row.texts[2] or ""
    if needle == "" or title:lower():find(needle, 1, true) then
      shown = shown + 1
      local hit = K.Card("history." .. row.row, row.row == selected, 43, function(x, y, w)
        T.Text(x, y + 3 * s, T.Fit(title, w - 6 * s, T.font.small), C.text, T.font.small)
        T.Text(x, y + 23 * s, T.Fit(row.texts[3] or "Completed", w - 6 * s, T.font.small), C.muted, T.font.small)
      end)
      if ui.IsItemHovered() then ui.SetTooltip(title) end
      if hit then API.windows.ListClick(WINDOW, "QUEST_TaskList", row.row, "left", 1) end
    end
  end
  if shown == 0 then K.Empty(needle == "" and "No completed quests." or "No matching quests.") end
end

local function objectiveTable(rows, s)
  if #rows == 0 then K.Empty("No objectives to show."); return end
  local avail = ui.GetContentRegionAvail()
  local zoneW = math.max(85 * s, math.min(155 * s, avail * 0.29))
  ui.PushFont(T.font.small)
  ui.PushStyleVar(ui.StyleVar.CellPadding, 4 * s, 1 * s)
  if ui.BeginTable("##questObjectives", 3, ui.TableFlags.SizingFixedFit, 0, 0) then
    ui.TableSetupColumn("Objective", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("Count", ui.TableColumnFlags.WidthFixed, 42 * s)
    ui.TableSetupColumn("Zone", ui.TableColumnFlags.WidthFixed, zoneW)
    ui.TableHeadersRow()
    for _, e in ipairs(rows) do
      ui.TableNextRow()
      ui.TableNextColumn()
      if e.separator then
        ui.Dummy(0, 4 * s)
      else
        local color = e.hidden and C.muted or e.done and C.pet or C.text
        local x, y = ui.GetCursorScreenPos()
        local _, lh = ui.CalcTextSize("Ag")
        if not e.hidden then K.Tick(x, y + (lh - 9 * s) / 2, 9 * s, e.done) end
        ui.Dummy(11 * s, lh)
        ui.SameLine(0, 4 * s)
        local label = e.text or ""
        local width = ui.GetContentRegionAvail()
        ui.TextColored(color[1], color[2], color[3], 1, T.Fit(label, width, T.font.small))
        if ui.IsItemHovered() and T.TextSize(label, T.font.small) > width then ui.SetTooltip(label) end
        ui.TableNextColumn()
        K.Muted(e.hidden and "" or (e.status or ""), e.done and C.pet or C.muted)
        ui.TableNextColumn()
        local zone = e.zone or ""
        if zone == "ALL" then zone = "Anywhere" end
        K.Muted(T.Fit(zone, ui.GetContentRegionAvail(), T.font.small))
        if ui.IsItemHovered() then ui.SetTooltip(zone) end
      end
    end
    ui.EndTable()
  end
  ui.PopStyleVar()
  ui.PopFont()
end

local function historyObjectives(s)
  local rows = K.Rows(WINDOW, "QUEST_TaskElementList")
  local objectives = {}
  for _, row in ipairs(rows) do
    local label = row.texts[1] or ""
    local separator = label == "" and (row.texts[2] or "") == "" and (row.texts[3] or "") == ""
    objectives[#objectives + 1] = { separator = separator, text = label, status = row.texts[2], zone = row.texts[3],
      hidden = not separator and label:match("^[%s%?]*$") ~= nil, done = row.texts[2] == "Done" }
  end
  objectiveTable(objectives, s)
end

local SECTIONS = { { "Objectives", "objectives", "section.objectives" },
  { "Description", "description", "section.description" }, { "Rewards", "rewards", "section.rewards" } }
-- a task's setting name for its section, made once a task rather than a frame
local sectionKeys, nSectionKeys = {}, 0
local function sectionTabs(key, s)
  local setting = sectionKeys[key]
  if not setting then
    if nSectionKeys >= 256 then sectionKeys, nSectionKeys = {}, 0 end
    setting = "tasks.section." .. tostring(key)
    sectionKeys[key], nSectionKeys = setting, nSectionKeys + 1
  end
  local section = ui.GetSetting(setting) or "objectives"
  if section ~= "description" and section ~= "rewards" then section = "objectives" end
  for i, tab in ipairs(SECTIONS) do
    if i > 1 then ui.SameLine(0, 5 * s) end
    if button(tab[1], tab[3], section == tab[2], s) then
      section = tab[2]
      ui.SetSetting(setting, section)
    end
  end
  ui.Dummy(0, 2 * s)
  return section
end

local function activeDetail(task, s)
  if not task then K.Empty("Select a task to see its objectives."); return end
  K.Muted(KIND[task.type] or "Task", C.gold)
  ui.SameLine(0, 0)
  local x, y = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x + math.max(0, ui.GetContentRegionAvail() - 85 * s), y - 3 * s)
  local tracked = Q.isTracked(task)
  if button(tracked and "Tracked" or "Track", "trackQuest", tracked, s) then Q.setTracked(task, not tracked) end
  if ui.IsItemHovered() then ui.SetTooltip("Show this quest in the independent Quest Tracker") end
  ui.PushFont(T.font.large)
  ui.PushTextWrapPos(0)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, task.title or "")
  ui.PopTextWrapPos()
  ui.PopFont()
  local done, total = Q.stats(task)
  local timed = (task.duration or 0) > 0
  local label = timed and ((task.left or 0) > 0 and Q.time(task.left) .. " left" or "Out of time") or "No time limit"
  local color = timed and (task.left or 0) <= 120 and C.danger or C.muted
  K.Muted(label, color)
  ui.SameLine(0, 12 * s)
  K.Muted(string.format("%d / %d complete", done, total))
  local bx, by = ui.GetCursorScreenPos()
  local bw = ui.GetContentRegionAvail()
  ui.DrawRectFilled(bx, by, bx + bw, by + 3 * s, C.inset[1], C.inset[2], C.inset[3], 1, 2 * s)
  if total > 0 and done > 0 then ui.DrawRectFilled(bx, by, bx + bw * done / total, by + 3 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s) end
  ui.Dummy(0, 6 * s)
  local section = sectionTabs(task.id, s)
  local _, lh = ui.CalcTextSize("Ag")
  local tasks = Q.tasks()
  local hasTimer = (tasks.requestTimerLeft or 0) > 0
  local footH = lh + 18 * s + (hasTimer and lh + 5 * s or 0)
  ui.BeginChild("##questContent", 0, -footH, false)
  if section == "objectives" then objectiveTable(task.objectives or {}, s)
  elseif section == "description" then
    if K.HasText(task.description) then K.Stml(WINDOW, "TASK_Description", task.description)
    else K.Empty("No description supplied.") end
  else
    if K.HasText(task.rewards) then K.Stml(WINDOW, "TASK_Description", task.rewards)
    elseif task.rewardSet then K.Empty("Reward details are available from the server.", "Use Preview Reward to view this quest's rewards.")
    else K.Empty("Reward preview unavailable.", "No reward details supplied for this quest.") end
  end
  ui.EndChild()
  ui.Separator()
  if hasTimer then
    K.Muted("Next request in " .. Q.time(tasks.requestTimerLeft))
  end
  W.Button(WINDOW, "TASK_PreviewRewardButton", "Preview Reward", { height = lh + 8 * s,
    tooltip = "Ask the server to preview the selected task's rewards" })
  if API.windows.ChildShown(WINDOW, "TASK_MonsterButton") == true then
    ui.SameLine(0, 8 * s)
    W.Button(WINDOW, "TASK_MonsterButton", "Select Monster", { primary = true, height = lh + 8 * s })
  end
  ui.SameLine(0, 8 * s)
  W.Button(WINDOW, "TASK_AbandonButton", "Abandon...", { danger = true, height = lh + 8 * s,
    tooltip = "The game asks for confirmation before abandoning this task" })
end

local function historyDetail(rows, selected, s)
  local task
  for _, row in ipairs(rows) do if row.row == selected then task = row; break end end
  if not task then K.Empty("Select a completed quest to review it."); return end
  K.Muted("Completed", C.pet)
  ui.PushFont(T.font.large)
  ui.PushTextWrapPos(0)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, task.texts[2] or "")
  ui.PopTextWrapPos()
  ui.PopFont()
  K.Muted(task.texts[3] or "")
  local section = sectionTabs("history", s)
  ui.BeginChild("##questHistoryContent", 0, 0, false)
  if section == "objectives" then historyObjectives(s)
  elseif section == "description" then K.Empty("No description in completed-task history.")
  else K.Empty("No reward details in completed-task history.") end
  ui.EndChild()
end

-- ---------------------------------------------------------------- advanced (crew) view
-- The crew's journals as ACNet publishes them natively (ezquests/progress.v3, the EZQuests
-- snapshot format): every peer's active tasks in one list, the selected task's objectives
-- with each owner's status. This character's own tasks come from hs.Tasks as usual; a peer's
-- snapshot is decoded only when its revision changes (the hex can be large).
local QUEST_TOPIC = "ezquests/progress.v3"
local advCache = {} -- [sender] = { revision, tasks }
local advSelected, advSource, advFilter = nil, nil, "all"

local function decodeQuests(hex)
  if type(hex) ~= "string" or hex == "ffff" or #hex % 2 ~= 0 or #hex > 2 * 64 * 1024 then return nil end
  local ok, tasks = pcall(function()
    local bytes = (hex:gsub("..", function(h) return string.char(tonumber(h, 16) or 0) end))
    local count, pos = string.unpack("<I2", bytes)
    if count > 512 then error("count") end
    local out = {}
    for _ = 1, count do
      local id, kind, duration, left, title, members, leader, objectives
      id, kind, duration, left, title, members, leader, objectives, pos =
        string.unpack("<I4BI4I4s2I2s2I2", bytes, pos)
      if objectives > 512 then error("objectives") end
      local task = { id = id, type = kind, duration = duration, left = left, title = title,
        members = members, leader = leader, objectives = {} }
      for i = 1, objectives do
        local oid, group, flags, ocount, goal, text, status, zone
        oid, group, flags, ocount, goal, text, status, zone, pos =
          string.unpack("<I4I4Bi4i4s2s2s2", bytes, pos)
        task.objectives[i] = { id = oid, group = group, hidden = (flags & 1) ~= 0,
          done = (flags & 2) ~= 0, optional = (flags & 4) ~= 0, count = ocount, goal = goal,
          text = text, status = status, zone = zone }
      end
      out[#out + 1] = task
    end
    if pos ~= #bytes + 1 then error("trailing") end
    return out
  end)
  return ok and tasks or nil
end

local function questPeers()
  local plugin = API.plugins and API.plugins.Get("ACNet")
  if not plugin or not plugin.BusState then return {} end
  local revs = plugin.BusState(QUEST_TOPIC) or {}
  local me = (API.character and API.character.Label(1) or ""):lower()
  local present, out = {}, {}
  for name, rev in pairs(revs) do
    if name ~= me then
      present[name] = true
      local cached = advCache[name]
      if not cached or cached.revision ~= rev then
        cached = { revision = rev, tasks = plugin.BusStateHex and decodeQuests(plugin.BusStateHex(QUEST_TOPIC, name)) }
        advCache[name] = cached
      end
      if cached.tasks then
        out[#out + 1] = { key = name, name = name:sub(1, 1):upper() .. name:sub(2), tasks = cached.tasks }
      end
    end
  end
  for name in pairs(advCache) do if not present[name] then advCache[name] = nil end end
  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end

local function advProgress(objectives)
  local done, total = 0, 0
  for _, o in ipairs(objectives or {}) do
    if not o.hidden and not o.separator then
      total = total + 1
      if o.done then done = done + 1 end
    end
  end
  return done, total
end

-- Unified entries across owners: key "type|id", owners[key] = { task, objectives, done, total }.
local function advEntries(localTasks, peers)
  local entries, order = {}, {}
  local function add(ownerKey, task, objectives)
    local key = (task.type or 0) .. "|" .. (task.id or 0)
    local entry = entries[key]
    if not entry then
      entry = { key = key, id = task.id, type = task.type, title = task.title or "", owners = {} }
      entries[key], order[#order + 1] = entry, entry
    end
    local done, total = advProgress(objectives)
    entry.owners[ownerKey] = { task = task, objectives = objectives, done = done, total = total }
    if ownerKey == "@self" then entry.title = task.title or entry.title end
  end
  for _, task in ipairs(localTasks) do add("@self", task, task.objectives) end
  for _, peer in ipairs(peers) do
    for _, task in ipairs(peer.tasks) do add(peer.key, task, task.objectives) end
  end
  local ownerCount = 1 + #peers
  for _, entry in ipairs(order) do
    local have, allDone = 0, true
    for _, owner in pairs(entry.owners) do
      have = have + 1
      if owner.total == 0 or owner.done < owner.total then allDone = false end
    end
    entry.have, entry.ownerCount, entry.allDone = have, ownerCount, have > 0 and allDone
  end
  table.sort(order, function(a, b)
    if a.allDone ~= b.allDone then return not a.allDone end
    return (a.title or ""):lower() < (b.title or ""):lower()
  end)
  return order
end

-- The visible objectives of one owner's copy, in order (EZQuests' numbering and
-- per-peer matching are by position in this list).
local function advVisible(objectives)
  local out = {}
  for _, o in ipairs(objectives or {}) do
    if not o.hidden and not o.separator then out[#out + 1] = o end
  end
  return out
end

local function advObjStatus(o)
  if o.status and o.status ~= "" then return o.status end
  if o.done then return "Done" end
  if (o.goal or 0) > 0 then return string.format("%d/%d", o.count or 0, o.goal) end
  return "In Progress"
end

-- EZQuests' Advanced right pane: title, type, a "View objectives from" source
-- picker, status and current step, a "Who has this task" summary table (one row
-- per owner, however many), then the chosen source's objectives in three
-- columns. Per-owner columns do not scale to a 24-character crew; the other
-- owners' progress on an objective is in its Progress cell's tooltip instead.
local function advDetail(entry, s, myName)
  if not entry then K.Empty("Select a task to view details."); return end
  -- owners who have it: you first, then peers by name
  local owners = {}
  if entry.owners["@self"] then owners[#owners + 1] = { key = "@self", name = myName } end
  local peerNames = {}
  for key in pairs(entry.owners) do if key ~= "@self" then peerNames[#peerNames + 1] = key end end
  table.sort(peerNames)
  for _, key in ipairs(peerNames) do
    owners[#owners + 1] = { key = key, name = key:sub(1, 1):upper() .. key:sub(2) }
  end
  -- the source: nil is Auto (self if available, else the first owner)
  local sourceKey = advSource
  if sourceKey and not entry.owners[sourceKey] then sourceKey = nil advSource = nil end
  if not sourceKey then sourceKey = entry.owners["@self"] and "@self" or (owners[1] and owners[1].key) end
  local source = sourceKey and entry.owners[sourceKey]
  local visible = source and advVisible(source.objectives) or {}
  local srcDone, srcTotal = advProgress(source and source.objectives)
  local complete = srcTotal > 0 and srcDone == srcTotal

  ui.PushFont(T.font.large)
  ui.PushTextWrapPos(0)
  ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, entry.title or "")
  ui.PopTextWrapPos()
  ui.PopFont()
  K.Muted("Type: " .. (KIND[entry.type] or "Task"))
  ui.Separator()

  local preview = "Auto (Self if available)"
  if advSource == "@self" then preview = "Self (" .. myName .. ")"
  elseif advSource then preview = advSource:sub(1, 1):upper() .. advSource:sub(2) end
  ui.Text("View objectives from:")
  ui.SameLine(0, 6 * s)
  ui.SetNextItemWidth(220 * s)
  if ui.BeginCombo("##advsource", preview) then
    if ui.Selectable("Auto (Self if available)", advSource == nil) then advSource = nil end
    for _, owner in ipairs(owners) do
      local label = owner.key == "@self" and ("Self (" .. myName .. ")") or owner.name
      if ui.Selectable(label .. "##advsource." .. owner.key, advSource == owner.key) then advSource = owner.key end
    end
    ui.EndCombo()
  end
  ui.Separator()

  if not source then K.Empty("No objective data available from selected source."); return end
  local statusCol = complete and C.pet or C.gold
  ui.TextColored(statusCol[1], statusCol[2], statusCol[3], 1,
    "Status: " .. (complete and "Complete" or "In Progress"))
  for _, o in ipairs(visible) do
    if not o.done then
      local a = C.accent
      ui.TextColored(a[1], a[2], a[3], 1, "Current Step:")
      ui.PushTextWrapPos(0)
      ui.TextWrapped(o.text or "")
      ui.PopTextWrapPos()
      break
    end
  end
  ui.Separator()

  if ui.CollapsingHeader("Who has this task:###advwho", 32) then
    local flags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV
    local _, lh = ui.CalcTextSize("Ag")
    local tableH = #owners > 8 and (lh + 6 * s) * 9 or 0
    if tableH > 0 then flags = flags | ui.TableFlags.ScrollY end
    if ui.BeginTable("##advwhohas", 3, flags, 0, tableH) then
      ui.TableSetupColumn("Peer", ui.TableColumnFlags.WidthStretch, 0)
      ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 110 * s)
      ui.TableSetupColumn("Progress", ui.TableColumnFlags.WidthFixed, 80 * s)
      ui.TableHeadersRow()
      for _, owner in ipairs(owners) do
        local progress = entry.owners[owner.key]
        ui.TableNextRow()
        ui.TableNextColumn()
        if ui.Selectable(owner.name .. "##advwho." .. owner.key, sourceKey == owner.key,
            ui.SelectableFlags and ui.SelectableFlags.SpanAllColumns or 0) then
          advSource = owner.key
        end
        ui.TableNextColumn()
        local col = (progress.total > 0 and progress.done == progress.total) and C.pet or C.text
        ui.TextColored(col[1], col[2], col[3], 1,
          (progress.total > 0 and progress.done == progress.total) and "Complete" or "In Progress")
        ui.TableNextColumn()
        ui.Text(string.format("%d/%d", progress.done, progress.total))
      end
      ui.EndTable()
    end
  end
  ui.Separator()

  if #visible == 0 then K.Empty("No objectives found for this task yet."); return end
  ui.PushFont(T.font.small)
  ui.PushStyleVar(ui.StyleVar.CellPadding, 4 * s, 1 * s)
  if ui.BeginTable("##advObjectives", 3, ui.TableFlags.SizingFixedFit | ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV, 0, 0) then
    ui.TableSetupColumn("Objective", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("Progress", ui.TableColumnFlags.WidthFixed, 110 * s)
    ui.TableSetupColumn("Zone", ui.TableColumnFlags.WidthFixed, 140 * s)
    ui.TableHeadersRow()
    for i, objective in ipairs(visible) do
      ui.TableNextRow()
      ui.TableNextColumn()
      local label = string.format("%d. %s", i, objective.text or "")
      if objective.optional then label = label .. " (Optional)" end
      ui.TextWrapped(label)
      ui.TableNextColumn()
      local col = objective.done and C.pet or C.gold
      ui.TextColored(col[1], col[2], col[3], 1, advObjStatus(objective))
      if ui.IsItemHovered() then
        local lines = {}
        for _, owner in ipairs(owners) do
          local theirs = advVisible(entry.owners[owner.key].objectives)[i]
          if theirs then lines[#lines + 1] = owner.name .. ": " .. advObjStatus(theirs) end
        end
        if #lines > 0 then ui.SetTooltip(table.concat(lines, "\n")) end
      end
      ui.TableNextColumn()
      local zone = objective.zone or ""
      ui.Text(zone ~= "" and zone ~= "ALL" and zone or "Any")
    end
    ui.EndTable()
  end
  ui.PopStyleVar()
  ui.PopFont()
end

local function advancedView(localTasks, s, width, height)
  local peers = questPeers()
  local entries = advEntries(localTasks, peers)
  -- the owner filter: everyone, you, or one peer (a name that left falls back to everyone)
  if advFilter ~= "all" and advFilter ~= "@self" then
    local found = false
    for _, peer in ipairs(peers) do if peer.key == advFilter then found = true break end end
    if not found then advFilter = "all" end
  end
  local shown = {}
  for _, entry in ipairs(entries) do
    if advFilter == "all" or entry.owners[advFilter] then shown[#shown + 1] = entry end
  end
  if not advSelected then advSelected = shown[1] and shown[1].key end
  local selected
  for _, entry in ipairs(shown) do if entry.key == advSelected then selected = entry break end end
  if not selected and #shown > 0 then selected, advSelected = shown[1], shown[1].key end

  local listW = math.max(170 * s, math.min((tonumber(ui.GetSetting("tasks.listw")) or state.listW) * s, width * 0.38))
  ui.BeginChild("##advSidebar", listW, height, false)
  -- the owner filter drop-down
  local filterName = advFilter == "all" and "All Peers" or advFilter == "@self" and "Self"
    or (advFilter:sub(1, 1):upper() .. advFilter:sub(2))
  ui.SetNextItemWidth(-1)
  if ui.BeginCombo("##advpeers", filterName) then
    if ui.Selectable("All Peers", advFilter == "all") then advFilter = "all" end
    if ui.Selectable("Self", advFilter == "@self") then advFilter = "@self" end
    for _, peer in ipairs(peers) do
      if ui.Selectable(peer.name .. "##" .. peer.key, advFilter == peer.key) then advFilter = peer.key end
    end
    ui.EndCombo()
  end
  local needle = K.Search(state, "advsearch", "Search crew quests")
  K.Muted("Tasks")
  ui.Separator()
  ui.BeginChild("##advList", 0, 0, false)
  if #peers == 0 then K.Empty("No peers reporting yet.") end
  -- One line per task, EZQuests' "[Type] (# who have it) Task Name": a crew of
  -- 24 has far too many quests for two-line cards.
  local ADV_KIND = { [0] = "Task", [1] = "Shared", [2] = "Quest" }
  local any = false
  ui.PushFont(T.font.small)
  for _, entry in ipairs(shown) do
    if needle == "" or (entry.title or ""):lower():find(needle, 1, true) then
      any = true
      local label = string.format("[%s] (%d) %s", ADV_KIND[entry.type] or "???", entry.have, entry.title or "")
      if ui.Selectable(label .. "##adv." .. entry.key, entry.key == advSelected) then
        advSelected, advSource = entry.key, nil
      end
      if ui.IsItemHovered() then
        local mine = entry.owners["@self"]
        ui.SetTooltip(string.format("%s\n%d of %d have it%s", entry.title or "", entry.have, entry.ownerCount,
          mine and string.format("  \xC2\xB7  you %d/%d", mine.done, mine.total) or ""))
      end
    end
  end
  ui.PopFont()
  if not any then K.Empty("No matching tasks.") end
  ui.EndChild()
  ui.EndChild()
  ui.SameLine(0, 14 * s)
  ui.BeginChild("##advDetail", 0, height, false)
  advDetail(selected, s, myName)
  ui.EndChild()
end

local WINDOW_OPTS = {
  size = function() return 820, 560 end,
  resizable = { 620, 360 },
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 30 end,
}
local function drawWindow(_, _, _, _, s)
  local tasks = Q.tasks()
  sync(tasks)
  local history, historySelected = K.Rows(WINDOW, "QUEST_TaskList")
  categories(tasks, history, s)
  sharedMembers(tasks, s)
  local advanced = ui.GetSetting("tasks.advanced") == "1"
  if button(advanced and "Advanced: on" or "Advanced", "advancedView", advanced, s) then
    ui.SetSetting("tasks.advanced", advanced and "0" or "1")
    advanced = not advanced
  end
  if ui.IsItemHovered() then
    ui.SetTooltip("The crew's quests together (ACNet quest snapshots), instead of only yours")
  end
  local width, height = ui.GetContentRegionAvail()
  if advanced then
    ui.Dummy(0, 4 * s)
    advancedView(tasks, s, width, height - 30 * s)
    return
  end
  local listW = math.max(155 * s, math.min((tonumber(ui.GetSetting("tasks.listw")) or state.listW) * s, width * 0.38))
  ui.BeginChild("##questSidebar", listW, height, false)
  local needle = K.Search(state, "search", state.category == "history" and "Search history" or "Search quests")
  local _, lh = ui.CalcTextSize("Ag")
  ui.BeginChild("##questList", 0, -(lh + 18 * s), false)
  if state.category == "history" then historyList(history, historySelected, needle, s)
  else taskList(tasks, needle, s) end
  ui.EndChild()
  local visible = Q.trackerVisible() and (#Q.getTracked(tasks) > 0 or ui.GetSetting("tasks.tracker.visible") == "1")
  if ui.Button(visible and "Hide tracker" or "Show tracker", -1, lh + 8 * s) then Q.setTrackerVisible(not visible) end
  ui.EndChild()
  ui.SameLine(0, 0)
  local x, y = ui.GetCursorScreenPos()
  T.Hit("##questSplit", x + 2 * s, y, x + 10 * s, y + height)
  local hot = ui.IsItemHovered() or ui.IsItemActive()
  if ui.IsItemActive() then
    local mx = ui.GetMousePos()
    local wx = ui.GetWindowPos()
    state.listW = math.max(155, math.min((mx - wx - 10 * s) / s, width * 0.38 / s))
    ui.SetSetting("tasks.listw", tostring(math.floor(state.listW)))
  end
  ui.DrawLine(x + 6 * s, y, x + 6 * s, y + height, C.ink[1], C.ink[2], C.ink[3], hot and 0.30 or 0.10, 1)
  ui.SetCursorScreenPos(x + 14 * s, y)
  ui.BeginChild("##questDetail", 0, height, false)
  if state.category == "history" then historyDetail(history, historySelected, s)
  else activeDetail(find(tasks, state.selected), s) end
  ui.EndChild()
end

ui.window("Tasks", function()
  if not API.session.InGame() then
    state.loaded, state.selected, state.nativeId, state.categoryPref = false, nil, nil, nil
    state.openMembers, state.memberName, state.memberTaskId = nil, nil, nil
    state.category, state.search = "active", ""
    Q.invalidate()
    return
  end
  hud.Dialog(WINDOW, "Quest Journal", WINDOW_OPTS, drawWindow)
end)
