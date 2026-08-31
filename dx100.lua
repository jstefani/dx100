-- dx100
-- 4-op FM, DX100 style
--
-- E1 algorithm  E2 feedback
-- E3 op level
-- K2 select op  K3 rnd voice
-- K1+E2 ratio  K1+E3 op wave
-- K1+K3 panic
-- midi / grid
-- PARAMS for full edit

engine.name = "DX100"

local MusicUtil = require "musicutil"
local presets = include("dx100/lib/presets")

local g = grid.connect()
local a = arc.connect()

local ALGOS = 8
local OPS = 4
local WAVES = { "sin", "half", "abs", "quart", "alt", "alt/2", "sq-sin", "saw" }
local LFO_WAVES = { "tri", "sin", "sqr", "s&h" }
local RATIOS = {
  0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5,
  6, 6.5, 7, 7.5, 8, 9, 10, 11, 12, 13, 14, 15, 16
}

-- carriers per algorithm (which ops reach the output)
local CARRIERS = {
  { 1 }, { 1 }, { 1 }, { 1 }, { 1, 2 }, { 1, 2, 3 }, { 1, 2, 3 }, { 1, 2, 3, 4 }
}
-- modulation edges per algorithm: { from, to }
local EDGES = {
  { { 4, 3 }, { 3, 2 }, { 2, 1 } },
  { { 4, 2 }, { 3, 2 }, { 2, 1 } },
  { { 3, 2 }, { 2, 1 }, { 4, 1 } },
  { { 4, 3 }, { 3, 1 }, { 2, 1 } },
  { { 4, 3 }, { 3, 1 }, { 4, 2 } },
  { { 4, 1 }, { 4, 2 }, { 4, 3 } },
  { { 4, 3 } },
  {},
}

local stack = {}
local grid_held = {}
local sustain = false
local sustained = {}
local last_note = 48
local cur_op = 1
local focus_t = 0
local hud = nil
local midi_devs = {}
local screen_dirty = true
local grid_dirty = true
local gfx
local ready = false
local FPS = 15

local function shifted()
  return _menu.alt == true
end

local function flash(name, val)
  if not ready then return end
  hud = { name = name, val = val }
  focus_t = util.time()
  screen_dirty = true
end

local function op_id(base, n)
  return base .. "_" .. n
end

local function is_poly()
  return params:get("voice_mode") == 2
end

local function root_note()
  return params:get("root") + ((params:get("kb_oct") + 1) * 12)
end

-- ---------- note handling ----------

local function send_note(note, vel, legato)
  local hz = MusicUtil.note_num_to_freq(note)
  vel = vel or 0.85
  if is_poly() then
    engine.note_on(note, hz, vel, 0, 1)
  else
    engine.note_on(0, hz, vel, legato and 1 or 0, legato and 0 or 1)
  end
  last_note = note
  screen_dirty = true
  grid_dirty = true
end

