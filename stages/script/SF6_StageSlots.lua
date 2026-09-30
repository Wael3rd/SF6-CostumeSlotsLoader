-- SF6 Stage Slots
-- On the stage select screen, UP / DOWN cycles the focused stage between its vanilla look and the
-- stage mods installed for it (reframework/stage_mods). The loader (amd_ags_x64.dll) serves the
-- chosen variant's files when the stage loads; this script keeps the choice
-- (data/SF6_StageSlots_Data/state.json, read by the loader) and shows it: stage name, preview.
--
-- Game thread only: the InputUp / InputDown hooks just count presses, every GUI write happens in
-- LateUpdateBehavior. Texts are compared before being written (an identical set_Message crashes).

local REG_PATH   = "SF6_StageSlots_Data/registry.json"
local STATE_PATH = "SF6_StageSlots_Data/state.json"

-- ---------------------------------------------------------------------------------------------
-- Variants and selection
-- ---------------------------------------------------------------------------------------------

local variants_of = {}                 -- stage id -> { {key, name, author, preview}, ... }
do
    local reg = json.load_file(REG_PATH)
    if reg and reg.stages then
        for _, st in ipairs(reg.stages) do
            if st.stage_id and st.variants and #st.variants > 0 then variants_of[st.stage_id] = st.variants end
        end
    end
end

local state = json.load_file(STATE_PATH)
if type(state) ~= "table" then state = {} end
if type(state.selected) ~= "table" then state.selected = {} end

-- 0 = vanilla, i = variants_of[stage][i]
local function selected_index(stage)
    local key = state.selected[tostring(stage)]
    local list = variants_of[stage]
    if not key or not list then return 0 end
    for i, v in ipairs(list) do if v.key == key then return i end end
    return 0
end

local function set_selected(stage, idx)
    local list = variants_of[stage]
    if idx == 0 or not list then state.selected[tostring(stage)] = nil
    else state.selected[tostring(stage)] = list[idx].key end
    json.dump_file(STATE_PATH, state)
end

-- ---------------------------------------------------------------------------------------------
-- The stage select screen
-- ---------------------------------------------------------------------------------------------

local T_FLOW = sdk.typeof("app.battle.bBattleStageSelectFlow")
local M_GET_TEXTURE = sdk.find_type_definition("via.gui.Texture"):get_method("getTexture")

local scr = { param = nil, agent_addr = nil, stage = nil, settle = 0, vanilla_name = {}, applied = {}, written = {} }
local SETTLE_FRAMES = 3           -- after a focus change, the game rewrites the name and the image first
local pending = 0                      -- UP / DOWN presses counted by the hooks, used next LateUpdate
local status = "not on the stage select screen"

local function current_scene()
    local sm = sdk.get_native_singleton("via.SceneManager")
    return sdk.call_native_func(sm, sdk.find_type_definition("via.SceneManager"), "get_CurrentScene")
end

local function find_stage_select()
    local scene = current_scene()
    if not scene then return nil end
    local comps = scene:call("findComponents(System.Type)", T_FLOW)
    if not comps or comps:call("get_Count") == 0 then return nil end
    return comps:call("get_Item", 0):get_field("mStageSelect")
end

-- The StageSelect agent while it is on screen, or nil
local function stage_select_agent()
    local mgr = sdk.get_managed_singleton("app.UIAgentManager")
    local list = mgr and mgr:get_field("_Entries")
    if not list then return nil end
    for i = 0, list:call("get_Count") - 1 do
        local agent = list:call("get_Item", i).Agent
        local go = agent and agent:call("get_GameObject")
        if go and go:call("get_Name") == "StageSelect" then
            local cm = agent:call("get_ControlMain")
            if cm and cm:call("get_ActualVisible") then return agent end
            return nil
        end
    end
    return nil
end

local function focused_stage(param)
    local list = param:call("get_StageIdList")
    local idx = param:call("GetSelectIndex")
    if list and idx and idx >= 0 and idx < list:call("get_Count") then return list:call("get_Item", idx) end
    return nil
end

-- Preview textures of the variants, created once and kept for the session. A texture resource
-- loads in the background: handing it to the GUI while it loads crashed the game (30/09), so a
-- holder is only used LOAD_FRAMES after its creation, and the previews of a stage are created as
-- soon as the stage is focused.
local SHOW_PREVIEWS = true
local LOAD_FRAMES = 60
local frame_no = 0
local holders = {}                     -- path -> { holder, frame } or false

local function preview_holder(path)
    if not SHOW_PREVIEWS or not path or path == "" then return nil end
    local e = holders[path]
    if e == nil then
        e = false
        local res = sdk.create_resource("via.render.TextureResource", path)
        if res then
            res:add_ref()
            local holder = res:create_holder("via.render.TextureResourceHolder")
            if holder then holder:add_ref(); e = { holder = holder, frame = frame_no } end
        end
        holders[path] = e
    end
    if not e or frame_no - e.frame < LOAD_FRAMES then return nil end
    return e.holder
end

local function preload_previews(stage)
    for _, v in ipairs(variants_of[stage] or {}) do preview_holder(v.preview) end
end

local function vanilla_holder(param, stage)
    local cache = param:get_field("PreviewTexCahceDataList")
    if not cache then return nil end
    for i = 0, cache:call("get_Count") - 1 do
        local c = cache:call("get_Item", i)
        if c:get_field("StageId") == stage then return c:get_field("Texture") end
    end
    return nil
