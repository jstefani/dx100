-- factory voices, written in DX100 units so they read against a patch
-- sheet: level 0-99, AR/D1R/D2R 0-31, D1L 0-15, RR 0-15, detune -3..3,
-- KLS 0-99, KVS 0-7, KRS 0-3, EBS 0-7, AME true/false. Ratios snap to
-- the 64-entry table. Feedback 0-7.
--
-- Levels are dB (0.75 dB/step near the top): a modulator at 66 is
-- -25 dB, i.e. a gentle index; 80 is brash; 90+ is harsh.

local function op(ratio, level, ar, d1r, d1l, d2r, rr, o)
  o = o or {}
  return {
    ratio = ratio, level = level,
    ar = ar, d1r = d1r, d1l = d1l, d2r = d2r, rr = rr,
    wave = o.wave or 1, det = o.det or 0,
    kls = o.kls or 0, kvs = o.kvs or 0, krs = o.krs or 0,
    ame = o.ame or false, ebs = o.ebs or 0,
  }
end

return {
  {
    name = "solid bass",
    algo = 1, fb = 6, transpose = -12,
    ops = {
      op(1.00, 99, 31, 12, 12, 6, 8, { kvs = 1 }),
      op(1.00, 80, 31, 14, 8, 8, 8, { kvs = 3 }),
      op(1.00, 72, 31, 16, 5, 10, 8, { kvs = 4 }),
      op(2.00, 65, 31, 18, 3, 10, 8, { kvs = 5 }),
    },
  },
  {
    -- the owner's manual tutorial voice (ch. IV): two pairs, the
    -- 7.00 modulator on op4 gives the tine "ping"
    name = "e.piano",
    algo = 5, fb = 5, transpose = 0,
    ops = {
      op(1.00, 99, 31, 10, 10, 8, 8, { kls = 20, kvs = 2 }),
      op(1.00, 66, 31, 10, 10, 8, 8, { kls = 30, kvs = 4, ame = true }),
      op(1.00, 70, 31, 13, 0, 0, 10, { kvs = 3 }),
      op(7.00, 71, 31, 13, 0, 0, 10, { kvs = 5 }),
    },
  },
  {
    name = "brass",
    algo = 2, fb = 4, transpose = 0,
    ops = {
      op(1.00, 99, 20, 10, 13, 4, 9, { kvs = 2 }),
      op(1.00, 82, 18, 8, 11, 4, 9, { det = 2, kvs = 4 }),
      op(1.00, 70, 17, 8, 10, 4, 9, { det = -2, kvs = 4 }),
      op(2.00, 62, 16, 8, 9, 4, 9, { kvs = 5 }),
    },
  },
  {
    name = "glass bell",
    algo = 6, fb = 2, transpose = 0,
    ops = {
      op(1.00, 99, 31, 8, 0, 0, 6, { kvs = 2, krs = 1 }),
      op(3.46, 70, 31, 9, 0, 0, 6, { det = 2, kvs = 3, krs = 1 }),
      op(7.07, 55, 31, 10, 0, 0, 6, { det = -2, kvs = 4, krs = 2 }),
      op(3.14, 72, 31, 12, 0, 0, 8, { kls = 30, kvs = 5, krs = 2 }),
    },
  },
  {
    name = "wood marimba",
    algo = 5, fb = 0, transpose = 0,
    ops = {
      op(1.00, 99, 31, 14, 0, 0, 9, { kvs = 3, krs = 2 }),
      op(3.46, 62, 31, 20, 0, 0, 10, { kvs = 5, krs = 2 }),
      op(4.00, 55, 31, 16, 0, 0, 10, { kls = 40, kvs = 3, krs = 2 }),
      op(9.89, 60, 31, 22, 0, 0, 10, { kvs = 5, krs = 3 }),
    },
  },
  {
    name = "lately bass",
    algo = 1, fb = 7, transpose = -12,
    ops = {
      op(0.50, 99, 31, 10, 10, 7, 8, { kvs = 1 }),
      op(1.00, 84, 31, 12, 8, 8, 8, { kvs = 3 }),
      op(1.00, 70, 31, 14, 6, 9, 8, { kvs = 4 }),
      op(1.00, 68, 31, 16, 4, 10, 8, { det = 2, kvs = 5 }),
    },
  },
  {
    name = "hollow pad",
    algo = 7, fb = 3, transpose = 0,
    ops = {
      op(1.00, 99, 14, 6, 14, 0, 5, { ame = true }),
      op(2.00, 78, 13, 6, 13, 0, 5, { det = 3, ame = true }),
      op(1.00, 75, 13, 6, 13, 0, 5, { det = -3 }),
      op(1.00, 60, 12, 5, 12, 0, 5, { wave = 2 }),
    },
  },
  {
    name = "clav",
    algo = 3, fb = 6, transpose = 0,
    ops = {
      op(1.00, 99, 31, 12, 9, 9, 10, { kvs = 2 }),
      op(3.00, 78, 31, 14, 5, 10, 10, { kvs = 5 }),
      op(1.00, 66, 31, 15, 4, 10, 10, { kvs = 4 }),
      op(1.00, 68, 31, 18, 0, 10, 10, { kls = 30, kvs = 6 }),
    },
  },
}