local function note_on(note, vel)
  note = util.clamp(math.floor(note + 0.5), 0, 127)
  local legato = (not is_poly()) and (#stack > 0)
  table.insert(stack, note)
  send_note(note, vel or 0.85, legato)
end

local function remove_stack(note)
  for i = #stack, 1, -1 do
    if stack[i] == note then table.remove(stack, i) end
  end
end

local function note_off(note)
  note = util.clamp(math.floor(note + 0.5), 0, 127)
  if sustain then
    sustained[note] = true
    return
  end
  remove_stack(note)
  if is_poly() then
    engine.note_off(note)
    grid_dirty = true
    screen_dirty = true
    return
  end
  if #stack > 0 then
    send_note(stack[#stack], 0.85, true)
  else
    engine.note_off(0)
    grid_dirty = true
    screen_dirty = true
  end
end

local function all_off()
  stack = {}
  sustained = {}
  grid_held = {}
  engine.note_off_all()
  grid_dirty = true
  screen_dirty = true
end

-- ---------- voice load / randomise ----------

local function load_preset(idx)
  local p = presets[idx]
  if p == nil then return end
  params:set("algo", p.algo)
  params:set("feedback", math.floor(p.fb * 99 + 0.5))
  params:set("dx_feedback", 0)
  params:set("transpose", p.transpose or 0)
  for i = 1, OPS do
    local o = p.ops[i]
    if o then
      -- snap ratio to the nearest entry in the ratio table
      local best, bd = 1, math.huge
      for ri, rv in ipairs(RATIOS) do
        local d = math.abs(rv - o.ratio)
        if d < bd then best, bd = ri, d end
      end
      params:set(op_id("ratio", i), best)
      params:set(op_id("det", i), o.det or 0)
      params:set(op_id("level", i), math.floor(o.level * 99 + 0.5))
      params:set(op_id("atk", i), math.floor(o.atk * 99 + 0.5))
      params:set(op_id("d1r", i), math.floor(o.d1r * 99 + 0.5))
      params:set(op_id("d1l", i), math.floor(o.d1l * 99 + 0.5))
      params:set(op_id("d2r", i), math.floor(o.d2r * 99 + 0.5))
      params:set(op_id("rel", i), math.floor(o.rel * 99 + 0.5))
      params:set(op_id("wave", i), o.wave or 1)
      params:set(op_id("ks", i), math.floor((o.ks or 0) * 99 + 0.5))
      params:set(op_id("vs", i), math.floor((o.vs or 0.4) * 99 + 0.5))
      params:set(op_id("fixed", i), 1)
    end
  end
  flash("VOICE", p.name)
end

local function rnd_voice()
  local algo = math.random(1, ALGOS)
  params:set("algo", algo)
  params:set("feedback", math.random(0, 85))
  params:set("dx_feedback", (math.random() < 0.35) and math.random(0, 80) or 0)
  for i = 1, OPS do
    local carrier = false
    for _, c in ipairs(CARRIERS[algo]) do
      if c == i then carrier = true end
    end
    -- carriers stay near simple ratios; modulators roam
    if carrier or math.random() < 0.5 then
      params:set(op_id("ratio", i), ({ 4, 4, 4, 8, 10, 12 })[math.random(1, 6)])
    else
      params:set(op_id("ratio", i), math.random(1, #RATIOS))
    end
    params:set(op_id("det", i), math.random(-7, 7))
    params:set(op_id("level", i), carrier and math.random(70, 99) or math.random(20, 90))
    params:set(op_id("atk", i), (math.random() < 0.7) and math.random(85, 99) or math.random(30, 80))
    params:set(op_id("d1r", i), math.random(25, 85))
    params:set(op_id("d1l", i), math.random(0, 90))
    params:set(op_id("d2r", i), math.random(5, 60))
    params:set(op_id("rel", i), math.random(35, 85))
    params:set(op_id("wave", i), (math.random() < 0.6) and 1 or math.random(1, #WAVES))
    params:set(op_id("ks", i), math.random(0, 40))
    params:set(op_id("vs", i), math.random(20, 80))
    params:set(op_id("fixed", i), (math.random() < 0.08) and 2 or 1)
  end
  flash("VOICE", "rnd")
end

-- ---------- drawing ----------

-- node positions for the algorithm diagram, per op, in a 4-slot stack
local function algo_layout(algo)
  -- returns { [op] = {col, row} }; row 1 = bottom (carrier), higher = modulator
  local L = {
    { [1] = { 1, 1 }, [2] = { 1, 2 }, [3] = { 1, 3 }, [4] = { 1, 4 } },
    { [1] = { 1, 1 }, [2] = { 1, 2 }, [3] = { 1, 3 }, [4] = { 2, 3 } },
    { [1] = { 1, 1 }, [2] = { 1, 2 }, [3] = { 1, 3 }, [4] = { 2, 2 } },
    { [1] = { 1, 1 }, [2] = { 2, 2 }, [3] = { 1, 2 }, [4] = { 1, 3 } },
    { [1] = { 1, 1 }, [2] = { 2, 1 }, [3] = { 1, 2 }, [4] = { 1, 3 } },
    { [1] = { 1, 1 }, [2] = { 2, 1 }, [3] = { 3, 1 }, [4] = { 2, 2 } },
    { [1] = { 1, 1 }, [2] = { 2, 1 }, [3] = { 3, 1 }, [4] = { 3, 2 } },
    { [1] = { 1, 1 }, [2] = { 2, 1 }, [3] = { 3, 1 }, [4] = { 4, 1 } },
  }
  return L[algo]
end

local function draw_algo(ox, oy)
  local algo = params:get("algo")
  local lay = algo_layout(algo)
  local cw, ch = 11, 10
  local function nx(c) return ox + (c - 1) * cw end
  local function ny(r) return oy + (4 - r) * ch end

  -- edges first so boxes sit on top
  screen.level(3)
  for _, e in ipairs(EDGES[algo]) do
    local from, to = lay[e[1]], lay[e[2]]
    local x1, y1 = nx(from[1]) + 4, ny(from[2]) + 8
    local x2, y2 = nx(to[1]) + 4, ny(to[2])
    screen.move(x1, y1)
    if from[1] == to[1] then
      screen.line(x2, y2)
    else
      screen.line(x1, y1 + 2)
      screen.line(x2, y1 + 2)
      screen.line(x2, y2)
    end
    screen.stroke()
  end

  -- feedback loop marker on op4 (either flavor)
  local fb_show = math.max(params:get("feedback"), params:get("dx_feedback"))
  if fb_show > 0 then
    local f = lay[4]
    local fx, fy = nx(f[1]), ny(f[2])
    screen.level(util.round(util.linlin(0, 99, 2, 12, fb_show)))
    screen.move(fx + 8, fy + 1)
    screen.line(fx + 11, fy + 1)
    screen.line(fx + 11, fy + 7)
    screen.line(fx + 8, fy + 7)
    screen.stroke()
  end

  for i = 1, OPS do
    local p = lay[i]
    local x, y = nx(p[1]), ny(p[2])
    local lvl = params:get(op_id("level", i)) / 99
    local carrier = false
    for _, c in ipairs(CARRIERS[algo]) do
      if c == i then carrier = true end
    end
    if carrier then
      -- carriers filled proportional to level
      screen.level(2)
      screen.rect(x + 0.5, y + 0.5, 8, 8)
      screen.stroke()
      local h = math.floor(lvl * 6 + 0.5)
      if h > 0 then
        screen.level(i == cur_op and 15 or 9)
        screen.rect(x + 2, y + 7 - h, 5, h)
        screen.fill()
      end
    else
      screen.level(i == cur_op and 15 or util.round(util.linlin(0, 1, 3, 10, lvl)))
      screen.rect(x + 0.5, y + 0.5, 8, 8)
      screen.stroke()
    end
    if i == cur_op then
      screen.level(15)
      screen.rect(x - 1.5, y - 1.5, 12, 12)
      screen.stroke()
    end
  end
end

-- draw the DX rate/level envelope of the selected operator
local function draw_env(ox, oy, w, h)
  local n = cur_op
  local atk = params:get(op_id("atk", n)) / 99
  local d1r = params:get(op_id("d1r", n)) / 99
  local d1l = params:get(op_id("d1l", n)) / 99
  local d2r = params:get(op_id("d2r", n)) / 99
  local rel = params:get(op_id("rel", n)) / 99

  -- rates are "higher = faster", so segment widths invert them
  local wa = (1 - atk) * 0.30 + 0.03
  local wd = (1 - d1r) * 0.34 + 0.05
  local wh = 0.16
  local ws = (1 - d2r) * 0.22 + 0.03
  local wr = (1 - rel) * 0.30 + 0.05
  local tot = wa + wd + wh + ws + wr

  local function px(t) return ox + (t / tot) * w end
  local function py(v) return oy + h - (v * h) end

  -- d2 falls from d1l toward zero while held
  local d2end = d1l * (1 - d2r) * 0.85

  screen.level(2)
  screen.move(ox, oy + h + 0.5)
  screen.line(ox + w, oy + h + 0.5)
  screen.stroke()

  screen.level(12)
  screen.move(px(0), py(0))
  screen.line(px(wa), py(1))
  screen.line(px(wa + wd), py(d1l))
  screen.line(px(wa + wd + wh), py(d1l))
  screen.line(px(wa + wd + wh + ws), py(d2end))
  screen.line(px(tot), py(0))
  screen.stroke()
end

local function bar(x, y, w, h, amt, bright)
  screen.level(2)
  screen.rect(x + 0.5, y + 0.5, w, h)
  screen.stroke()
  local fill = math.floor(util.clamp(amt, 0, 1) * (w - 2))
  if fill > 0 then
    screen.level(bright or 10)
    screen.rect(x + 1, y + 1, fill, h - 2)
    screen.fill()
  end
end

function redraw()
  screen.clear()
  screen.aa(0)
  screen.line_width(1)
  screen.font_face(1)
  screen.font_size(8)

  -- header
  screen.level(10)
  screen.move(0, 7)
  screen.text("DX100")
  screen.level(5)
  screen.move(35, 7)
  screen.text("ALG " .. params:get("algo"))
  screen.move(70, 7)
  local fb_h = params:get("feedback")
  local dx_h = params:get("dx_feedback")
  if dx_h > 0 then
    screen.text("FB " .. fb_h .. "/" .. dx_h)
  else
    screen.text("FB " .. fb_h)
  end
  screen.level(is_poly() and 6 or 4)
  screen.move(127, 7)
  screen.text_right(is_poly() and ("poly " .. #stack) or "mono")

  draw_algo(2, 12)

  -- selected operator readout
  local n = cur_op
  screen.level(15)
  screen.move(52, 19)
  screen.text("OP" .. n)
  screen.level(4)
  screen.move(74, 19)
  if params:get(op_id("fixed", n)) == 2 then
    screen.text(string.format("%.0fhz", params:get(op_id("fhz", n))))
  else
    local r = RATIOS[params:get(op_id("ratio", n))]
    screen.text((r == math.floor(r)) and string.format("x%d", r)
      or string.format("x%.2f", r))
  end
  screen.level(4)
  screen.move(127, 19)
  screen.text_right(WAVES[params:get(op_id("wave", n))])

  screen.level(3)
  screen.move(52, 28)
  screen.text("LVL")
  bar(70, 22, 57, 6, params:get(op_id("level", n)) / 99, 12)

  draw_env(52, 32, 75, 18)

  -- footer
  if hud and (util.time() - focus_t) < 1.6 then
    screen.level(12)
    screen.move(0, 63)
    screen.text(hud.name)
    screen.level(10)
    screen.move(127, 63)
    screen.text_right(tostring(hud.val))
  else
    screen.level(8)
    screen.move(0, 63)
    screen.text(MusicUtil.note_num_to_name(last_note, true))
    screen.level(4)
    screen.move(34, 63)
    screen.text(presets[params:get("preset")].name)
    screen.level(4)
    screen.move(127, 63)
    screen.text_right(string.format("A%d D%d S%d R%d",
      params:get(op_id("atk", n)) // 10,
      params:get(op_id("d1r", n)) // 10,
      params:get(op_id("d1l", n)) // 10,
      params:get(op_id("rel", n)) // 10))
  end

  screen.update()
end

-- ---------- grid ----------

local function grid_note(x, y)
  local rows = (g.rows and g.rows > 0) and g.rows or 8
  return util.clamp(root_note() + ((rows - y) * 5) + (x - 1), 0, 127)
end

local function grid_redraw()
  g:all(0)
  local rows = (g.rows and g.rows > 0) and g.rows or 8
  local cols = (g.cols and g.cols > 0) and g.cols or 16
  local root = params:get("root")
  local current = stack[#stack]
  for y = 1, rows do
    for x = 1, cols do
      local n = grid_note(x, y)
      local lvl = 0
      if (n % 12) == root then lvl = 2 end
      if grid_held[n] then lvl = 8 end
      if current == n then lvl = 15 end
      g:led(x, y, lvl)
    end
  end
  g:refresh()
end

local function arc_redraw()
  if a.device == nil then return end
  local n = cur_op
  local vals = {
    params:get(op_id("level", n)) / 99,
    params:get("feedback") / 99,
    params:get(op_id("d1r", n)) / 99,
    params:get(op_id("rel", n)) / 99,
  }
  a:all(0)
  for i = 1, 4 do
    local v = math.floor(vals[i] * 64 + 0.5)
    for j = 1, v do a:led(i, j, 10) end
  end
  a:refresh()
end

local function midi_event(data)
  local msg = midi.to_msg(data)
  local ch = params:get("midi_ch")
  if ch > 0 and msg.ch ~= ch then return end
  if msg.type == "note_on" then
    if msg.vel == 0 then note_off(msg.note)
    else note_on(msg.note, msg.vel / 127) end
  elseif msg.type == "note_off" then
    note_off(msg.note)
  elseif msg.type == "cc" then
    if msg.cc == 64 then
      sustain = msg.val >= 64
      if not sustain then
        for nn, _ in pairs(sustained) do note_off(nn) end
        sustained = {}
      end
    elseif msg.cc == 1 then
      params:set("pms", math.floor(msg.val / 127 * 99))
    elseif msg.cc == 123 then
      all_off()
    end
  end
end

-- ---------- params ----------

local function add_op_params(n)
  local eng = {
    ratio = { "r", n }, det = { "d", n }, fhz = { "f", n }, fixed = { "x", n },
    wave = { "w", n }, level = { "l", n },
    atk = { "a", n }, d1r = { "b", n }, d1l = { "c", n },
    d2r = { "e", n }, rel = { "g", n }, ks = { "k", n }, vs = { "v", n },
  }
  local function cmd(key)
    return eng[key][1] .. eng[key][2]
  end

  params:add_group("op " .. n, 13)

  params:add_option(op_id("ratio", n), "ratio", (function()
    local t = {}
    for i, r in ipairs(RATIOS) do
      t[i] = (r == math.floor(r)) and string.format("%d", r) or string.format("%.2f", r)
    end
    return t
  end)(), 4)
  params:set_action(op_id("ratio", n), function(x)
    engine[cmd("ratio")](RATIOS[x])
    flash("OP" .. n .. " RATIO", RATIOS[x])
  end)

  params:add_number(op_id("det", n), "detune", -50, 50, 0)
  params:set_action(op_id("det", n), function(x)
    engine[cmd("det")](x)
    flash("OP" .. n .. " DET", x)
  end)

  params:add_option(op_id("fixed", n), "mode", { "ratio", "fixed" }, 1)
  params:set_action(op_id("fixed", n), function(x)
    engine[cmd("fixed")](x - 1)
    flash("OP" .. n, ({ "ratio", "fixed" })[x])
  end)

  params:add_control(op_id("fhz", n), "fixed hz",
    controlspec.new(1, 8000, "exp", 0, 100, "hz"))
  params:set_action(op_id("fhz", n), function(x)
    engine[cmd("fhz")](x)
    flash("OP" .. n .. " HZ", string.format("%.0f", x))
  end)

  params:add_option(op_id("wave", n), "wave", WAVES, 1)
  params:set_action(op_id("wave", n), function(x)
    engine[cmd("wave")](x - 1)
    flash("OP" .. n .. " WAVE", WAVES[x])
  end)

  params:add_number(op_id("level", n), "level", 0, 99,
    ({ 99, 75, 60, 50 })[n])
  params:set_action(op_id("level", n), function(x)
    engine[cmd("level")](x / 99)
    flash("OP" .. n .. " LVL", x)
  end)

  params:add_number(op_id("atk", n), "attack rate", 0, 99, 95)
  params:set_action(op_id("atk", n), function(x)
    engine[cmd("atk")](x / 99)
    flash("OP" .. n .. " AR", x)
  end)

  params:add_number(op_id("d1r", n), "decay 1 rate", 0, 99, 45)
  params:set_action(op_id("d1r", n), function(x)
    engine[cmd("d1r")](x / 99)
    flash("OP" .. n .. " D1R", x)
  end)

  params:add_number(op_id("d1l", n), "decay 1 level", 0, 99,
    ({ 80, 50, 40, 30 })[n])
  params:set_action(op_id("d1l", n), function(x)
    engine[cmd("d1l")](x / 99)
    flash("OP" .. n .. " D1L", x)
  end)

  params:add_number(op_id("d2r", n), "decay 2 rate", 0, 99, 20)
  params:set_action(op_id("d2r", n), function(x)
    engine[cmd("d2r")](x / 99)
    flash("OP" .. n .. " D2R", x)
  end)

  params:add_number(op_id("rel", n), "release rate", 0, 99, 55)
  params:set_action(op_id("rel", n), function(x)
    engine[cmd("rel")](x / 99)
    flash("OP" .. n .. " RR", x)
  end)

  params:add_number(op_id("ks", n), "key scale", 0, 99, 0)
  params:set_action(op_id("ks", n), function(x)
    engine[cmd("ks")](x / 99)
    flash("OP" .. n .. " KS", x)
  end)

  params:add_number(op_id("vs", n), "velocity", 0, 99,
    ({ 30, 50, 50, 50 })[n])
  params:set_action(op_id("vs", n), function(x)
    engine[cmd("vs")](x / 99)
    flash("OP" .. n .. " VEL", x)
  end)
end

function init()
  math.randomseed(math.floor(util.time() * 1000) % 100000)

  params:add_separator("voice")
  params:add_number("algo", "algorithm", 1, ALGOS, 1)
  params:set_action("algo", function(x)
    engine.algo(x - 1)
    flash("ALGORITHM", x)
    screen_dirty = true
  end)
  params:add_number("feedback", "feedback", 0, 99, 40)
  params:set_action("feedback", function(x)
    engine.feedback(x / 99)
    flash("FEEDBACK", x)
  end)
  params:add_number("dx_feedback", "dx feedback", 0, 99, 0)
  params:set_action("dx_feedback", function(x)
    engine.dxFeedback(x / 99)
    flash("DX FB", x)
  end)
  params:add_number("transpose", "transpose", -24, 24, 0)
  params:set_action("transpose", function(x)
    engine.transpose(x)
    flash("TRANSPOSE", x)
  end)

  for n = 1, OPS do
    add_op_params(n)
  end

  params:add_separator("lfo")
  params:add_number("lfo_rate", "rate", 0, 99, 30)
  params:set_action("lfo_rate", function(x)
    local hz = util.linexp(0, 99, 0.05, 40, x)
    engine.lfoRate(hz)
    flash("LFO", string.format("%.2fHz", hz))
  end)
  params:add_option("lfo_wave", "wave", LFO_WAVES, 1)
  params:set_action("lfo_wave", function(x)
    engine.lfoWave(x - 1)
    flash("LFO", LFO_WAVES[x])
  end)
  params:add_number("lfo_delay", "delay", 0, 99, 0)
  params:set_action("lfo_delay", function(x)
    engine.lfoDelay(x / 99)
    flash("LFO DELAY", x)
  end)
  params:add_number("pms", "pitch mod", 0, 99, 0)
  params:set_action("pms", function(x)
    engine.pms(x / 99)
    flash("PMS", x)
  end)
  params:add_number("ams", "amp mod", 0, 99, 0)
  params:set_action("ams", function(x)
    engine.ams(x / 99)
    flash("AMS", x)
  end)

  params:add_separator("pitch eg")
  params:add_number("peg_amt", "amount", -99, 99, 0)
  params:set_action("peg_amt", function(x)
    engine.pitchEgAmt(x / 99)
    flash("PEG", x)
  end)
  params:add_number("peg_rate", "rate", 0, 99, 50)
  params:set_action("peg_rate", function(x)
    engine.pitchEgRate(x / 99)
    flash("PEG RATE", x)
  end)
  params:add_number("peg_level", "init level", 0, 99, 99)
  params:set_action("peg_level", function(x)
    engine.pitchEgLevel(x / 99)
    flash("PEG LVL", x)
  end)

  params:add_separator("character")
  params:add_number("rate_scale", "rate scaling", 0, 99, 0)
  params:set_action("rate_scale", function(x)
    engine.rateScale(x / 99)
    flash("RATE SCL", x)
  end)
  params:add_number("bits", "bit crush", 0, 99, 0)
  params:set_action("bits", function(x)
    engine.bits(x / 99)
    flash("BITS", x)
  end)
  params:add_number("srate", "downsample", 0, 99, 0)
  params:set_action("srate", function(x)
    engine.srate(x / 99)
    flash("SRATE", x)
  end)
  params:add_number("drive", "drive", 0, 99, 0)
  params:set_action("drive", function(x)
    engine.drive(x / 99)
    flash("DRIVE", x)
  end)
  params:add_number("glitch", "glitch", 0, 99, 0)
  params:set_action("glitch", function(x)
    engine.glitch(x / 99)
    flash("GLITCH", x)
  end)
  params:add_number("hiss", "hiss", 0, 99, 0)
  params:set_action("hiss", function(x)
    engine.hiss(x / 99)
    flash("HISS", x)
  end)

  params:add_separator("fx")
  params:add_number("chorus", "chorus", 0, 99, 0)
  params:set_action("chorus", function(x)
    engine.chorus(x / 99)
    flash("CHORUS", x)
  end)
  params:add_number("chorus_rate", "chorus rate", 0, 99, 40)
  params:set_action("chorus_rate", function(x)
    engine.chorusRate(util.linexp(0, 99, 0.05, 4, x))
    flash("CH RATE", string.format("%.2fHz", util.linexp(0, 99, 0.05, 4, x)))
  end)
  params:add_number("chorus_width", "chorus width", 0, 99, 50)
  params:set_action("chorus_width", function(x)
    engine.chorusWidth(x / 99)
    flash("CH WID", x)
  end)
  params:add_number("phaser", "phaser", 0, 99, 0)
  params:set_action("phaser", function(x)
    engine.phaser(x / 99)
    flash("PHASER", x)
  end)
  params:add_number("phaser_rate", "phaser rate", 0, 99, 25)
  params:set_action("phaser_rate", function(x)
    engine.phaserRate(util.linexp(0, 99, 0.03, 2, x))
    flash("PH RATE", string.format("%.2fHz", util.linexp(0, 99, 0.03, 2, x)))
  end)
  params:add_number("phaser_width", "phaser width", 0, 99, 50)
  params:set_action("phaser_width", function(x)
    engine.phaserWidth(x / 99)
    flash("PH WID", x)
  end)

  params:add_separator("play")
  params:add_option("voice_mode", "voice", { "mono", "poly" }, 2)
  params:set_action("voice_mode", function(x)
    all_off()
    engine.voice_mode(x - 1)
    flash("VOICE", ({ "mono", "poly" })[x])
  end)
  params:set_save("voice_mode", true)
  params:add_number("port_time", "portamento", 0, 99, 0)
  params:set_action("port_time", function(x)
    engine.port(x == 0 and 0 or util.linexp(1, 99, 0.01, 2, x))
    flash("PORT", x)
  end)
  params:add_option("port_mode", "port mode", { "legato", "always" }, 1)
  params:set_action("port_mode", function(x)
    engine.portMode(x - 1)
    flash("PORT", ({ "legato", "always" })[x])
  end)
  params:add_number("kb_oct", "grid oct", 1, 6, 3)
  params:set_action("kb_oct", function() grid_dirty = true end)
  params:add_number("root", "root", 0, 11, 0)
  params:set_action("root", function() grid_dirty = true end)
  params:add_control("amp", "amp", controlspec.new(0, 1, "lin", 0, 0.4))
  params:set_action("amp", function(x)
    engine.amp(x)
    flash("AMP", string.format("%.2f", x))
  end)
  params:add_control("pan", "pan", controlspec.new(-1, 1, "lin", 0, 0))
  params:set_action("pan", function(x) engine.pan(x) end)

  params:add_trigger("clear_noise", "clear noise")
  params:set_action("clear_noise", function()
    -- everything under "character" back to off, plus the two randomiser
    -- settings that produce distortion or ring-mod style artifacts:
    -- heavy feedback, and fixed-frequency operators (in fixed mode an
    -- operator runs at the same constant hz in every voice, so voices
    -- sum coherently and beat against each other).
    for _, id in ipairs({ "bits", "srate", "drive", "glitch", "hiss",
                          "rate_scale" }) do
      params:set(id, 0)
    end
    params:set("feedback", 0)
    for i = 1, OPS do
      params:set(op_id("fixed", i), 1)  -- 1 = ratio, 2 = fixed hz
      params:set(op_id("det", i), 0)
    end
    flash("CLEAR", "noise")
  end)

  params:add_separator("headroom (wip)")
  -- temporary controls while we dial in polyphonic level. once a good
  -- pair is found these get hard-coded and the params removed.
  params:add_control("headroom", "headroom",
    controlspec.new(0, 2, "lin", 0, 1.0))
  params:set_action("headroom", function(x)
    engine.headroom(x)
    flash("HEADROOM", string.format("%.2f", x))
  end)
  -- 0 = none, 0.5 = 1/sqrt(N), 1.0 = 1/N
  params:add_control("scale_exp", "voice scaling",
    controlspec.new(0, 1.5, "lin", 0, 0.5))
  params:set_action("scale_exp", function(x)
    engine.scale_exp(x)
    flash("VSCALE", string.format("%.2f", x))
  end)

  params:add_separator("presets")
  params:add_number("preset", "voice", 1, #presets, 1)
  params:set_action("preset", function(x)
    if ready then load_preset(x) end
  end)
  params:add_trigger("load_preset", "load voice")
  params:set_action("load_preset", function()
    load_preset(params:get("preset"))
  end)
  params:add_trigger("randomize", "randomize")
  params:set_action("randomize", rnd_voice)

  params:add_separator("midi")
  params:add_number("midi_ch", "midi channel", 0, 16, 0)

  params.action_read = function()
    engine.voice_mode(params:get("voice_mode") - 1)
  end
  params:read()
  params:bang()
  ready = true

  for i = 1, 16 do
    midi_devs[i] = midi.connect(i)
    midi_devs[i].event = midi_event
  end

  gfx = metro.init()
  gfx.time = 1 / FPS
  gfx.event = function()
    if hud and (util.time() - focus_t) > 1.6 then
      hud = nil
      screen_dirty = true
    end
    if norns.menu.status() == false and screen_dirty then
      redraw()
      screen_dirty = false
    end
    if grid_dirty then
      grid_redraw()
      grid_dirty = false
    end
    arc_redraw()
  end
  gfx:start()

  grid_dirty = true
  screen_dirty = true
end

function enc(n, d)
  if shifted() then
    if n == 1 then
      params:delta("preset", d)
      load_preset(params:get("preset"))
    elseif n == 2 then
      params:delta(op_id("ratio", cur_op), d)
    else
      params:delta(op_id("wave", cur_op), d)
    end
  else
    if n == 1 then
      params:delta("algo", d)
    elseif n == 2 then
      params:delta("feedback", d)
    else
      params:delta(op_id("level", cur_op), d)
    end
  end
  screen_dirty = true
end

function key(n, z)
  if z ~= 1 then return end
  if n == 2 then
    cur_op = (cur_op % OPS) + 1
    flash("OP", cur_op)
  elseif n == 3 then
    if shifted() then
      all_off()
      flash("PANIC", "")
    else
      rnd_voice()
    end
  end
  screen_dirty = true
end

g.key = function(x, y, z)
  local note = grid_note(x, y)
  if z == 1 then
    grid_held[note] = (grid_held[note] or 0) + 1
    note_on(note, 0.88)
  else
    grid_held[note] = (grid_held[note] or 1) - 1
    if grid_held[note] <= 0 then
      grid_held[note] = nil
      note_off(note)
    end
  end
  grid_dirty = true
end

function a.delta(n, d)
  local ids = {
    op_id("level", cur_op), "feedback",
    op_id("d1r", cur_op), op_id("rel", cur_op)
  }
  params:delta(ids[n], d)
  screen_dirty = true
end

function cleanup()
  if gfx then gfx:stop() end
  engine.note_off_all()
end
