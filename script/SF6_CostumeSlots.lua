-- SF6_CostumeSlots — slots de costume ajoutes (pak patch) jouables EN LIGNE par alias sur DriveTech.
-- Multi-persos : table SLOTS[fighter][costume_no] et etat par perso dans state.json.
--   * Menu Battle Settings : pour chaque perso qui a des slots, le joueur choisit un slot (costume 5+). A la fermeture
--     du menu le script note slot + couleur dans state.json par perso et ecrit BASE (DriveTech) dans la sauvegarde
--     -> hors du menu, la sauvegarde ne contient jamais que du vanilla, c'est tout ce que le reseau voit.
--   * A l'ouverture du menu, si l'intention est notee, on remet SLOT dans la sauvegarde pour que le menu affiche le
--     slot au bon endroit. Choisir le vrai DriveTech (ou autre) efface l'intention.
--   * Tant que l'intention est notee : dossier esfXXXv<SLOT> garde monte + manifeste recopie sur celui de v<BASE>
--     (SettingData.overwrite natif) des que le jeu monte DriveTech -> le jeu dit DriveTech, l'ecran montre le mod.
--   * Couleur : DriveTech n'a que les couleurs existantes dans la table master ; une autre couleur annoncee au
--     reseau = erreur de communication. A la fermeture du menu on note la couleur voulue et on ecrit BASE_COLOR
--     dans la sauvegarde ; au montage, on echange les donnees de materiaux dans le CCVD. Remis en place au demontage.
--   * Gate : rien pendant l'ecran de selection (UIFlowUI105*/SelectFighter*).
--   * Miroir : ne remplacer que le PREMIER holder v04 trouve (limite connue : si l'adversaire a aussi DriveTech
--     du meme perso, son v04 pourrait etre le premier — a verifier en miroir).
-- AUCUN hook, tout depuis LateUpdateBehavior.
local BASE_COS = 4  -- DriveTech = costume 4 pour tous les persos
local STATE_FILE = "SF6_CostumeSlots_data/state.json"
local MENU_FLOW = "app.UIFlowMatchingSetting.Param"

