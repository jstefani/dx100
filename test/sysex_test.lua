-- Cross-check lib/sysex.lua's two parsers against the DX100 / DX21 /
-- TX81Z voice dump layouts (owner's manuals, MIDI data format).
-- One voice is packed by hand into a VCED (93 bytes) and a VMEM slot
-- (128 bytes); both must parse to the same table, and the known fields
-- must land where they should. Run from the repo root:
--
--   lua test/sysex_test.lua

package.path = "./lib/?.lua;" .. package.path
local S = require("sysex")

-- identity ratio table so ops[n].ratio == hardware F index + 1
local RATIOS = {}
for i = 1, 64 do RATIOS[i] = i end

-- operators in dump order: OP4, OP2, OP3, OP1
local OPS = {
  { ar = 31, d1r = 20, d2r = 10, rr = 8, d1l = 12, ls = 40, rs = 2, ebs = 5, ame = 1, kvs = 6, out = 90, f = 8, det = 5 },
  { ar = 30, d1r = 21, d2r = 11, rr = 9, d1l = 13, ls = 41, rs = 1, ebs = 4, ame = 0, kvs = 5, out = 91, f = 4, det = 1 },
  { ar = 29, d1r = 22, d2r = 12, rr = 10, d1l = 14, ls = 42, rs = 3, ebs = 3, ame = 1, kvs = 4, out = 92, f = 22, det = 3 },
  { ar = 28, d1r = 23, d2r = 13, rr = 11, d1l = 15, ls = 43, rs = 0, ebs = 2, ame = 0, kvs = 3, out = 93, f = 63, det = 6 },
}
local G = {
  alg = 4, fbl = 6, lfs = 70, lfd = 20, pmd = 50, amd = 30, sync = 1, lfw = 2,
  pms = 7, ams = 3, trps = 36, pbr = 2, ch = 1, mo = 1, su = 1, po = 0, pm = 1,
  port = 45, fcv = 99, mwp = 11, mwa = 22, bcp = 33, bca = 44, bcpb = 55,
  bceb = 66, name = "TESTVOICE ", pr = { 91, 92, 93 }, pl = { 71, 72, 73 },
}

local function push(t, ...)
  for _, v in ipairs({ ... }) do t[#t + 1] = v end
end

-- VCED: 13 bytes per op, then 41 common bytes (52..92)
local function vced()
  local b = {}
  for _, o in ipairs(OPS) do
    push(b, o.ar, o.d1r, o.d2r, o.rr, o.d1l, o.ls, o.rs, o.ebs, o.ame, o.kvs,
      o.out, o.f, o.det)
  end
  push(b, G.alg, G.fbl, G.lfs, G.lfd, G.pmd, G.amd, G.sync, G.lfw, G.pms,
    G.ams, G.trps, G.pbr, G.ch, G.mo, G.su, G.po, G.pm, G.port, G.fcv,
    G.mwp, G.mwa, G.bcp, G.bca, G.bcpb, G.bceb)
  for i = 1, 10 do push(b, G.name:byte(i)) end
  push(b, G.pr[1], G.pr[2], G.pr[3], G.pl[1], G.pl[2], G.pl[3])
  assert(#b == 93, "vced is " .. #b .. " bytes")
  return b
end

-- VMEM: 10 packed bytes per op, common bytes 40..72, zero to 128
local function vmem()
  local b = {}
  for _, o in ipairs(OPS) do
    push(b, o.ar, o.d1r, o.d2r, o.rr, o.d1l, o.ls,
      (o.ame << 6) | (o.ebs << 3) | o.kvs,
      o.out, o.f,
      (o.rs << 3) | o.det)
  end
  push(b, (G.sync << 6) | (G.fbl << 3) | G.alg,
    G.lfs, G.lfd, G.pmd, G.amd,
    (G.pms << 4) | (G.ams << 2) | G.lfw,
    G.trps, G.pbr,
    (G.ch << 4) | (G.mo << 3) | (G.su << 2) | (G.po << 1) | G.pm,
    G.port, G.fcv, G.mwp, G.mwa, G.bcp, G.bca, G.bcpb, G.bceb)
  for i = 1, 10 do push(b, G.name:byte(i)) end
  push(b, G.pr[1], G.pr[2], G.pr[3], G.pl[1], G.pl[2], G.pl[3])
  while #b < 128 do push(b, 0) end
  return b
end

-- flatten a voice to sorted "key=value" lines for diffing
local function flat(v)
  local out = {}
  local function add(k, x) out[#out + 1] = k .. "=" .. tostring(x) end
  add("name", v.name); add("algo", v.algo); add("fb", v.fb)
  add("transpose", v.transpose)
  for k, x in pairs(v.extra) do add("extra." .. k, x) end
  for op = 1, 4 do
    for k, x in pairs(v.ops[op]) do add("op" .. op .. "." .. k, x) end
  end
  table.sort(out)
  return out
end

local fails = 0
local function check(cond, msg)
  if not cond then
    fails = fails + 1
    print("FAIL " .. msg)
  end
end

local a = S.parse_vced(vced(), 1, RATIOS)
local b = S.parse_vmem(vmem(), 1, RATIOS)

-- 1. the two layouts agree
local fa, fb = flat(a), flat(b)
check(#fa == #fb, "field count differs")
for i = 1, math.max(#fa, #fb) do
  check(fa[i] == fb[i], "vced " .. tostring(fa[i]) .. " ~= vmem " .. tostring(fb[i]))
end

-- 2. known fields land where the spec puts them (checked on the VCED,
-- which the VMEM must match per step 1)
local function eq(k, got, want)
  check(got == want, k .. ": got " .. tostring(got) .. ", want " .. tostring(want))
end
eq("name", a.name, "testvoice")
eq("algo", a.algo, G.alg + 1)
eq("fb", a.fb, G.fbl)
eq("transpose", a.transpose, G.trps - 24)
eq("lfo_rate", a.extra.lfo_rate, G.lfs)
eq("lfo_delay", a.extra.lfo_delay, G.lfd)
eq("lfo_sync", a.extra.lfo_sync, 2)
eq("lfo_wave (tri)", a.extra.lfo_wave, 1)
eq("bend_range", a.extra.bend_range, G.pbr)
eq("chorus", a.extra.chorus, 60)
eq("voice_mode (mono)", a.extra.voice_mode, 1)
eq("port_mode (fingered -> legato)", a.extra.port_mode, 1)
eq("port_time", a.extra.port_time, G.port)
eq("mw_pitch", a.extra.mw_pitch, G.mwp)
eq("mw_amp", a.extra.mw_amp, G.mwa)
eq("bc_pitch", a.extra.bc_pitch, G.bcp)
eq("bc_amp", a.extra.bc_amp, G.bca)
eq("bc_pbias", a.extra.bc_pbias, G.bcpb)
eq("bc_egbias", a.extra.bc_egbias, G.bceb)
for i = 1, 3 do
  eq("peg_r" .. i, a.extra["peg_r" .. i], G.pr[i])
  eq("peg_l" .. i, a.extra["peg_l" .. i], G.pl[i])
end
-- op slot order OP4, OP2, OP3, OP1
local slot_of = { [4] = 1, [2] = 2, [3] = 3, [1] = 4 }
for op = 1, 4 do
  local o, h = a.ops[op], OPS[slot_of[op]]
  local pre = "op" .. op .. "."
  eq(pre .. "ratio", o.ratio, h.f + 1)
  eq(pre .. "level", o.level, h.out)
  eq(pre .. "ar", o.ar, h.ar); eq(pre .. "d1r", o.d1r, h.d1r)
  eq(pre .. "d1l", o.d1l, h.d1l); eq(pre .. "d2r", o.d2r, h.d2r)
  eq(pre .. "rr", o.rr, h.rr)
  eq(pre .. "det", o.det, h.det - 3)
  eq(pre .. "kls", o.kls, h.ls); eq(pre .. "kvs", o.kvs, h.kvs)
  eq(pre .. "krs", o.krs, h.rs); eq(pre .. "ebs", o.ebs, h.ebs)
  eq(pre .. "ame", o.ame, h.ame == 1)
end

-- 3. poly / full-time variant flips the two mode fields
G.mo, G.pm = 0, 0
local c = S.parse_vced(vced(), 1, RATIOS)
local d = S.parse_vmem(vmem(), 1, RATIOS)
eq("voice_mode (poly) vced", c.extra.voice_mode, 2)
eq("voice_mode (poly) vmem", d.extra.voice_mode, 2)
eq("port_mode (full time) vced", c.extra.port_mode, 2)
eq("port_mode (full time) vmem", d.extra.port_mode, 2)

-- 4. framed messages: header, checksum, EOX
local function framed(fmt, count, data)
  local sum = 0
  for _, x in ipairs(data) do sum = sum + x end
  local msg = { 0xf0, 0x43, 0x00, fmt, count >> 7, count & 0x7f }
  for _, x in ipairs(data) do msg[#msg + 1] = x end
  msg[#msg + 1] = (-sum) & 0x7f
  msg[#msg + 1] = 0xf7
  return msg
end
G.mo, G.pm = 1, 1
local voices, warn = S.parse(framed(0x03, 93, vced()), RATIOS)
eq("framed vced voice count", #voices, 1)
eq("framed vced warnings", #warn, 0)
eq("framed vced mono", voices[1] and voices[1].extra.voice_mode, 1)

local bank = {}
for _ = 1, 32 do for _, x in ipairs(vmem()) do bank[#bank + 1] = x end end
voices, warn = S.parse(framed(0x04, 4096, bank), RATIOS)
eq("framed vmem voice count", #voices, 32)
eq("framed vmem warnings", #warn, 0)

if fails == 0 then
  print("ok: vced and vmem agree, all fields in place")
else
  print(fails .. " failure(s)")
  os.exit(1)
end
