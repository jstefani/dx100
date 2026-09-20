-- dx100 splash: italic striped "DX", hollow "100", tagline.
-- the logo is rendered once into horizontal runs at load; draw() blits
-- them with per-frame tape wear: flicker, tears, dropouts, noise. shapes are sampled per pixel in letter space, sheared for
-- the italic lean, then outlined by a 1px edge test.

local H, SH, GAP = 26, 4, 4 -- logo height, shear px, letter gap
local TOP = 5                -- logo y on screen

local function rrect(x, y, x0, x1, y0, y1, r, left, right)
  if x < x0 or x > x1 or y < y0 or y > y1 then return false end
  local function inc(cx, cy)
    return (x - cx) ^ 2 + (y - cy) ^ 2 <= r * r + r * 0.5
  end
  if right and x > x1 - r then
    if y < y0 + r then return inc(x1 - r, y0 + r) end
    if y > y1 - r then return inc(x1 - r, y1 - r) end
  end
  if left and x < x0 + r then
    if y < y0 + r then return inc(x0 + r, y0 + r) end
    if y > y1 - r then return inc(x0 + r, y1 - r) end
  end
  return true
end

local function D(x, y, W)
  local t = 6
  if not rrect(x, y, 0, W - 1, 0, H - 1, 12, false, true) then return false end
  return not rrect(x, y, t, W - 1 - t, t, H - 1 - t, 12 - t, false, true)
end

local function X(x, y, W)
  local w = 9
  local f = y / (H - 1)
  local c1, c2 = (W - 1) * f, (W - 1) * (1 - f)
  return x >= 0 and x <= W - 1
    and (math.abs(x - c1) <= w / 2 or math.abs(x - c2) <= w / 2)
end

local function ONE(x, y, W)
  if y >= 0 and y <= H - 1 and x >= 2 and x <= W - 1 then return true end
  return y >= 0 and y <= 9 and x >= 0 and x <= W - 1
end

local function ZERO(x, y, W)
  return rrect(x, y, 0, W - 1, 0, H - 1, 8, true, true)
end

-- { shape, width, mode }: "s" striped fill, "o" outline only
local LETTERS = {
  { D, 28, "s" }, { X, 28, "s" },
  { ONE, 12, "o" }, { ZERO, 18, "o" }, { ZERO, 18, "o" },
}

local function build()
  local grid = {}
  for y = 0, H - 1 do grid[y] = {} end
  local ox = 2
  for _, L in ipairs(LETTERS) do
    local fn, W, mode = L[1], L[2], L[3]
    local WW = W + SH + 2
    local ins = {}
    for y = 0, H - 1 do
      ins[y] = {}
      for x = 0, WW - 1 do
        ins[y][x] = fn(x - SH * (H - 1 - y) / (H - 1), y, W)
      end
    end
    local function solid(y, x)
      return y >= 0 and y < H and x >= 0 and x < WW and ins[y][x]
    end
    local k = (mode == "s") and 1 or 3
    for y = 0, H - 1 do
      for x = 0, WW - 1 do
        if ins[y][x] then
          local edge = false
          for dy = -k, k do
            for dx = -k, k do
              if math.abs(dx) + math.abs(dy) <= k
                and not solid(y + dy, x + dx) then
                edge = true
              end
            end
          end
          if edge or (mode == "s" and y % 2 == 0) then
            grid[y][ox + x] = true
          end
        end
      end
    end
    ox = ox + W + GAP
  end
  -- collapse to horizontal runs { y, x, len }
  local runs = {}
  for y = 0, H - 1 do
    local x = 0
    while x < 128 do
      if grid[y][x] then
        local s = x
        while x < 128 and grid[y][x] do x = x + 1 end
        runs[#runs + 1] = { y + TOP, s, x - s }
      else
        x = x + 1
      end
    end
  end
  return runs
end

local M = { runs = build(), frame = 0 }

local function rnd(a, b) return math.random(a, b) end

-- one frame of worn-tape playback. call every tick while the splash is up.
function M.draw()
  M.frame = M.frame + 1
  screen.clear()
  screen.aa(0)

  -- brightness: mostly steady, occasional dips like a weak signal
  local base = 15
  local r = math.random()
  if r < 0.06 then base = rnd(5, 9)
  elseif r < 0.25 then base = rnd(12, 14) end

  -- whole-frame jitter: small, rare
  local jx = (math.random() < 0.15) and rnd(-1, 1) or 0
  local jy = (math.random() < 0.05) and rnd(-2, 2) or 0

  -- tear band: a slab of rows dragged sideways
  local ty, th, tdx = -1, 0, 0
  if math.random() < 0.4 then
    ty, th, tdx = rnd(0, 60), rnd(2, 9), rnd(-4, 4)
  end
  local function tear(y) return (y >= ty and y < ty + th) and tdx or 0 end

  -- logo
  for _, run in ipairs(M.runs) do
    local y = run[1] + jy
    local x = run[2] + jx + tear(y)
    local lvl = base
    if math.random() < 0.05 then lvl = rnd(1, 6) end -- dropout
    screen.level(lvl)
    screen.rect(x, y, run[3], 1)
    screen.fill()
  end

  -- tagline: dimmer, same tears, one line sometimes blinks out
  screen.font_face(1)
  screen.font_size(8)
  local tl = math.max(1, math.floor(base * 0.45))
  for i, line in ipairs({ { 46, "DIGITAL PROGRAMMABLE" },
                           { 57, "ALGORITHM SYNTHESIZER" } }) do
    if math.random() > 0.04 then
      local y = line[1] + jy
      screen.level(tl)
      screen.move(64 + jx + tear(y - 4), y)
      screen.text_center(line[2])
    end
  end

  -- head-switching noise: a band that creeps down the screen and wraps
  local band = (M.frame * 3) % 90 - 12
  for _ = 1, 40 do
    local y = band + rnd(0, 4)
    if y >= 0 and y <= 63 then
      screen.level(rnd(1, 8))
      screen.rect(rnd(0, 127), y, rnd(1, 6), 1)
      screen.fill()
    end
  end
  -- plus a constant fizz along the bottom edge
  for _ = 1, 14 do
    screen.level(rnd(1, 5))
    screen.rect(rnd(0, 127), rnd(61, 63), rnd(1, 4), 1)
    screen.fill()
  end

  screen.update()
end

return M
