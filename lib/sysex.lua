-- DX100 / DX21 / DX27 voice sysex -> dx100 preset tables.
-- Pure Lua (no norns globals) so it can be tested off-device.
--
-- Formats (DX100 owner's manual, section 5):
--   VCED  F0 43 0n 03 00 5D  <93 bytes>  <sum> F7   one voice (edit buffer)
--   VMEM  F0 43 0n 04 20 00  <4096 bytes> <sum> F7  32 voices, 128 each
-- Operators are stored in the order OP4, OP2, OP3, OP1. The checksum is
-- the low 7 bits of the two's complement of the data sum.
--
-- Output voices use the same table shape as lib/presets.lua (hardware
-- units), plus an `extra` map of param id -> value for the LFO, pitch
-- EG, performance and wheel/breath settings the presets do not carry.

local M = {}

local OP_ORDER = { 4, 2, 3, 1 }

-- LFO wave: hardware 0 saw up, 1 square, 2 triangle, 3 s/h
-- script LFO_WAVES: 1 tri, 2 sin, 3 sqr, 4 s&h, 5 up, 6 down
local LFW = { [0] = 5, [1] = 3, [2] = 1, [3] = 4 }

-- PMS 0-7 -> max pitch swing in cents at PMD 99 (7 = 800, per manual;
-- halving per step below). AMS 0-3 -> dB at AMD 99 (3 = 96, per manual).
local PMS_CENTS = { [0] = 0, 12.5, 25, 50, 100, 200, 400, 800 }
local AMS_DB = { [0] = 0, 24, 48, 96 }

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function round(v) return math.floor(v + 0.5) end

-- the script's single pms/ams params: cents = 800 * (pms/99)^3,
-- dB = 96 * (ams/99)^2. invert.
local function pms_param(pms, pmd)
  local cents = (PMS_CENTS[clamp(pms, 0, 7)] or 0) * clamp(pmd, 0, 99) / 99
  if cents <= 0 then return 0 end
  return clamp(round(99 * (cents / 800) ^ (1 / 3)), 0, 99)
end

local function ams_param(ams, amd)
  local db = (AMS_DB[clamp(ams, 0, 3)] or 0) * clamp(amd, 0, 99) / 99
  if db <= 0 then return 0 end
  return clamp(round(99 * math.sqrt(db / 96)), 0, 99)
end

local function name_of(bytes, at)
  local t = {}
  for i = 0, 9 do
    local b = bytes[at + i] or 32
    if b < 32 or b > 126 then b = 32 end
    t[#t + 1] = string.char(b)
  end
  local s = table.concat(t):gsub("%s+$", ""):gsub("^%s+", "")
  if s == "" then s = "(untitled)" end
  return s:lower()
end

-- shared tail: everything after the operators, given a field table
local function build_voice(ops, f, ratios)
  local v = {
    name = f.name,
    algo = clamp(f.alg, 0, 7) + 1,
    fb = clamp(f.fbl, 0, 7),
    transpose = clamp(f.trps, 0, 48) - 24,
    ops = {},
    imported = true,
    extra = {
      lfo_rate = clamp(f.lfs, 0, 99),
      lfo_delay = clamp(f.lfd, 0, 99),
      lfo_sync = (f.sync == 1) and 2 or 1,
      lfo_wave = LFW[clamp(f.lfw, 0, 3)] or 1,
      pms = pms_param(f.pms, f.pmd),
      ams = ams_param(f.ams, f.amd),
      peg_r1 = clamp(f.pr[1], 0, 99), peg_r2 = clamp(f.pr[2], 0, 99),
      peg_r3 = clamp(f.pr[3], 0, 99),
      peg_l1 = clamp(f.pl[1], 0, 99), peg_l2 = clamp(f.pl[2], 0, 99),
      peg_l3 = clamp(f.pl[3], 0, 99),
      mw_pitch = clamp(f.mwp, 0, 99), mw_amp = clamp(f.mwa, 0, 99),
      bc_pitch = clamp(f.bcp, 0, 99), bc_amp = clamp(f.bca, 0, 99),
      bc_pbias = clamp(f.bcpb, 0, 99), bc_egbias = clamp(f.bceb, 0, 99),
      bend_range = clamp(f.pbr, 0, 12),
      port_time = clamp(f.port, 0, 99),
      -- hardware 0 = full time, 1 = fingered; script 1 = legato, 2 = always
      port_mode = (f.pm == 1) and 1 or 2,
      voice_mode = (f.mono == 1) and 1 or 2,
      -- the DX21 chorus is a switch; give it a middling depth
      chorus = (f.ch == 1) and 60 or 0,
    },
  }
  for slot, op in ipairs(OP_ORDER) do
    local o = ops[slot]
    v.ops[op] = {
      ratio = ratios[clamp(o.f, 0, 63) + 1] or 1.0,
      level = clamp(o.out, 0, 99),
      ar = clamp(o.ar, 0, 31), d1r = clamp(o.d1r, 0, 31),
      d1l = clamp(o.d1l, 0, 15), d2r = clamp(o.d2r, 0, 31),
      rr = clamp(o.rr, 0, 15),
      wave = 1, det = clamp(o.det, 0, 6) - 3,
      kls = clamp(o.ls, 0, 99), kvs = clamp(o.kvs, 0, 7),
      krs = clamp(o.rs, 0, 3), ame = (o.ame == 1), ebs = clamp(o.ebs, 0, 7),
    }
  end
  return v
end

-- 93 unpacked bytes at bytes[at..]
function M.parse_vced(bytes, at, ratios)
  local ops = {}
  for slot = 1, 4 do
    local b = at + (slot - 1) * 13
    ops[slot] = {
      ar = bytes[b], d1r = bytes[b + 1], d2r = bytes[b + 2], rr = bytes[b + 3],
      d1l = bytes[b + 4], ls = bytes[b + 5], rs = bytes[b + 6],
      ebs = bytes[b + 7], ame = bytes[b + 8], kvs = bytes[b + 9],
      out = bytes[b + 10], f = bytes[b + 11], det = bytes[b + 12],
    }
  end
  local p = at + 52
  local f = {
    alg = bytes[p], fbl = bytes[p + 1], lfs = bytes[p + 2], lfd = bytes[p + 3],
    pmd = bytes[p + 4], amd = bytes[p + 5], sync = bytes[p + 6],
    lfw = bytes[p + 7], pms = bytes[p + 8], ams = bytes[p + 9],
    trps = bytes[p + 10], mono = bytes[p + 11], pbr = bytes[p + 12],
    pm = bytes[p + 13], port = bytes[p + 14],
    -- p+15 foot volume, p+16 sustain fs, p+17 portamento fs
    ch = bytes[p + 18],
    mwp = bytes[p + 19], mwa = bytes[p + 20], bcp = bytes[p + 21],
    bca = bytes[p + 22], bcpb = bytes[p + 23], bceb = bytes[p + 24],
    name = name_of(bytes, p + 25),
    pr = { bytes[p + 35], bytes[p + 36], bytes[p + 37] },
    pl = { bytes[p + 38], bytes[p + 39], bytes[p + 40] },
  }
  return build_voice(ops, f, ratios)
end

-- 128 packed bytes at bytes[at..]
function M.parse_vmem(bytes, at, ratios)
  local ops = {}
  for slot = 1, 4 do
    local b = at + (slot - 1) * 10
    local b6, b9 = bytes[b + 6], bytes[b + 9]
    ops[slot] = {
      ar = bytes[b], d1r = bytes[b + 1], d2r = bytes[b + 2], rr = bytes[b + 3],
      d1l = bytes[b + 4], ls = bytes[b + 5],
      ame = (b6 >> 6) & 1, ebs = (b6 >> 3) & 7, kvs = b6 & 7,
      out = bytes[b + 7], f = bytes[b + 8],
      rs = (b9 >> 3) & 3, det = b9 & 7,
    }
  end
  local p = at + 40
  local b40, b45, b48 = bytes[p], bytes[p + 5], bytes[p + 8]
  local f = {
    sync = (b40 >> 6) & 1, fbl = (b40 >> 3) & 7, alg = b40 & 7,
    lfs = bytes[p + 1], lfd = bytes[p + 2], pmd = bytes[p + 3], amd = bytes[p + 4],
    pms = (b45 >> 4) & 7, ams = (b45 >> 2) & 3, lfw = b45 & 3,
    trps = bytes[p + 6], pbr = bytes[p + 7],
    ch = (b48 >> 4) & 1, mono = (b48 >> 3) & 1, pm = b48 & 1,
    port = bytes[p + 9],
    -- p+10 foot volume
    mwp = bytes[p + 11], mwa = bytes[p + 12], bcp = bytes[p + 13],
    bca = bytes[p + 14], bcpb = bytes[p + 15], bceb = bytes[p + 16],
    name = name_of(bytes, p + 17),
    pr = { bytes[p + 27], bytes[p + 28], bytes[p + 29] },
    pl = { bytes[p + 30], bytes[p + 31], bytes[p + 32] },
  }
  return build_voice(ops, f, ratios)
end

local function checksum_ok(bytes, at, n)
  local sum = 0
  for i = at, at + n - 1 do sum = sum + (bytes[i] or 0) end
  return ((sum + (bytes[at + n] or 0)) & 0x7f) == 0
end

-- bytes: 1-indexed array of 0..255. Returns list of voices (may be
-- empty) and a list of warning strings.
function M.parse(bytes, ratios)
  local voices, warn = {}, {}
  local n = #bytes
  local i = 1
  local found = false
  while i <= n do
    if bytes[i] == 0xf0 and bytes[i + 1] == 0x43 and bytes[i + 3] then
      local fmt = bytes[i + 3]
      local data = i + 6
      if fmt == 0x03 and data + 92 <= n then
        found = true
        if not checksum_ok(bytes, data, 93) then
          warn[#warn + 1] = "vced checksum mismatch"
        end
        voices[#voices + 1] = M.parse_vced(bytes, data, ratios)
        i = data + 94
      elseif fmt == 0x04 and data + 4095 <= n then
        found = true
        if not checksum_ok(bytes, data, 4096) then
          warn[#warn + 1] = "vmem checksum mismatch"
        end
        for v = 0, 31 do
          voices[#voices + 1] = M.parse_vmem(bytes, data + v * 128, ratios)
        end
        i = data + 4097
      else
        -- some other yamaha message (parameter change etc): skip to EOX
        local j = i + 1
        while j <= n and bytes[j] ~= 0xf7 do j = j + 1 end
        i = j + 1
      end
    else
      i = i + 1
    end
  end
  if not found then
    -- headerless dumps: raw VMEM banks (multiples of 128) or a raw VCED
    if n == 93 then
      voices[1] = M.parse_vced(bytes, 1, ratios)
    elseif n >= 128 and n % 128 == 0 then
      for v = 0, n // 128 - 1 do
        voices[#voices + 1] = M.parse_vmem(bytes, 1 + v * 128, ratios)
      end
    else
      warn[#warn + 1] = "no dx100 voice data found"
    end
  end
  -- drop the empty slots a half-filled bank ships with
  local out = {}
  for _, v in ipairs(voices) do
    local live = false
    for op = 1, 4 do
      if v.ops[op].level > 0 then live = true end
    end
    if live then out[#out + 1] = v end
  end
  return out, warn
end

function M.read_file(path)
  local fd = io.open(path, "rb")
  if not fd then return nil, "cannot open " .. tostring(path) end
  local s = fd:read("a")
  fd:close()
  local bytes = {}
  for k = 1, #s do bytes[k] = s:byte(k) end
  return bytes
end

function M.load_file(path, ratios)
  local bytes, err = M.read_file(path)
  if not bytes then return {}, { err } end
  return M.parse(bytes, ratios)
end

return M
