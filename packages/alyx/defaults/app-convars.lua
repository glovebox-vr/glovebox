-- app-convars.lua: the convars file bundled in the app (tools/alyx-test/build-tf.sh). It sets NOTHING: the demo's
-- render settings reach the game as defaults through the files the app writes when missing (hlvr/cfg/deckard/video2.txt,
-- the game's own saved video settings, and the fixed fidelity table), so whatever a player then changes in Options is the
-- game's to keep. On a fresh install those give exactly FIX-demo-convars.lua's values at spawn (Mac, a1_intro_world,
-- 2026-10-03), which test scripts still push to force them. This reads them back for a pulled log: KLCV, KLTAB,
-- KLSTREAM at each spawn, KLTHROW at each thing let go. The one thing it changes is KLGLASS at the end: a glass panel that
-- won't stay broken is removed.
ListenToGameEvent("player_activate", function()
  local w = Entities:First()
  if w then w:SetThink(function()
    local out = {}
    for _, k in ipairs({ "r_texturefilteringquality", "r_texture_stream_max_resolution", "volume_fog_width", "vr_aoproxy_enable",
        "r_high_quality_shadows", "vr_fidelity_level", "vr_render_target_scale" }) do out[#out + 1] = k .. "=" .. tostring(Convars:GetStr(k)) end
    Msg("KLCV " .. table.concat(out, " ") .. "\n")
    local tab = {}
    for _, k in ipairs({ "vr_msaa", "r_ssao", "r_post_bloom", "r_high_quality_shadows", "sc_disable_spotlight_shadows",
        "vr_single_light_fast_path_sun_cascade_0_texels_per_world", "vr_single_light_fast_path_sun_cascade_1_texels_per_world",
        "vr_aoproxy_enable", "vr_character_level_placeholder", "cl_enable_eye_occlusion" }) do tab[#tab + 1] = k .. "=" .. tostring(Convars:GetStr(k)) end
    Msg("KLTAB " .. table.concat(tab, " ") .. "\n")
    return nil end, "klhi", 2.0) end
end, nil)

-- Print the texture-streaming convars' current values (defaults under our video settings) at the first spawn.
ListenToGameEvent("player_activate", function()
  local w = Entities:First(); if not w then return end
  w:SetThink(function()
    for _, k in ipairs({ "r_texture_stream_max_resolution", "r_texture_stream_mip_bias", "r_texture_stream_resolution_bias",
        "r_texture_stream_resolution_bias_decrease_rate", "r_texture_stream_resolution_bias_increase_rate",
        "r_texture_stream_resolution_bias_min", "r_texture_stream_resolution_bias_update_period",
        "r_texture_stream_throttle_amount", "r_texture_stream_throttle_count", "gpu_mem_level", "mem_level",
        "vr_throw_velocity_scale", "vr_handinfo_history_size", "vr_handinfo_history_ignore_size",
        "r_texture_pool_size", "r_texture_budget_dynamic", "r_texture_stream_throttle_count_over_budget" }) do
      Msg("KLSTREAM " .. k .. " = " .. tostring(Convars:GetStr(k)) .. "\n")
    end
    return nil end, "klstream", 2.0)
end, nil)

-- KLTHROW: how fast a thing really leaves the hand, for a pulled log. It changes nothing. At each item_released: where
-- everything loose near the player is, and again 0.1 s on; the fastest is what was thrown. Beside the headset log's
-- [throw] line, the hand's speed, it gives what the game made of a throw (DEMO.txt has how it works that out).
do
  local REACH, AFTER, SLOWEST = 200, 0.1, 40
  ListenToGameEvent("item_released", function(ev)
    pcall(function()
      local player, w = Entities:GetLocalPlayer(), Entities:First()
      if not player or not w then return end
      local seen, t0 = {}, Time()
      for _, e in ipairs(Entities:FindAllInSphere(player:GetCenter(), REACH)) do
        local cls = e:GetClassname()
        if (cls:find("^prop_physics") or cls:find("^item_")) and not e:GetMoveParent() then
          seen[#seen + 1] = { e = e, p = e:GetCenter(), cls = cls, idx = e:GetEntityIndex() }
        end
      end
      w:SetThink(function()
        pcall(function()
          local best, speed = nil, SLOWEST
          for _, s in ipairs(seen) do
            local ok, p = pcall(function() return s.e:GetCenter() end)  -- it may be gone by now
            if ok and p then
              local v = (p - s.p):Length() / math.max(Time() - t0, 0.001)
              if v > speed then best, speed = s, v end
            end
          end
          if best then
            Msg(string.format("KLTHROW %s let go: %s #%d leaves at %.0f units/s (%.1f m/s)\n", tostring(ev and ev.item),
                              best.cls, best.idx, speed, speed * 0.0254))
          end
        end)
        return nil
      end, "klthrow", AFTER)
    end)
  end, nil)
end

-- KLGLASS: breakable glass that won't stay broken. A player's Chapter 6 storeroom window (Arms Race), on the headset
-- (2026-11-07): drawn and solid; a shot leaves no hole (only the impact effect plays); a thrown chair goes straight
-- through it, and then the player can walk through it too, but only for 1-2 s: after that it is solid again, still
-- looking whole. So the game breaks it, but the break doesn't hold, and the way on is through it: a soft-lock. Glass
-- that works has a hole wherever an object went through it, from then on. So when a physics object goes through a
-- panel, this looks again SETTLE seconds later with a short trace through the panel at that spot: solid there again
-- means it is one of these, and the panel is removed, with any pieces still in its frame, after its OnBroken if the
-- game never fired it (the map's logic may wait on that). Where neither trace can run, a panel that no piece of glass
-- came off is taken as one. Logged in the game's console as KLGLASS lines: the map's panels, shard count and glass
-- settings at each spawn, each panel the first time the player is near it, each crossing and its verdict, and every
-- OnBroken, so a log also says what glass does on this device. Every call into the game is inside pcall: whatever
-- this finds, it can't stop the game.
KLGLASS = KLGLASS or {}
do
  local G = KLGLASS
  local TICK = 0.1       -- seconds between looks
  local NEAR = 400       -- panels within this many units (~10 m) of the player are watched
  local MARGIN = 4       -- units around a panel's outline that still count as through it
  local JUMP = 150       -- units in one look (1500 units/s, ~38 m/s, past any throw): a teleport, not a pass-through
  local SETTLE = 3.0     -- seconds from a crossing to its verdict (the storeroom window was solid again in 1-2 s)
  local SIDE = 8         -- units each side of the panel where the trace through it starts and ends
  local MASK = 33636363  -- MASK_PLAYERSOLID: what stops the player
  local REMOVE = 0.2     -- seconds from our OnBroken to removing the panel, so the output goes out first

  local function say(fmt, ...) Msg("KLGLASS " .. string.format(fmt, ...) .. "\n") end
  local function where(v) return string.format("(%.0f %.0f %.0f)", v.x, v.y, v.z) end
  local function convar(name)
    local ok, v = pcall(function() return Convars:GetStr(name) end)
    return ok and tostring(v) or "?"
  end
  local function count(cls, center, reach)
    local ok, list = pcall(function()
      if center then return Entities:FindAllByClassnameWithin(cls, center, reach) end
      return Entities:FindAllByClassname(cls)
    end)
    return ok and list and #list or 0
  end
  local function with(v, axis, value)
    local t = { x = v.x, y = v.y, z = v.z }
    t[axis] = value
    return Vector(t.x, t.y, t.z)
  end
  local function inside(p, mins, maxs)
    for _, k in ipairs({ "x", "y", "z" }) do
      if p[k] < mins[k] - MARGIN or p[k] > maxs[k] + MARGIN then return false end
    end
    return true
  end

  -- Where does p0 -> p1 (points in the panel's own space) cross the panel's mid-plane, inside its outline? The
  -- panel's thin axis is its normal. The point (panel space) and that axis, or nil.
  function G.crossing(p0, p1, mins, maxs)
    local ex, ey, ez = maxs.x - mins.x, maxs.y - mins.y, maxs.z - mins.z
    local a, b, c = "x", "y", "z"
    if ey <= ex and ey <= ez then a, b, c = "y", "x", "z" elseif ez <= ex and ez <= ey then a, b, c = "z", "x", "y" end
    local mid = (mins[a] + maxs[a]) * 0.5
    local d0, d1 = p0[a] - mid, p1[a] - mid
    if d0 == d1 or (d0 > 0) == (d1 > 0) then return nil end
    local t = d0 / (d0 - d1)
    local hit = { [a] = mid }
    for _, k in ipairs({ b, c }) do
      local v = p0[k] + (p1[k] - p0[k]) * t
      if v < mins[k] - MARGIN or v > maxs[k] + MARGIN then return nil end
      hit[k] = v
    end
    return Vector(hit.x, hit.y, hit.z), a
  end

  local function panel(glass)
    local idx = glass:GetEntityIndex()
    local st = G.panels[idx]
    if st then return st end
    local mins, maxs = glass:GetBoundingMins(), glass:GetBoundingMaxs()
    st = { idx = idx, stage = 0, at = 0, broken = false, reach = (maxs - mins):Length() * 0.5 + 48 }
    G.panels[idx] = st
    say("%s #%d '%s' at %s, %.0f x %.0f x %.0f units, %d shards near it", GetMapName(), idx, glass:GetName(),
        where(glass:GetCenter()), maxs.x - mins.x, maxs.y - mins.y, maxs.z - mins.z,
        count("shatterglass_shard", glass:GetCenter(), st.reach))
    local ok, err = pcall(function()
      local scope = glass:GetOrCreatePrivateScriptScope()
      scope.KLGlassBroken = function()
        if not st.broken then say("#%d broke (OnBroken)", idx); st.brokeAt = Time() end
        st.broken = true
      end
      glass:RedirectOutput("OnBroken", "KLGlassBroken", glass)
    end)
    if not ok then say("#%d: no OnBroken hook (%s)", idx, tostring(err)) end
    return st
  end

  -- A physics object (not one parented to something: a held hand, a wrist pocket's item) whose centre crosses the
  -- panel between two looks starts a verdict. The game breaks glass at the impact, before the look that sees the
  -- crossing: so the shards to compare with are the last look's, and an OnBroken under a second old is this one's.
  local function watch(glass, st, now)
    local mins, maxs = glass:GetBoundingMins(), glass:GetBoundingMaxs()
    local shards = count("shatterglass_shard", glass:GetCenter(), st.reach)
    local before = st.shardsPrev or shards
    st.shardsPrev = shards
    for _, e in ipairs(Entities:FindAllInSphere(glass:GetCenter(), st.reach)) do
      local cls = e:GetClassname()
      if (cls:find("^prop_physics") or cls:find("^item_")) and not e:GetMoveParent() then
        local key = st.idx .. ":" .. e:GetEntityIndex()
        local p = glass:TransformPointWorldToEntity(e:GetCenter())
        local prev = G.seen[key]
        G.seen[key] = { p = p, t = now }
        if st.stage == 0 and prev and now - prev.t <= 3 * TICK and (p - prev.p):Length() < JUMP then
          local c, axis = G.crossing(prev.p, p, mins, maxs)
          if c then
            st.what = string.format("%s #%d", cls, e:GetEntityIndex())
            st.brokenBefore = st.broken and st.brokeAt ~= nil and now - st.brokeAt > 1.0
            say("#%d: %s went through it at %.0f units/s%s", st.idx, st.what,
                (p - prev.p):Length() / math.max(now - prev.t, 0.001), st.brokenBefore and " (it had broken before)" or "")
            st.stage, st.at, st.point, st.axis = 1, now, c, axis
            st.shards0, st.fresh = before, math.max(shards - before, 0)
          end
        end
      end
    end
  end

  -- Is the panel solid where the object went through? A short trace through it there, along its thin axis, two
  -- ways: what stops the player (it counts when it hits the panel or a piece of it), and the panel's own collision.
  -- true or false (nil when neither trace could run), and what each one said.
  local function probe(glass, st, player)
    local mins, maxs = glass:GetBoundingMins(), glass:GetBoundingMaxs()
    local w0 = glass:TransformPointEntityToWorld(with(st.point, st.axis, mins[st.axis] - SIDE))
    local w1 = glass:TransformPointEntityToWorld(with(st.point, st.axis, maxs[st.axis] + SIDE))
    local byTrace, byPanel, notes = nil, nil, {}
    local ok, err = pcall(function()
      local t = { startpos = w0, endpos = w1, mask = MASK, ignore = player }
      TraceLine(t)
      local e = t.hit and t.enthit or nil
      if e then
        local cls = e:GetClassname()
        byTrace = e:GetEntityIndex() == st.idx or cls == "shatterglass_shard"
        notes[#notes + 1] = string.format("the trace hit %s #%d", cls, e:GetEntityIndex())
      else
        byTrace = false
        notes[#notes + 1] = t.hit and "the trace hit the world" or "the trace hit nothing"
      end
    end)
    if not ok then notes[#notes + 1] = "no trace (" .. tostring(err) .. ")" end
    ok, err = pcall(function()
      local t = { start = w0, ["end"] = w1, startpos = w0, endpos = w1, ent = glass }
      TraceCollideable(t)
      byPanel = (t.hit or t.startsolid or (t.fraction ~= nil and t.fraction < 1)) and true or false
      notes[#notes + 1] = byPanel and "its own collision is there" or "its own collision is not"
    end)
    if not ok then notes[#notes + 1] = "no collision trace (" .. tostring(err) .. ")" end
    local solid = nil
    if byTrace or byPanel then solid = true elseif byTrace == false or byPanel == false then solid = false end
    return solid, table.concat(notes, "; ")
  end

  local function condemn(glass, st, player, now, why)
    say("#%d: %s: removing it", st.idx, why)
    if not st.broken then
      say("#%d: firing its OnBroken first (the game never did)", st.idx)
      local ok, err = pcall(function() glass:FireOutput("OnBroken", player, player, nil, 0) end)
      if not ok then say("#%d: FireOutput failed (%s)", st.idx, tostring(err)) end
    end
    st.stage, st.at = 2, now
  end

  local function decide(glass, st, player, now)
    local solid, how = probe(glass, st, player)
    local after, fresh = now - st.at, math.max(st.fresh, 0)
    if solid then
      condemn(glass, st, player, now, string.format("%.1f s after %s went through it, it is solid there again (%s)",
              after, st.what, how))
    elseif solid == nil and fresh == 0 and not st.brokenBefore then
      condemn(glass, st, player, now, string.format("no piece of glass came off it in the %.1f s after %s went through (%s)",
              after, st.what, how))
    elseif solid == nil then
      say("#%d: %d new pieces came off it after %s went through, so it broke (%s)", st.idx, fresh, st.what, how)
      st.stage, st.shardsPrev = 0, nil
    else
      say("#%d: it broke where %s went through: open there %.1f s on, %d new pieces (%s)", st.idx, st.what, after,
          fresh, how)
      st.stage, st.shardsPrev = 0, nil
    end
  end

  -- The panel goes, and so do pieces still in its frame (one of them may be what is solid there); pieces that fell stay.
  local function remove(glass, st)
    local mins, maxs = glass:GetBoundingMins(), glass:GetBoundingMaxs()
    local n = 0
    for _, s in ipairs(Entities:FindAllByClassnameWithin("shatterglass_shard", glass:GetCenter(), st.reach)) do
      if inside(glass:TransformPointWorldToEntity(s:GetCenter()), mins, maxs) then s:Kill(); n = n + 1 end
    end
    glass:Kill()
    st.stage = 3
    say("#%d: removed, with %d pieces that were still in its frame", st.idx, n)
  end

  function G.look(now)
    local player = Entities:GetLocalPlayer()
    if not player then return end
    for _, glass in ipairs(Entities:FindAllByClassnameWithin("func_shatterglass", player:GetCenter(), NEAR)) do
      local st = panel(glass)
      if st.stage == 0 then
        watch(glass, st, now)
      elseif st.stage == 1 then
        st.fresh = math.max(st.fresh, count("shatterglass_shard", glass:GetCenter(), st.reach) - st.shards0)
        if now - st.at >= SETTLE - TICK / 2 then decide(glass, st, player, now) end
      elseif st.stage == 2 and now - st.at >= REMOVE then
        remove(glass, st)
      end
    end
  end

  ListenToGameEvent("player_activate", function()
    G.panels, G.seen, G.erred = {}, {}, false
    local w = Entities:First()
    if not w then return end
    w:SetThink(function()
      say("%s: %d func_shatterglass panels, %d shards; shatterglass_cleanup %s, _cleanup_max %s, _shard_lifetime %s, "
          .. "cl_async_client_shatter %s, net_usesocketsforloopback %s", GetMapName(),
          count("func_shatterglass"), count("shatterglass_shard"), convar("shatterglass_cleanup"),
          convar("shatterglass_cleanup_max"), convar("shatterglass_shard_lifetime"), convar("cl_async_client_shatter"),
          convar("net_usesocketsforloopback"))
      return nil
    end, "klglass_census", 1.0)
    w:SetThink(function()
      local ok, err = pcall(G.look, Time())
      if not ok and not G.erred then G.erred = true; say("error, looking on: %s", tostring(err)) end
      return TICK
    end, "klglass", 1.0)
  end, nil)
end