end

local function set_text(text_obj, s)
    if text_obj:call("get_Message") == s then return end
    local ms = sdk.create_managed_string(s)
    ms:add_ref()
    text_obj:call("set_Message", ms)
end

-- A TextureResourceHolder keeps its native resource at +0x10; getTexture returns a new holder
-- every call, so textures are compared by resource.
local function resource_of(holder) return holder and holder:read_qword(0x10) or 0 end

local function set_texture(tex_obj, holder)
    if not holder then return end
    local cur = M_GET_TEXTURE:call(tex_obj)
    if cur and resource_of(cur) == resource_of(holder) then return end
    tex_obj:call("setTexture", holder)
end

-- The game's own name of a stage: read once the game has written it, never one of ours
local function learn_vanilla_name(text0, stage)
    local cur = text0:call("get_Message")
    if not cur or cur == "" or cur == scr.written[stage] then return end
    scr.vanilla_name[stage] = cur
end

-- A variant named like the stage itself (a lighting pack: "Bather's Beach") shows its bundle
-- name instead, so it cannot be mistaken for the vanilla stage.
local function norm(s) return (s or ""):lower():gsub("^%s+", ""):gsub("%s+$", "") end
local function display_name(stage, v)
    local vn = scr.vanilla_name[stage]
    if vn and v.bundle and v.bundle ~= "" and norm(v.name) == norm(vn) then return v.bundle end
    return v.name
end

-- Shows the selected variant of the focused stage (or puts the game's own name/image back)
local function apply(param, stage)
    local text0, tex0 = param:get_field("text0"), param:get_field("texture0")
    if not text0 or not tex0 then return end
    local idx = selected_index(stage)
    if idx == 0 then
        if scr.applied[stage] then
            if scr.vanilla_name[stage] then set_text(text0, scr.vanilla_name[stage]) end
            scr.written[stage] = nil
            set_texture(tex0, vanilla_holder(param, stage))
            scr.applied[stage] = nil
        end
        learn_vanilla_name(text0, stage)
        return
    end
    if not scr.applied[stage] then learn_vanilla_name(text0, stage) end
    local v = variants_of[stage][idx]
    local name = display_name(stage, v)
    set_text(text0, name)
    scr.written[stage] = name
    if v.preview and v.preview ~= "" then set_texture(tex0, preview_holder(v.preview))
    else set_texture(tex0, vanilla_holder(param, stage)) end
    scr.applied[stage] = v.key
end

local function on_late_update()
    frame_no = frame_no + 1
    local agent = stage_select_agent()
    if not agent then
        if scr.param then status = "not on the stage select screen" end
        scr.param, scr.agent_addr, scr.stage = nil, nil, nil
        scr.applied, scr.written = {}, {}
        pending = 0
        return
    end
    scr.agent_addr = agent:get_address()
    if not scr.param or not sdk.is_managed_object(scr.param) then
        scr.param = find_stage_select()
        if not scr.param then status = "stage select: flow not found"; return end
    end
    local param = scr.param
    local stage = focused_stage(param)
    if stage == nil then return end
    if stage ~= scr.stage then
        scr.stage = stage
        scr.applied[stage], scr.written[stage] = nil, nil   -- the game rewrites the name and the image
        scr.settle = SETTLE_FRAMES
        preload_previews(stage)
    end
    if scr.settle > 0 then scr.settle = scr.settle - 1; pending = 0; return end
    local list = variants_of[stage]
    if list and pending ~= 0 then
        local n = #list + 1
        local idx = (selected_index(stage) + pending) % n
        if idx < 0 then idx = idx + n end
        set_selected(stage, idx)
    end
    pending = 0
    if list then apply(param, stage) end
    local idx = selected_index(stage)
    status = string.format("stage %d: %s (%d/%d)", stage,
        idx == 0 and "vanilla" or list[idx].name, idx, list and #list or 0)
end

re.on_pre_application_entry("LateUpdateBehavior", function()
    local ok, err = pcall(on_late_update)
    if not ok then status = "error: " .. tostring(err) end
end)

-- ---------------------------------------------------------------------------------------------
-- UP / DOWN: the game hands menu directions to the focused UI agent (InputUp / InputDown, with
-- the key configuration applied). Presses on the StageSelect agent are counted, never swallowed.
-- ---------------------------------------------------------------------------------------------

local FLAG_TRIGGER, FLAG_REPEAT = 2, 8

local function hook_direction(name, step)
    local td = sdk.find_type_definition("app.UIAgent")
    local m = td and td:get_method(name .. "(app.InputDigitalFlag)")
    if not m then return end
    sdk.hook(m, function(args)
        if scr.agent_addr and sdk.to_int64(args[2]) == scr.agent_addr then
            local flag = sdk.to_int64(args[3]) & 0xF
            if (flag & (FLAG_TRIGGER | FLAG_REPEAT)) ~= 0 then pending = pending + step end
        end
    end, function(rv) return rv end)
end
hook_direction("InputUp", -1)
hook_direction("InputDown", 1)

re.on_draw_ui(function()
    if imgui.tree_node("SF6 Stage Slots") then
        local n = 0
        for _, list in pairs(variants_of) do n = n + #list end
        imgui.text(string.format("%d stage variant(s) installed", n))
        imgui.text(status)
        imgui.tree_pop()
    end
end)