-- ---- registre ecrit par le loader : section possession = { fighter, costume_no, record_id, ... } ----
local REGISTRY = json.load_file("SF6_Costumes_Data/registry.json")
local SLOTS = {}       -- SLOTS[fighter][costume_no] = "product/content/esf/fighterNNN/esfNNNvNN"
local POSSESSION = {}  -- flat list (toutes les entrees possession du registre, tous persos)
local FIGHTERS = {}    -- liste ordonnee des fighter ids qui ont des slots
local FIGHTER_SET = {} -- lookup rapide : FIGHTER_SET[fighter] = true
if REGISTRY and REGISTRY.possession then
    for _, e in ipairs(REGISTRY.possession) do
        if e.fighter and e.costume_no then
            if not SLOTS[e.fighter] then
                SLOTS[e.fighter] = {}
                FIGHTERS[#FIGHTERS + 1] = e.fighter
                FIGHTER_SET[e.fighter] = true
            end
            SLOTS[e.fighter][e.costume_no] = string.format("product/content/esf/fighter%03d/esf%03dv%02d", e.fighter, e.fighter, e.costume_no)
            POSSESSION[#POSSESSION + 1] = e
        end
    end
end

-- ---- etat par perso et runtime par perso ----
local F = {}  -- F[fighter] = { src_folder, swapped_addr, color_swap, last_mount_frame }
for _, fid in ipairs(FIGHTERS) do
    F[fid] = { src_folder = nil, swapped_addr = nil, color_swap = nil, last_mount_frame = -1000 }
end
local DST = {}         -- DST[fighter] = chemin complet du dossier v04 (trouve par suffixe dans l'arbre de scene)
local DST_DONE = false
local BASE_COLORS = {} -- BASE_COLORS[fighter] = { [colorNo]=true, ... } couleurs de DriveTech lues dans la table master
local BASE_COLOR = {}  -- BASE_COLOR[fighter] = plus petit colorNo de DriveTech
local BC_DONE = false

-- ---- etat persistant (migration de l'ancien format mono-perso) ----
local frame = 0
local state = json.load_file(STATE_FILE) or {}
do
    -- ancien format : { intent = ... } -> { slot = ... }
    if state.intent ~= nil then
        state.slot = state.intent and 5 or false
        state.intent = nil
    end
    -- ancien format : { slot = 5, color = 3 } -> { fighters = { ["1"] = { slot = 5, color = 3 } } }
    if state.slot ~= nil then
        local old_s, old_c = state.slot, state.color
        state = { fighters = { ["1"] = { slot = old_s or false, color = old_c } } }
        json.dump_file(STATE_FILE, state)
    end
    if not state.fighters then state.fighters = {} end
end
local function fstate(f)
    local key = tostring(f)
    if not state.fighters[key] then state.fighters[key] = { slot = false } end
    return state.fighters[key]
end
local function SRC(f)
    local s = fstate(f)
    return s.slot and SLOTS[f] and SLOTS[f][s.slot]
end

-- ---- journal ----
local log = { events = {} }
local dirty, last_write = false, 0
local function ev(s)
    if #log.events < 300 then log.events[#log.events + 1] = os.date("%H:%M:%S") .. " f" .. frame .. " " .. s dirty = true end
end
local function set_slot(f, v, why)
    local fs = fstate(f)
    if fs.slot ~= v then
        fs.slot = v
        if not v then fs.color = nil end
        json.dump_file(STATE_FILE, state)
        ev("[F" .. f .. "] INTENTION slot = " .. tostring(v) .. " (" .. why .. ")")
        local ff = F[f]
        if ff then ff.src_folder, ff.swapped_addr = nil, nil end
    end
end

-- ---- sauvegarde : entrees MatchingFighterSetting pour tous les persos avec slots ----
local function save_entries()
    local mgr = sdk.get_managed_singleton("app.SystemSaveManager")
    local data = mgr and mgr:get_field("Data")
    local list = data and data:get_field("MatchingFighterSetting")
    if not list then return nil end
    local result = {}
    for i = 0, list:call("get_Count") - 1 do
        local e = list:call("get_Item", i)
        if e then
            local fid = e:get_field("FighterId")
            if FIGHTER_SET[fid] then result[fid] = e end
        end
    end
    return result
end
local function get_cos(e) return e:get_field("MatchingFighterCostume") end
local function get_col(e) return e:get_field("MatchingFighterColor") end
local function set_col(e, v, f, why)
    if get_col(e) ~= v then
        e:set_field("MatchingFighterColor", v)
        ev("[F" .. f .. "] sauvegarde MatchingFighterColor -> " .. tostring(v) .. " (" .. why .. ")")
    end
end
local function set_color_intent(f, c, why)
    local fs = fstate(f)
    if fs.color ~= c then
        fs.color = c
        json.dump_file(STATE_FILE, state)
        ev("[F" .. f .. "] INTENTION couleur = " .. tostring(c) .. " (" .. why .. ")")
    end
end
local function set_cos(e, v, f, why)
    if get_cos(e) ~= v then
        e:set_field("MatchingFighterCostume", v)
        ev("[F" .. f .. "] sauvegarde MatchingFighterCostume -> " .. tostring(v) .. " (" .. why .. ")")
    end
end

-- ---- menu Battle Settings ouvert ? ----
local function menu_open()
    local fm = sdk.get_managed_singleton("app.UIFlowManager")
    local hs = fm and fm:get_field("_Handles")
    if not hs then return false end
    for i = 0, hs:call("get_Count") - 1 do
        local h = hs:call("get_Item", i)
        local prm = h and h:get_field("<Param>k__BackingField")
        if prm and prm:get_type_definition():get_full_name() == MENU_FLOW then return true end
    end
    return false
end

-- ---- ecran de selection de personnage actif ? (le slot y est un vrai costume : l'alias n'a rien a faire, et toucher
-- aux dossiers pendant que cet ecran monte/demonte des costumes bloque le jeu — 3 ecrans noirs le 22/09) ----
local function select_screen_active()
    local fm = sdk.get_managed_singleton("app.UIFlowManager")
    local hs = fm and fm:get_field("_Handles")
    if not hs then return false end
    for i = 0, hs:call("get_Count") - 1 do
        local h = hs:call("get_Item", i)
        local prm = h and h:get_field("<Param>k__BackingField")
        if prm then
            local tn = prm:get_type_definition():get_full_name()
            if tn:find("UIFlowUI105") or tn:find("SelectFighter") then return true end
        end
    end
    return false
end

-- ---- scene et dossiers ----
local function scene()
    local sm = sdk.get_native_singleton("via.SceneManager")
    return sdk.call_native_func(sm, sdk.find_type_definition("via.SceneManager"), "get_CurrentScene")
end
-- Un dossier de slot vit sous "product/content/esf/..." pour un perso de base mais sous "ver_02_0300/content/esf/..."
-- pour un perso DLC (slots declares dans version_update/ver_02_0300/esf.scn.20) : comparer par suffixe "content/esf/fighterNNN/esfNNNvNN".
local function same_slot_path(actual, wanted)
    if actual == wanted then return true end
    local sfx = wanted:match("(content/esf/fighter%d+/esf%d+v%d+)$")
    return sfx ~= nil and actual:sub(-#sfx) == sfx
end
local function find_folder(path)
    local found
    local function walk(f, depth)
        if not f or found or depth > 8 then return end
        local okp, p = pcall(f.call, f, "get_Path")
        if okp and same_slot_path(tostring(p), path) then found = f return end
        local okc, c = pcall(f.call, f, "get_Child")
        if okc and c then walk(c, depth + 1) end
        local okn, n = pcall(f.call, f, "get_Next")
        if okn and n then walk(n, depth) end
    end
    local sc = scene()
    if sc then walk(sc:call("get_FirstFolder"), 0) end
    return found
end

-- ---- DST par perso : trouver le dossier v04 par suffixe (le prefixe ver_XX_YYYY differe selon le perso) ----
local function find_dst_all()
    if DST_DONE then return end
    local sc = scene()
    if not sc then return end
    local suffixes = {}
    for _, fid in ipairs(FIGHTERS) do
        if not DST[fid] then
            suffixes[#suffixes + 1] = { fid = fid, sfx = string.format("fighter%03d/esf%03dv%02d", fid, fid, BASE_COS) }
        end
    end
    if #suffixes == 0 then DST_DONE = true return end
    local function walk(f, depth)
        if not f or #suffixes == 0 or depth > 8 then return end
        local okp, p = pcall(f.call, f, "get_Path")
        if okp and p then
            local s = tostring(p)
            for i = #suffixes, 1, -1 do
                local e = suffixes[i]
                if s:sub(-#e.sfx) == e.sfx then
                    DST[e.fid] = s
                    ev("[F" .. e.fid .. "] DST = " .. s)
                    table.remove(suffixes, i)
                end
            end
        end
        local okc, c = pcall(f.call, f, "get_Child")
        if okc and c then walk(c, depth + 1) end
        local okn, n = pcall(f.call, f, "get_Next")
        if okn and n then walk(n, depth) end
    end
    walk(sc:call("get_FirstFolder"), 0)
    DST_DONE = (#suffixes == 0)
end

-- ---- BASE_COLORS par perso : lire dans la table master les couleurs de DriveTech au lieu de coder en dur ----
local function read_base_colors()
    if BC_DONE then return end
    local tm = sdk.get_managed_singleton("app.TableDataManager")
    local im = sdk.get_managed_singleton("app.InventoryManager")
    local ci = im and im:get_field("_Costume")
    if not (tm and ci) then return end
    -- trouver le record DriveTech de chaque perso (costumeNo == BASE_COS) puis ses couleurs
    for rid = 1, 250 do
        local ok, rec = pcall(ci.call, ci, "GetMasterDataRecord", rid)
        if ok and rec then
            local fid = rec:get_field("fighterId")
            local cn = rec:get_field("costumeNo")
            if cn == BASE_COS and FIGHTER_SET[fid] and not BASE_COLORS[fid] then
                local ok2, cls = pcall(tm.call, tm, "GetFighterCostumeColors", rid, false)
                if ok2 and cls then
                    BASE_COLORS[fid] = {}
                    local min_c = 999
                    for j = 0, cls:call("get_Count") - 1 do
                        local cr = cls:call("get_Item", j)
                        local c = cr:get_field("colorNo")
                        BASE_COLORS[fid][c] = true
                        if c < min_c then min_c = c end
                    end
                    BASE_COLOR[fid] = min_c < 999 and min_c or 0
                    ev("[F" .. fid .. "] BASE_COLORS lu (" .. cls:call("get_Count") .. " couleurs, base=" .. BASE_COLOR[fid] .. ")")
                end
            end
        end
    end
    local all = true
    for _, fid in ipairs(FIGHTERS) do if not BASE_COLORS[fid] then all = false end end
    BC_DONE = all
end

-- ---- montage du dossier source + echange de manifeste ----
local holder_t = sdk.typeof("app.battle.assets.FighterVisualHolder")
local function holder_folder(c) return c:call("get_GameObject"):call("get_FolderSelf"):call("get_Path") end
local function dst_holder_present(f)
    if not DST[f] then return false end
    local sc = scene()
    if not sc then return false end
    local comps = sc:call("findComponents(System.Type)", holder_t)
    for i = 0, comps:call("get_Count") - 1 do
        local okp, p = pcall(holder_folder, comps:call("get_Item", i))
        if okp and p and tostring(p) == DST[f] then return true end
    end
    return false
end
local function keep_mounted(f)
    -- REGLE (22/09) : ne JAMAIS pre-monter le dossier du slot. Le jeu saute le montage de DriveTech si un dossier du
    -- meme perso est deja actif (chargement bloque, ecran noir). On monte le slot seulement une fois que le jeu a
    -- monte v<BASE> lui-meme (holder present), puis on echange le manifeste.
    local path = SRC(f)
    if not path then return end
    local ff = F[f]
    if not ff.src_folder then ff.src_folder = find_folder(path) end
    if not ff.src_folder then return end
    local oka, a = pcall(ff.src_folder.call, ff.src_folder, "get_Active")
    if oka and not a and (frame - ff.last_mount_frame) > 120 and dst_holder_present(f) then
        ff.last_mount_frame = frame
        pcall(ff.src_folder.call, ff.src_folder, "activate")
        ev("[F" .. f .. "] montage de v" .. tostring(fstate(f).slot) .. " (DriveTech monte par le jeu)")
    end
end

-- Colors = List<CostumeColorData{ColorId, Data}> ; on echange les Data des entrees ColorId == a et ColorId == b
local function ccvd_entries(ccvd, a, b)
    local list = ccvd and ccvd:get_field("Colors")
    if not list then return nil end
    local ea, eb
    for i = 0, list:call("get_Count") - 1 do
        local it = list:call("get_Item", i)
        local id = it and it:get_field("ColorId")
        if id == a then ea = it elseif id == b then eb = it end
    end
    return ea, eb
end
local function ccvd_swap(ccvd, a, b)
    local ea, eb = ccvd_entries(ccvd, a, b)
    if not (ea and eb) then return false end
    local da, db = ea:get_field("Data"), eb:get_field("Data")
    if not (da and db) then return false end
    ea:set_field("Data", db)
    eb:set_field("Data", da)
    return true
end
local function apply_color_alias(f, d_set)
    local fs = fstate(f)
    local want = fs.color
    local bc = BASE_COLORS[f]
    local ff = F[f]
    if not want or (bc and bc[want]) or ff.color_swap then return end
    local base_c = BASE_COLOR[f] or 0
    local ccvd = d_set:get_field("costumeColorVariation")
    if not ccvd then ev("[F" .. f .. "] couleur : pas de costumeColorVariation dans le manifeste") return end
    if ccvd_swap(ccvd, base_c, want) then
        ff.color_swap = { addr = ccvd:get_address(), a = base_c, b = want }
        ev("[F" .. f .. "] couleur " .. tostring(want) .. " placee sous la couleur " .. base_c .. " (CCVD " .. string.format("%x", ff.color_swap.addr) .. ")")
    else
        ev("[F" .. f .. "] couleur : entrees " .. base_c .. "/" .. tostring(want) .. " introuvables dans le CCVD")
    end
end
local function restore_color_alias(f, src_h)
    -- remise en place uniquement a travers un objet vivant re-lu (le CCVD du dossier source), jamais un pointeur garde
    local ff = F[f]
    if not ff.color_swap then return end
    local s_set = src_h and src_h:get_field("_Setting")
    local ccvd = s_set and s_set:get_field("costumeColorVariation")
    if ccvd and ccvd:get_address() == ff.color_swap.addr then
        ev("[F" .. f .. "] couleur : remise en place = " .. tostring(ccvd_swap(ccvd, ff.color_swap.a, ff.color_swap.b)))
    else
        ev("[F" .. f .. "] couleur : CCVD source different/absent, pas de remise en place")
    end
    ff.color_swap = nil
end
-- ---- delegation au plugin natif (SF6_CostumeSlotsNative.dll) ----
local native_check_time = 0
local native_active = false
local native_logged = false
local function native_plugin_active()
    if os.time() - native_check_time < 1 then return native_active end
    native_check_time = os.time()
    local data = json.load_file("SF6_CostumeSlots_data/native_present.json")
    local now_active = data ~= nil and data.ts ~= nil and os.time() - data.ts < 10
    if now_active and not native_logged then
        ev("alias delegue au plugin natif")
        native_logged = true
    elseif not now_active and native_logged then
        ev("reprise de l'alias par le Lua (plugin natif absent)")
        native_logged = false
    end
    native_active = now_active
    return native_active
end

-- etat partage (declare AVANT les fonctions qui le lisent)
local was_open = false
local select_gate = false
local function swap_manifest(f)
    if not DST[f] then return end
    local sc = scene()
    if not sc then return end
    local comps = sc:call("findComponents(System.Type)", holder_t)
    local path = SRC(f)
    if not path then return end
    local src_h, dst_h
    for i = 0, comps:call("get_Count") - 1 do
        local c = comps:call("get_Item", i)
        local okp, p = pcall(holder_folder, c)
        if okp and p then
            local s = tostring(p)
            -- le premier holder v04 trouvee seulement (miroir : l'adversaire pourrait avoir le meme v04)
            if same_slot_path(s, path) then src_h = c elseif s == DST[f] and not dst_h then dst_h = c end
        end
    end
    local ff = F[f]
    -- diagnostic : journaliser tout changement de presence des deux holders (adresse), pour comprendre les remontages
    local sig = (src_h and string.format("src=%x", src_h:get_address()) or "src=-") .. " " .. (dst_h and string.format("dst=%x", dst_h:get_address()) or "dst=-")
    if ff.last_sig ~= sig then
        local oka, a = pcall(ff.src_folder and ff.src_folder.call, ff.src_folder, "get_Active")
        ev("[F" .. f .. "] holders " .. sig .. " (n=" .. comps:call("get_Count") .. ", v" .. tostring(fstate(f).slot) .. " actif=" .. tostring(oka and a) .. ", menu=" .. tostring(was_open) .. ", select=" .. tostring(select_gate) .. ")")
        ff.last_sig = sig
    end
    if dst_h and src_h then
        local addr = dst_h:get_address()
        if ff.swapped_addr ~= addr then
            local s_set, d_set = src_h:get_field("_Setting"), dst_h:get_field("_Setting")
            if s_set and d_set then
                local ok = pcall(d_set.call, d_set, "overwrite", s_set)
                ev("[F" .. f .. "] manifeste v" .. tostring(fstate(f).slot) .. " -> v" .. BASE_COS .. " = " .. tostring(ok))
                ff.swapped_addr = addr
                if ok then pcall(apply_color_alias, f, d_set) end
            end
        end
    elseif not dst_h then
        if ff.swapped_addr and ff.color_swap then pcall(restore_color_alias, f, src_h) end
        ff.swapped_addr = nil
    end
end

-- ---- possession des slots : AddDlcSaveData (type DLC, re-evaluee a chaque lancement) pour TOUS les persos ----
-- la possession DLC est effacee par le jeu au passage du titre/login -> re-verification periodique
local own_done, own_logged, own_err_logged = false, false, false
-- Les records statiques du loader occupent les ids STATIC_ID_MIN..STATIC_ID_MAX (5000 + index_perso*100 + slot-5).
-- La possession DLC est PERSISTEE dans la sauvegarde (constate 23/09 : trois slots fantomes de Ken apres retrait du
-- mod = personnage vide) -> a chaque passage on REVOQUE tout record statique possede qui n'est plus dans le registre.
local STATIC_ID_MIN, STATIC_ID_MAX = 5000, 8199
-- Revocation : au lieu de boucler sur 3200 ids toutes les 120 frames,
-- on fait la boucle complete seulement au boot et quand registry.json change (taille).
local revoke_cursor = 5000
local REGISTRY_PATH = "SF6_Costumes_Data/registry.json"
local function registry_file_size()
    local fh = io.open(REGISTRY_PATH, "rb")  -- relatif a reframework/data/ (sandbox)
    if not fh then return 0 end
    local sz = fh:seek("end")
    fh:close()
    return sz or 0
end
local function insert_ownership()
    local ci = sdk.get_managed_singleton("app.InventoryManager"):get_field("_Costume")
    if not ci then return end
    -- Possession : toujours ajouter les records du registre (rapide, quelques appels)
    local wanted = {}
    for _, e in ipairs(POSSESSION) do wanted[e.record_id] = true end
    local n = 0
    if frame % 120 == 0 or not own_logged then for _, e in ipairs(POSSESSION) do
        local oke, has = pcall(ci.call, ci, "ExistsSaveData", e.record_id)
        if oke and not has then
            local oka = pcall(ci.call, ci, "AddDlcSaveData", e.record_id)
            if oka then n = n + 1 end
        end
    end end
    -- Revocation INCREMENTALE et continue : la sauvegarde n'est chargee qu'apres l'ecran titre (une boucle au boot ne
    -- voit rien), et une boucle complete (3 200 ExistsSaveData) d'un coup ferait un spike -> 100 ids par appel,
    -- reprise au debut apres chaque passe. Un slot fantome disparait donc en ~32 appels.
    local revoked = 0
    local stop = math.min(revoke_cursor + 99, STATIC_ID_MAX)
    for id = revoke_cursor, stop do
        if not wanted[id] then
            local oke, has = pcall(ci.call, ci, "ExistsSaveData", id)
            if oke and has then
                local okd = pcall(ci.call, ci, "DeleteDlcSaveData", id)
                if okd then revoked = revoked + 1 end
            end
        end
    end
    revoke_cursor = (stop >= STATIC_ID_MAX) and STATIC_ID_MIN or (stop + 1)
    if n > 0 or revoked > 0 or not own_logged then ev("possession costumes ajoutee=" .. n .. " revoquee=" .. revoked .. " (registre : " .. #POSSESSION .. " slots, " .. #FIGHTERS .. " persos)") own_logged = true end
end

-- records de couleur pour les slots du registre : 10 couleurs par slot, ids 9000 + index_global*10 + colorNo (hors plage statique 5000..8199)
-- (index_global = position dans la liste possession, 0-based ; jamais utilises par le jeu)
local function registry_color_records()
    local recs = {}
    for i, e in ipairs(POSSESSION) do
        for c = 0, 9 do
            local id = 9000 + (i - 1) * 10 + c
            recs[#recs + 1] = { id = id, ManageId = id, fighterCostumeId = e.record_id, colorNo = c, isDefault = true, isBundleCostume = false }
        end
    end
    return recs
end

-- ---- couleurs des slots : insertion a chaud dans TableDataManager.FighterCostumeColorUserDataDict (cle ManageId) ----
local colors_done = false
local COLOR_DICT = nil
local function insert_colors()
    local recs = (#POSSESSION > 0) and registry_color_records() or json.load_file("SF6_CostumeSlots_data/colors.json")
    if not recs then ev("colors.json absent") return end
    local tm = sdk.get_managed_singleton("app.TableDataManager")
    if not tm then return end
    local d
    for _, f in ipairs(tm:get_type_definition():get_fields() or {}) do if f:get_name():find("FighterCostumeColorUserDataDict") then d = f:get_data(tm) end end
    if not d then ev("dict couleurs introuvable") return end
    COLOR_DICT = d
    local sample = d:call("get_Item", 1)
    if not sample then ev("dict couleurs vide") return end
    -- modeles : records du jeu de meme colorNo (clone fidele ; un record cree par create_instance n'est PAS reconnu par IsValidItem)
    local models = {}
    local okm, lst = pcall(tm.call, tm, "GetFighterCostumeColors", 1, false)
    if okm and lst then for i = 0, lst:call("get_Count") - 1 do local m = lst:call("get_Item", i) models[m:get_field("colorNo")] = m end end
    local ci = sdk.get_managed_singleton("app.InventoryManager"):get_field("_Color")
    local n, skipped, owned = 0, 0, 0
    for _, r in ipairs(recs) do
        local okk, has = pcall(d.call, d, "ContainsKey", r.ManageId)
        if okk and has then skipped = skipped + 1 else
            local m = models[r.colorNo]
            local rec = m and m:call("MemberwiseClone")
            if rec then
                rec = rec:add_ref()
                rec:set_field("id", r.id) rec:set_field("ManageId", r.ManageId)
                rec:set_field("fighterCostumeId", r.fighterCostumeId) rec:set_field("isDefault", r.isDefault)
                rec:set_field("colorNo", r.colorNo) rec:set_field("isBundleCostume", r.isBundleCostume)
                local holder = sample:call("MemberwiseClone")
                if holder then
                    holder = holder:add_ref()
                    holder:set_field("RefCount", 1)
                    holder:set_field("RecordData", rec)
                    d:call("Add", r.ManageId, holder)
                    n = n + 1
                end
            end
        end
        -- possession : le spin ne montre que les couleurs presentes dans la sauvegarde (ColorList)
        local oke, e = pcall(ci.call, ci, "ExistsSaveData", r.ManageId)
        if oke and not e then local oka, a = pcall(ci.call, ci, "AddSaveData", r.ManageId) if oka and a then owned = owned + 1 end end
    end
    ev("couleurs inserees=" .. n .. " deja presentes=" .. skipped .. " possession ajoutee=" .. owned)
end

-- ---- menage : couleurs possedees sans record (slots retires, anciens ids 6000+) ----
-- Regle : un id present dans ColorList mais absent de FighterCostumeColorUserDataDict n'existe plus
-- pour le jeu -> on rend la possession (DeleteDlcSaveData, seule suppression exposee par ColorInventory :
-- [get_SaveDataList AddSaveData ExistsSaveData AddDlcSaveData DeleteDlcSaveData]). Couvre les ids d'une
-- version precedente (6000+) et les slots dont le mod a ete desinstalle. Balayage incremental, 100 ids
-- toutes les 30 frames (~35 s le tour complet) : la sauvegarde n'est chargee qu'apres le titre.
-- Mesure 24/09 : le jeu refuse deja AddSaveData sur un id sans record et n'en garde aucun -> filet de securite.
local COLOR_SCAN_MIN, COLOR_SCAN_MAX = 6000, 12999
local color_scan_cursor = COLOR_SCAN_MIN
local color_del_name, color_del_done = nil, false
local color_menage_total = 0
local color_seen_owned, color_sweep_logged = 0, false
local function find_color_delete(ci)
    color_del_done = true
    local names = {}
    local td = ci:get_type_definition()
    while td do
        for _, m in ipairs(td:get_methods() or {}) do names[m:get_name()] = true end
        td = td:get_parent_type()
    end
    for _, n in ipairs({ "DeleteSaveData", "RemoveSaveData", "DeleteDlcSaveData" }) do
        if names[n] then return n end
    end
    return nil
end
local function cleanup_colors()
    if not COLOR_DICT then return end
    local im = sdk.get_managed_singleton("app.InventoryManager")
    local ci = im and im:get_field("_Color")
    if not ci then return end
    if not color_del_done then
        color_del_name = find_color_delete(ci)
        ev("menage couleurs : methode " .. tostring(color_del_name))
    end
    if not color_del_name then return end
    local revoked = 0
    local stop = math.min(color_scan_cursor + 99, COLOR_SCAN_MAX)
    for id = color_scan_cursor, stop do
        local okk, has_rec = pcall(COLOR_DICT.call, COLOR_DICT, "ContainsKey", id)
        local oke, owned = pcall(ci.call, ci, "ExistsSaveData", id)
        if oke and owned then color_seen_owned = color_seen_owned + 1 end
        if okk and not has_rec and oke and owned then
            local okd = pcall(ci.call, ci, color_del_name, id)
            if okd then revoked = revoked + 1 end
        end
    end
    color_menage_total = color_menage_total + revoked
    if stop >= COLOR_SCAN_MAX and not color_sweep_logged then
        color_sweep_logged = true
        ev("menage couleurs : balayage " .. COLOR_SCAN_MIN .. "-" .. COLOR_SCAN_MAX ..
           " termine, " .. color_seen_owned .. " possedees, " .. color_menage_total .. " orphelines rendues")
    end
    color_scan_cursor = (stop >= COLOR_SCAN_MAX) and COLOR_SCAN_MIN or (stop + 1)
    if revoked > 0 then
        ev("menage couleurs : " .. revoked .. " possessions orphelines rendues (total " .. color_menage_total .. ")")
    end
end

-- ---- pastilles / liste de couleurs de l'ecran de selection : pour TOUS les persos du registre ----
-- Sans entree SelectFighterUIData pour le slot, hGUI.GetFighterCostumeColorData rend nil et
-- Param.GetEnableColorList plante -> spin bloque. Clone de l'entree du costume de base (Outfit 1).
local ui_done = false
local function insert_select_ui()
    local gm = sdk.get_managed_singleton("app.GuiManager")
    local mgr = gm and gm:get_field("<SelectFighterUIData>k__BackingField")
    if not mgr then return false end
    local ml = mgr:get_field("ManagedList")
    if not ml or ml:call("get_Count") == 0 then return false end
    local n = 0
    for _, fid in ipairs(FIGHTERS) do
        local lst = mgr:call("GetData_Fighter", fid)
        if lst then
            local cd = lst:get_field("CosDatas")
            local base = mgr:call("GetData_Costume", fid, 0)
            if base and cd then
                for slot, _ in pairs(SLOTS[fid]) do
                    if not mgr:call("GetData_Costume", fid, slot) then
                        local clone = base:call("MemberwiseClone")
                        if clone then
                            clone = clone:add_ref()
                            clone:set_field("CostumeId", slot)
                            cd:call("Add", clone)
                            n = n + 1
                        end
                    end
                end
            end
        end
    end
    ev("SelectFighterUIData inserees=" .. n .. " (" .. #FIGHTERS .. " persos)")
    return true
end

-- ---- boucle ----
local function tick()
    frame = frame + 1
    -- init one-shot : DST et BASE_COLORS avant le premier montage
    if not DST_DONE and frame > 60 and frame % 30 == 0 then pcall(find_dst_all) end
    if not BC_DONE and frame > 80 and frame % 30 == 0 then pcall(read_base_colors) end
    -- la possession DLC est effacee par le jeu au passage du titre/login (vu 22/09) -> re-verification periodique
    if frame > 100 and (not own_done or frame % 2 == 0) then own_done = true local oko, erro = pcall(insert_ownership) if not oko and not own_err_logged then own_err_logged = true ev("possession : ERREUR " .. tostring(erro)) end end
    if not colors_done and frame > 120 then colors_done = true pcall(insert_colors) end
    if colors_done and frame % 30 == 0 then pcall(cleanup_colors) end
    if not ui_done and frame > 120 and frame % 60 == 0 then local ok, done = pcall(insert_select_ui) if ok and done then ui_done = true end end
    if frame % 6 == 0 then select_gate = select_screen_active() end
    local entries = save_entries()
    if entries then
        if frame % 6 == 0 then
            local open = menu_open()
            if open and not was_open then
                for fid, e in pairs(entries) do
                    local fs = fstate(fid)
                    ev("[F" .. fid .. "] menu Battle Settings ouvert (sauvegarde = " .. tostring(get_cos(e)) .. ", slot = " .. tostring(fs.slot) .. ")")
                    if fs.slot and get_cos(e) == BASE_COS then set_cos(e, fs.slot, fid, "pour que le menu affiche le slot") end
                    if fs.slot and fs.color and get_col(e) ~= fs.color then set_col(e, fs.color, fid, "pour que le menu affiche la couleur") end
                end
            elseif was_open and not open then
                for fid, e in pairs(entries) do
                    local v = get_cos(e)
                    ev("[F" .. fid .. "] menu Battle Settings ferme (sauvegarde = " .. tostring(v) .. ")")
                    local slots_f = SLOTS[fid]
                    set_slot(fid, (slots_f and slots_f[v]) and v or false, "choix fait dans le menu")
                    if slots_f and slots_f[v] then
                        set_cos(e, BASE_COS, fid, "ce que le jeu et le reseau doivent voir")
                        local c = get_col(e)
                        set_color_intent(fid, c, "choix fait dans le menu")
                        local bc = BASE_COLORS[fid]
                        if bc and not bc[c] then set_col(e, BASE_COLOR[fid] or 0, fid, "couleur que DriveTech possede") end
                    end
                end
            end
            was_open = open
        end
        -- filet de securite : hors du menu, le numero du slot ne doit JAMAIS rester dans la sauvegarde
        for fid, e in pairs(entries) do
            local v = get_cos(e)
            local slots_f = SLOTS[fid]
            if not was_open and slots_f and slots_f[v] then
                set_slot(fid, v, "slot trouve dans la sauvegarde hors menu")
                set_cos(e, BASE_COS, fid, "filet de securite")
            end
            if not was_open and fstate(fid).slot then
                local bc = BASE_COLORS[fid]
                if bc then
                    local col = get_col(e)
                    if not bc[col] then
                        set_color_intent(fid, col, "couleur trouvee dans la sauvegarde hors menu")
                        set_col(e, BASE_COLOR[fid] or 0, fid, "filet de securite")
                    end
                end
            end
        end
    end
    -- montage + echange de manifeste pour chaque perso avec intention active
    if not select_gate and not native_plugin_active() then
        for _, fid in ipairs(FIGHTERS) do
            if fstate(fid).slot then
                keep_mounted(fid)
                if frame % 3 == 0 then swap_manifest(fid) end
            end
        end
    end
    do
        local t = os.time()
        if frame % 600 == 0 then
            local parts = {}
            for _, fid in ipairs(FIGHTERS) do
                local fs = fstate(fid)
                parts[#parts + 1] = "F" .. fid .. "=" .. tostring(fs.slot)
            end
            log.heartbeat = os.date("%H:%M:%S") .. " f" .. frame .. " " .. table.concat(parts, ",") .. " entries=" .. tostring(entries ~= nil) dirty = true
        end
        if dirty and t ~= last_write then last_write = t dirty = false json.dump_file("SF6_CostumeSlots_data/log.json", log) end
    end
end

re.on_pre_application_entry("LateUpdateBehavior", function()
    local ok, err = pcall(tick)
    if not ok then
        if not log.error then log.error = tostring(err) log.error_frame = frame json.dump_file("SF6_CostumeSlots_data/log.json", log) end
    elseif frame == 1 then
        log.first_tick_ok = true
        json.dump_file("SF6_CostumeSlots_data/log.json", log)
    end
end)
do
    local parts = {}
    for _, fid in ipairs(FIGHTERS) do
        local fs = fstate(fid)
        parts[#parts + 1] = "F" .. fid .. "=" .. tostring(fs.slot)
    end
    ev("alias complet arme (" .. #FIGHTERS .. " persos, " .. #POSSESSION .. " slots : " .. table.concat(parts, ", ") .. ")")
end
json.dump_file("SF6_CostumeSlots_data/log.json", log)
