"""BSIM3v3.3 drain current and charges in plain Python, as a second implementation.

Written separately from src/circuit/devices/Bsim3.cpp (scalar arithmetic, no
derivatives, another structure) from the same model equations of the BSIM
group, University of California, Berkeley. tests/test_bsim3.py runs the
simulator at many bias points and compares the two; a slip in either
transcription shows up as a difference.

    ids, isub = drain_current(params, "nmos", vgs, vds, vbs, temp=27.0)
    qd, qg, qs, qb = terminal_charges(params, "nmos", vd, vg, vs, vb, temp=27.0)

params: model and instance parameters by their lower-case names. The drain
current takes voltages in the n-channel frame with vds >= 0 (for a p-channel:
all voltages negated); the charges take the terminal voltages as they are.
"""

import math

EPSOX = 3.453133e-11
EPSSI = 1.03594e-10
KBOQ = 8.617087e-5
Q = 1.60219e-19
MAX_EXP = 5.834617425e14
MIN_EXP = 1.713908431e-15
EXP_THRESHOLD = 34.0

ALIASES = {"vtho": "vth0", "nch": "npeak", "pdiblc1": "pdibl1", "pdiblc2": "pdibl2", "pdiblcb": "pdiblb"}


def _canonical(params):
    out = {}
    for key, value in params.items():
        key = key.lower()
        for prefix in ("", "l", "w", "p"):
            if key.startswith(prefix) and key[len(prefix):] in ALIASES:
                key = prefix + ALIASES[key[len(prefix):]]
                break
        out[key] = float(value)
    return out


class Prepared:
    """Everything that depends on size and temperature only."""

    def __init__(self, params, kind, temp=27.0, tnom_default=27.0):
        raw = _canonical(params)
        sign = 1.0 if kind == "nmos" else -1.0
        get = lambda k, d: raw.get(k, d)  # noqa: E731
        tnom = get("tnom", tnom_default) + 273.15
        temp = temp + 273.15
        ratio = temp / tnom
        self.temp_ratio = ratio - 1.0

        self.tox = get("tox", 150e-10)
        toxm = get("toxm", self.tox)
        self.cox = EPSOX / self.tox
        self.factor1 = math.sqrt(EPSSI / EPSOX * self.tox)
        self.mobmod = int(get("mobmod", 1))
        self.mult = get("m", 1.0)

        vtm0 = KBOQ * tnom
        eg0 = 1.16 - 7.02e-4 * tnom * tnom / (tnom + 1108.0)
        ni = 1.45e10 * (tnom / 300.15) * math.sqrt(tnom / 300.15) * math.exp(21.5565981 - eg0 / (2.0 * vtm0))
        self.vtm = KBOQ * temp

        length, width = get("l", 5e-6), get("w", 5e-6)
        dl = (get("lint", 0.0) + get("ll", 0.0) / length ** get("lln", 1.0) + get("lw", 0.0) / width ** get("lwn", 1.0)
              + get("lwl", 0.0) / (length ** get("lln", 1.0) * width ** get("lwn", 1.0)))
        dw = (get("wint", 0.0) + get("wl", 0.0) / length ** get("wln", 1.0) + get("ww", 0.0) / width ** get("wwn", 1.0)
              + get("wwl", 0.0) / (length ** get("wln", 1.0) * width ** get("wwn", 1.0)))
        self.leff = length + get("xl", 0.0) - 2.0 * dl
        self.weff = width + get("xw", 0.0) - 2.0 * dw

        unit = 1e-6 if int(get("binunit", 1)) == 1 else 1.0
        inv_l, inv_w, inv_lw = unit / self.leff, unit / self.weff, unit * unit / (self.leff * self.weff)

        def given(k):
            return any(p + k in raw for p in ("", "l", "w", "p"))

        def bin_(k, default):
            return get(k, default) + get("l" + k, 0.0) * inv_l + get("w" + k, 0.0) * inv_w + get("p" + k, 0.0) * inv_lw

        for name, default in (("cdsc", 2.4e-4), ("cdscb", 0.0), ("cdscd", 0.0), ("cit", 0.0), ("nfactor", 1.0),
                              ("xj", 0.15e-6), ("a0", 1.0), ("ags", 0.0), ("a1", 0.0), ("a2", 1.0),
                              ("keta", -0.047), ("ngate", 0.0), ("kt1", -0.11), ("kt1l", 0.0), ("kt2", 0.022),
                              ("k3", 80.0), ("k3b", 0.0), ("w0", 2.5e-6), ("nlx", 1.74e-7), ("dvt0", 2.2),
                              ("dvt1", 0.53), ("dvt2", -0.032), ("dvt0w", 0.0), ("dvt1w", 5.3e6),
                              ("dvt2w", -0.032), ("voff", -0.08), ("delta", 0.01), ("prwg", 0.0), ("prwb", 0.0),
                              ("eta0", 0.08), ("etab", -0.07), ("pclm", 1.3), ("pdiblb", 0.0), ("pscbe1", 4.24e8),
                              ("pscbe2", 1e-5), ("pvag", 0.0), ("dwg", 0.0), ("dwb", 0.0), ("b0", 0.0),
                              ("b1", 0.0), ("alpha0", 0.0), ("alpha1", 0.0), ("beta0", 30.0)):
            setattr(self, name, bin_(name, default))

        vsat, at = bin_("vsat", 8e4), bin_("at", 3.3e4)
        nsub, npeak = bin_("nsub", 6e16), bin_("npeak", 1.7e17)
        gamma1, gamma2 = bin_("gamma1", 0.0), bin_("gamma2", 0.0)
        vbx, vbm, xt = bin_("vbx", 0.0), bin_("vbm", -3.0), bin_("xt", 1.55e-7)
        drout = bin_("drout", 0.56)
        dsub = bin_("dsub", 0.0) if given("dsub") else drout
        third = self.mobmod == 3
        self.ua = bin_("ua", 2.25e-9) + bin_("ua1", 4.31e-9) * self.temp_ratio
        self.ub = bin_("ub", 5.87e-19) + bin_("ub1", -7.61e-18) * self.temp_ratio
        self.uc = bin_("uc", -0.0465 if third else -0.0465e-9) + bin_("uc1", -0.056 if third else -0.056e-9) * self.temp_ratio
        u0 = bin_("u0", 0.067 if sign > 0 else 0.025)
        if u0 > 1.0:
            u0 /= 1e4
        self.u0temp = u0 * ratio ** bin_("ute", -1.5)
        self.vsattemp = vsat - at * self.temp_ratio
        self.rds0 = (bin_("rdsw", 0.0) + bin_("prt", 0.0) * self.temp_ratio) / (self.weff * 1e6) ** bin_("wr", 1.0)

        if npeak > 1e20:
            npeak *= 1e-6
        if nsub > 1e20:
            nsub *= 1e-6
        if self.ngate > 1e23:
            self.ngate *= 1e-6
        if not given("npeak") and given("gamma1"):
            npeak = 3.021e22 * (gamma1 * self.cox) ** 2

        self.phi = 2.0 * vtm0 * math.log(npeak / ni)
        self.sqrt_phi = math.sqrt(self.phi)
        self.xdep0 = math.sqrt(2.0 * EPSSI / (Q * npeak * 1e6)) * self.sqrt_phi
        self.litl = math.sqrt(3.0 * self.xj * self.tox)
        self.vbi = vtm0 * math.log(1e20 * npeak / (ni * ni))
        self.cdep0 = math.sqrt(Q * EPSSI * npeak * 1e6 / 2.0 / self.phi)

        if given("k1") or given("k2"):
            self.k1 = bin_("k1", 0.0) if given("k1") else 0.53
            self.k2 = bin_("k2", 0.0) if given("k2") else -0.0186
        else:
            if not given("vbx"):
                vbx = self.phi - 7.7348e-4 * npeak * xt * xt
            vbx = -abs(vbx)
            vbm = -abs(vbm)
            if not given("gamma1"):
                gamma1 = 5.753e-12 * math.sqrt(npeak) / self.cox
            if not given("gamma2"):
                gamma2 = 5.753e-12 * math.sqrt(nsub) / self.cox
            self.k2 = ((gamma1 - gamma2) * (math.sqrt(self.phi - vbx) - self.sqrt_phi)
                       / (2.0 * (math.sqrt(self.phi * (self.phi - vbm)) - self.phi) + vbm))
            self.k1 = gamma2 - 2.0 * self.k2 * math.sqrt(self.phi - vbm)

        if self.k2 < 0.0:
            self.vbsc = min(-3.0, max(-30.0, 0.9 * (self.phi - (0.5 * self.k1 / self.k2) ** 2)))
        else:
            self.vbsc = -30.0
        self.vbsc = min(self.vbsc, vbm)

        vth_model = bin_("vth0", 0.7 if sign > 0 else -0.7)
        if given("vfb"):
            self.vfb = bin_("vfb", 0.0)
        elif given("vth0"):
            self.vfb = sign * vth_model - self.phi - self.k1 * self.sqrt_phi
        else:
            self.vfb = -1.0
        self.vth0 = sign * vth_model if given("vth0") else self.vfb + self.phi + self.k1 * self.sqrt_phi
        self.k1ox = self.k1 * self.tox / toxm
        self.k2ox = self.k2 * self.tox / toxm

        lt0 = math.sqrt(EPSSI / EPSOX * self.tox * self.xdep0)
        e = math.exp(-0.5 * dsub * self.leff / lt0)
        self.theta0vb0 = e + 2.0 * e * e
        e = math.exp(-0.5 * drout * self.leff / lt0)
        self.theta_rout = bin_("pdibl1", 0.39) * (e + 2.0 * e * e) + bin_("pdibl2", 0.0086)

        # --- charges ---
        self.capmod = int(get("capmod", 3))
        self.xpart = get("xpart", 0.0)
        dlc, dwc = get("dlc", get("lint", 0.0)), get("dwc", get("wint", 0.0))
        tl, tw = length ** get("lln", 1.0), width ** get("lwn", 1.0)
        dlc_eff = dlc + get("llc", get("ll", 0.0)) / tl + get("lwc", get("lw", 0.0)) / tw + get("lwlc", get("lwl", 0.0)) / (tl * tw)
        tl, tw = length ** get("wln", 1.0), width ** get("wwn", 1.0)
        dwc_eff = dwc + get("wlc", get("wl", 0.0)) / tl + get("wwc", get("ww", 0.0)) / tw + get("wwlc", get("wwl", 0.0)) / (tl * tw)
        self.leff_cv = length + get("xl", 0.0) - 2.0 * dlc_eff
        self.weff_cv = width + get("xw", 0.0) - 2.0 * dwc_eff
        self.cox_wl = self.cox * self.leff_cv * self.weff_cv
        self.abulk_cv_factor = 1.0 + (bin_("clc", 0.1e-6) / self.leff_cv) ** bin_("cle", 0.6)
        self.vfbcv = bin_("vfbcv", -1.0)
        self.noff = bin_("noff", 1.0)
        self.voffcv = bin_("voffcv", 0.0)
        self.moin = bin_("moin", 15.0)
        self.acde = bin_("acde", 1.0) / (npeak / 2e16) ** 0.25
        self.ldeb = math.sqrt(EPSSI * vtm0 / (Q * npeak * 1e6)) / 3.0
        self.ckappa = bin_("ckappa", 0.6)
        # vfbzb: threshold voltage at zero bias, without phi and the body effect
        root = self.factor1 * math.sqrt(self.xdep0)
        vth_zero = (self.vth0
                    - p_dvt(bin_("dvt0w", 0.0), bin_("dvt1w", 5.3e6) * self.weff * self.leff / root, self.vbi - self.phi)
                    - p_dvt(bin_("dvt0", 2.2), bin_("dvt1", 0.53) * self.leff / root, self.vbi - self.phi)
                    + bin_("k3", 80.0) * self.tox * self.phi / (self.weff + bin_("w0", 2.5e-6))
                    + self.k1ox * (math.sqrt(1.0 + bin_("nlx", 1.74e-7) / self.leff) - 1.0) * self.sqrt_phi
                    + (bin_("kt1", -0.11) + bin_("kt1l", 0.0) / self.leff) * self.temp_ratio)
        self.vfbzb = vth_zero - self.phi - self.k1 * self.sqrt_phi
        # overlaps, per transistor
        cf = bin_("cf", 2.0 * EPSOX / math.pi * math.log(1.0 + 0.4e-6 / self.tox))
        self.cgsl, self.cgdl = bin_("cgsl", 0.0), bin_("cgdl", 0.0)

        def overlap(key, cgl):
            if key in raw:
                return raw[key]
            if "dlc" in raw and dlc > 0.0:
                return dlc * self.cox - cgl
            return 0.6 * get("xj", 0.15e-6) * self.cox

        self.cgso = (overlap("cgso", get("cgsl", 0.0)) + cf) * self.weff_cv
        self.cgdo = (overlap("cgdo", get("cgdl", 0.0)) + cf) * self.weff_cv
        self.cgbo = get("cgbo", 2.0 * dwc * self.cox) * self.leff_cv


def p_dvt(dvt0, x, v0):
    """Threshold shift dvt0 (e^-x/2 + 2 e^-x) v0 of short and narrow channels."""
    t = -0.5 * x
    e = math.exp(t) if t > -EXP_THRESHOLD else MIN_EXP
    return dvt0 * (e + 2.0 * e * e) * v0


def _keep_positive(t):
    """1 + t, bent so that it stays positive (the model's guard against bad parameters)."""
    return 1.0 + t if t >= -0.5 else (1.0 + 3.0 * t) / (3.0 + 8.0 * t)


def _short(t):
    e = math.exp(t) if t > -EXP_THRESHOLD else MIN_EXP
    return e * (1.0 + 2.0 * e)


def drain_current(params, kind, vgs, vds, vbs, temp=27.0, prepared=None):
    """Drain current and substrate current for vds >= 0 in the n-channel frame."""
    p = prepared or Prepared(params, kind, temp)
    ids, isub, _ = _channel(p, vgs, vds, vbs)
    return ids * p.mult, isub * p.mult


def _channel(p, vgs, vds, vbs):
    """ids, isub and the quantities the charges need, for one transistor."""
    vtm, cox, leff = p.vtm, p.cox, p.leff

    # effective body bias
    a = vbs - p.vbsc - 0.001
    vbseff = p.vbsc + 0.5 * (a + math.sqrt(a * a - 0.004 * p.vbsc))
    vbseff = max(vbseff, vbs)
    if vbseff > 0.0:
        phis = p.phi * p.phi / (p.phi + vbseff)
        sqrt_phis = p.sqrt_phi * p.phi / (p.phi + 0.5 * vbseff)
    else:
        phis = p.phi - vbseff
        sqrt_phis = math.sqrt(phis)
    xdep = p.xdep0 * sqrt_phis / p.sqrt_phi

    # threshold voltage
    v0 = p.vbi - p.phi
    lt1 = p.factor1 * math.sqrt(xdep) * _keep_positive(p.dvt2 * vbseff)
    ltw = p.factor1 * math.sqrt(xdep) * _keep_positive(p.dvt2w * vbseff)
    theta0 = _short(-0.5 * p.dvt1 * leff / lt1)
    dvth_short = p.dvt0 * theta0 * v0
    dvth_narrow = p.dvt0w * _short(-0.5 * p.dvt1w * p.weff * leff / ltw) * v0
    lateral = (p.k1ox * (math.sqrt(1.0 + p.nlx / leff) - 1.0) * p.sqrt_phi
               + (p.kt1 + p.kt1l / leff + p.kt2 * vbseff) * p.temp_ratio)
    eta = p.eta0 + p.etab * vbseff
    if eta < 1e-4:
        eta = (2e-4 - eta) / (3.0 - 2e4 * eta)
    vth = (p.vth0 - p.k1 * p.sqrt_phi + p.k1ox * sqrt_phis - p.k2ox * vbseff - dvth_short - dvth_narrow
           + (p.k3 + p.k3b * vbseff) * p.tox * p.phi / (p.weff + p.w0) + lateral - eta * p.theta0vb0 * vds)

    # subthreshold swing
    t = (p.nfactor * EPSSI / xdep + (p.cdsc + p.cdscb * vbseff + p.cdscd * vds) * theta0 + p.cit) / cox
    n = _keep_positive(t)

    # poly depletion
    vgs_eff = vgs
    if 1e18 < p.ngate < 1e25 and vgs > p.vfb + p.phi:
        t1 = 1e6 * Q * EPSSI * p.ngate / (cox * cox)
        t2 = t1 * (math.sqrt(1.0 + 2.0 * (vgs - p.vfb - p.phi) / t1) - 1.0)
        vpoly = 0.5 * t2 * t2 / t1
        t7 = 1.12 - vpoly - 0.05
        vgs_eff = vgs - (1.12 - 0.5 * (t7 + math.sqrt(t7 * t7 + 0.224)))
    vgst = vgs_eff - vth

    # effective gate drive
    two_nvt = 2.0 * n * vtm
    if vgst / two_nvt > EXP_THRESHOLD:
        vgsteff = vgst
    elif (2.0 * p.voff - vgst) / two_nvt > EXP_THRESHOLD:
        vgsteff = vtm * p.cdep0 / cox * math.exp((vgst - p.voff) / (n * vtm))
    else:
        vgsteff = (two_nvt * math.log(1.0 + math.exp(vgst / two_nvt))
                   / (1.0 + two_nvt * cox / (vtm * p.cdep0) * math.exp((2.0 * p.voff - vgst) / two_nvt)))

    # effective width, series resistance
    dphi = sqrt_phis - p.sqrt_phi
    weff = p.weff - 2.0 * (p.dwg * vgsteff + p.dwb * dphi)
    if weff < 2e-8:
        weff = 2e-8 * (4e-8 - weff) / (6e-8 - 2.0 * weff)
    t = p.prwg * vgsteff + p.prwb * dphi
    rds = p.rds0 * (1.0 + t) if t >= -0.9 else p.rds0 * (0.8 + t) / (17.0 + 20.0 * t)

    # bulk charge factor
    half = 0.5 * p.k1ox / sqrt_phis
    lfrac = leff / (leff + 2.0 * math.sqrt(p.xj * xdep))
    abulk0 = 1.0 + half * (p.a0 * lfrac + p.b0 / (p.weff + p.b1))
    abulk = abulk0 - half * p.ags * p.a0 * lfrac ** 3 * vgsteff
    abulk0, abulk = (a if a >= 0.1 else (0.2 - a) / (3.0 - 20.0 * a) for a in (abulk0, abulk))
    t = p.keta * vbseff
    keta_factor = 1.0 / (1.0 + t) if t >= -0.9 else (17.0 + 20.0 * t) / (0.8 + t)
    abulk *= keta_factor
    abulk0 *= keta_factor

    # mobility
    if p.mobmod == 1:
        field = (vgsteff + 2.0 * vth) / p.tox
        t = field * (p.ua + p.uc * vbseff + p.ub * field)
    elif p.mobmod == 2:
        field = vgsteff / p.tox
        t = field * (p.ua + p.uc * vbseff + p.ub * field)
    else:
        field = (vgsteff + 2.0 * vth) / p.tox
        t = field * (p.ua + p.ub * field) * (1.0 + p.uc * vbseff)
    denominator = 1.0 + t if t >= -0.8 else (0.6 + t) / (7.0 + 10.0 * t)
    ueff = p.u0temp / denominator

    # saturation voltage
    wvcox_rds = weff * p.vsattemp * cox * rds
    esat_l = 2.0 * p.vsattemp / ueff * leff
    if p.a1 == 0.0:
        lam = p.a2
    elif p.a1 > 0.0:
        c = 1.0 - p.a2
        b = c - p.a1 * vgsteff - 0.0001
        lam = p.a2 + c - 0.5 * (b + math.sqrt(b * b + 0.0004 * c))
    else:
        b = p.a2 + p.a1 * vgsteff - 0.0001
        lam = 0.5 * (b + math.sqrt(b * b + 0.0004 * p.a2))
    vg2 = vgsteff + 2.0 * vtm
    if rds == 0.0 and lam == 1.0:
        vdsat = esat_l * vg2 / (abulk * esat_l + vg2)
    else:
        aa = 2.0 * abulk * (abulk * wvcox_rds - 1.0 + 1.0 / lam)
        bb = vg2 * (2.0 / lam - 1.0) + abulk * esat_l + 3.0 * vg2 * abulk * wvcox_rds
        cc = vg2 * (esat_l + 2.0 * vg2 * wvcox_rds)
        vdsat = (bb - math.sqrt(bb * bb - 2.0 * aa * cc)) / aa

    # effective drain voltage
    b = vdsat - vds - p.delta
    vdseff = vdsat - 0.5 * (b + math.sqrt(b * b + 4.0 * p.delta * vdsat))
    if vds == 0.0:
        vdseff = 0.0
    vdseff = min(vdseff, vds)
    diff = vds - vdseff

    # output resistance
    vasat = ((esat_l + vdsat + 2.0 * wvcox_rds * vgsteff * (1.0 - 0.5 * abulk * vdsat / vg2))
             / (2.0 / lam - 1.0 + wvcox_rds * abulk))
    vaclm = MAX_EXP
    if p.pclm > 0.0 and diff > 1e-10:
        vaclm = leff * (abulk + vgsteff / esat_l) / (p.pclm * abulk * p.litl) * diff
    vadibl = MAX_EXP
    if p.theta_rout > 0.0:
        t8 = abulk * vdsat
        vadibl = (vg2 - vg2 * t8 / (vg2 + t8)) / p.theta_rout
        t = p.pdiblb * vbseff
        vadibl *= 1.0 / (1.0 + t) if t >= -0.9 else (17.0 + 20.0 * t) / (0.8 + t)
    t = p.pvag / esat_l * vgsteff
    gate_factor = 1.0 + t if t > -0.9 else (0.8 + t) / (17.0 + 20.0 * t)
    va = vasat + gate_factor * vaclm * vadibl / (vaclm + vadibl)
    vascbe = MAX_EXP
    if p.pscbe2 > 0.0:
        if diff > p.pscbe1 * p.litl / EXP_THRESHOLD:
            vascbe = leff * math.exp(p.pscbe1 * p.litl / diff) / p.pscbe2
        else:
            vascbe = MAX_EXP * leff / p.pscbe2

    # drain current
    beta = ueff * cox * weff / leff
    gche = beta * vgsteff * (1.0 - 0.5 * abulk * vdseff / vg2) / (1.0 + vdseff / esat_l)
    idl = gche * vdseff / (1.0 + gche * rds)
    idsa = idl * (1.0 + diff / va)
    ids = idsa * (1.0 + diff / vascbe)

    # substrate current
    alpha = p.alpha0 + p.alpha1 * leff
    if alpha <= 0.0 or p.beta0 <= 0.0:
        isub = 0.0
    elif diff > p.beta0 / EXP_THRESHOLD:
        isub = alpha / leff * diff * math.exp(-p.beta0 / diff) * idsa
    else:
        isub = alpha / leff * MIN_EXP * diff * idsa
    bias = dict(vbs=vbs, vbseff=vbseff, phis=phis, sqrt_phis=sqrt_phis, vth=vth, vgs_eff=vgs_eff, vgst=vgst,
                n=n, abulk0=abulk0)
    return ids, isub, bias


# ---------------------------------------------------------------- charges ---

def _partition_linear(xpart, c, vg, ad, qinv):
    """Source charge of capMod 1 to 3 below saturation (ad = Abulk * Vds)."""
    if xpart == 0.5:
        return -0.5 * qinv
    d = vg - 0.5 * ad + 1e-20
    if xpart > 0.5:
        return -c * (vg / 2.0 + ad / 4.0 - ad * ad / (24.0 * d))
    return -c * (vg ** 3 - 4.0 / 3.0 * vg * vg * ad + 2.0 / 3.0 * vg * ad * ad - 2.0 / 15.0 * ad ** 3) / (2.0 * d * d)


def _smooth_min(vdsat, vds, delta):
    """Vds below vdsat, vdsat above (the model's smoothing with delta)."""
    if vds == 0.0:
        return 0.0
    v4 = vdsat - vds - delta
    return vdsat - 0.5 * (v4 + math.sqrt(v4 * v4 + 4.0 * delta * vdsat))


def _qsub(p, c, vgs_eff, vfbeff, vbseff, vgsteff):
    """Accumulation and depletion charge of capMod 2 and 3 (oxide capacitance c)."""
    qacc = c * (vfbeff - p.vfbzb)
    x = vgs_eff - vfbeff - vbseff - vgsteff
    if p.k1ox == 0.0:
        qdep = 0.0
    elif x < 0.0:
        qdep = c * x
    else:
        qdep = c * p.k1ox * (math.sqrt(0.25 * p.k1ox ** 2 + x) - 0.5 * p.k1ox)
    return qacc, qdep


def _cap0(p, vds, b):
    """capMod 0: (qg, qd, qs, qb)."""
    c = p.cox_wl
    vb = b["vbs"] if b["vbseff"] < 0.0 else p.phi - b["phis"]
    vg = b["vgs_eff"]
    vgst = vg - (p.vfbcv + p.phi + p.k1ox * b["sqrt_phis"])
    above_fb = vg - vb - p.vfbcv
    if above_fb <= 0.0:
        return c * above_fb, 0.0, 0.0, -c * above_fb
    if vgst <= 0.0:
        qg = c * p.k1ox * (math.sqrt(0.25 * p.k1ox ** 2 + above_fb) - 0.5 * p.k1ox)
        return qg, 0.0, 0.0, -qg
    a = b["abulk0"] * p.abulk_cv_factor
    vdsat = vgst / a
    if vds >= vdsat:
        qg = c * (vg - p.vfbcv - p.phi - vdsat / 3.0)
        qinv = -2.0 / 3.0 * c * vgst
        share = 0.0 if p.xpart > 0.5 else (0.4 if p.xpart < 0.5 else 0.5)
        qd, qb = share * qinv, -qg - qinv
    else:
        u = 2.0 * vdsat - vds
        w = vds * vds / (3.0 * u)
        k = 0.25 * c * a
        qg = c * (vg - p.vfbcv - p.phi - 0.5 * vds + 0.5 * w)
        if p.xpart > 0.5:
            qd = k * (2.0 * vds - u - 3.0 * w)
            qb = -qg - qd - k * (w - u - 2.0 * vds)
        elif p.xpart < 0.5:
            qd = k * (vds - u - vds / (3.0 * u * u) * (8.0 * vdsat ** 2 - 6.0 * vdsat * vds + 1.2 * vds ** 2))
            qb = -qg + 2.0 * k * (u + w)
        else:
            qd = -k * (u + w)
            qb = -qg - 2.0 * qd
    return qg, qd, -(qg + qd + qb), qb


def _cap123(p, vds, b):
    """capMod 1, 2 and 3: (qg, qd, qs, qb)."""
    c = p.cox_wl
    vg_eff = b["vgs_eff"]
    vb = b["vbseff"] if b["vbseff"] < 0.0 else p.phi - b["phis"]
    nvt = p.vtm * p.noff * b["n"]
    x = (b["vgst"] - p.voffcv) / nvt
    if x > EXP_THRESHOLD:
        vgt = b["vgst"] - p.voffcv
    elif x < -EXP_THRESHOLD:
        vgt = nvt * math.log(1.0 + MIN_EXP)
    else:
        vgt = nvt * math.log1p(math.exp(x))
    a = b["abulk0"] * p.abulk_cv_factor

    if p.capmod == 1:
        y = vg_eff - vb - p.vfbzb - vgt
        qdep = c * y if y <= 0.0 else c * p.k1ox * (math.sqrt(0.25 * p.k1ox ** 2 + y) - 0.5 * p.k1ox)
        vdsat = vgt / a
        if vdsat < vds:
            qg_ch, qb_ch = c * (vgt - vdsat / 3.0), c * (vdsat - vgt) / 3.0
            qs = -c * vgt * {True: 2.0 / 3.0, False: 0.4}[p.xpart > 0.5] if p.xpart != 0.5 else -c * vgt / 3.0
        else:
            ad = a * vds
            r = ad * vds / (12.0 * (vgt - 0.5 * ad + 1e-20))
            qg_ch, qb_ch = c * (vgt - 0.5 * vds + r), c * (1.0 - a) * (0.5 * vds - r)
            qs = _partition_linear(p.xpart, c, vgt, ad, qg_ch + qb_ch)
        qg, qb = qg_ch + qdep, qb_ch - qdep
        return qg, -(qg + qb + qs), qs, qb

    def vfb_effective():
        v3 = p.vfbzb - vg_eff + vb - 0.02
        return p.vfbzb - 0.5 * (v3 + math.sqrt(v3 * v3 + 0.08 * abs(p.vfbzb)))

    if p.capmod == 2:
        qacc, qdep = _qsub(p, c, vg_eff, vfb_effective(), vb, vgt)
        vdse = _smooth_min(vgt / a, vds, 0.02)
        ad = a * vdse
        r = ad * vdse / (12.0 * (vgt - 0.5 * ad + 1e-20))
        qg_ch, qb_ch = c * (vgt - 0.5 * vdse + r), c * (1.0 - a) * (0.5 * vdse - r)
        qs = _partition_linear(p.xpart, c, vgt, ad, qg_ch + qb_ch)
        qg, qb = qg_ch + qacc + qdep, qb_ch - qacc - qdep
        return qg, -(qg + qb + qs), qs, qb

    # capMod 3: charge centroids
    def with_centroid(tcen):
        ccen = EPSSI / tcen
        return c * ccen / (p.cox + ccen)

    t = p.acde * (vg_eff - vb - p.vfbzb) / (1e8 * p.tox)
    tcen = p.ldeb * (MIN_EXP if t <= -EXP_THRESHOLD else MAX_EXP if t >= EXP_THRESHOLD else math.exp(t))
    link = 1e-3 * p.tox
    v3 = p.ldeb - tcen - link
    tcen = p.ldeb - 0.5 * (v3 + math.sqrt(v3 * v3 + 4.0 * link * p.ldeb))
    qacc, qdep = _qsub(p, with_centroid(tcen), vg_eff, vfb_effective(), vb, vgt)

    if p.k1ox <= 0.0:
        dphi = p.vtm * math.log(1.0 + (p.sqrt_phi + vgt) * vgt / (0.25 * p.moin * p.vtm))
    else:
        dphi = p.vtm * math.log(1.0 + (2.0 * p.k1ox * p.sqrt_phi + vgt) * vgt / (p.moin * p.vtm * p.k1ox ** 2))
    extra = 4.0 * (b["vth"] - p.vfbzb - p.phi)
    field = (vgt + (extra if extra >= 0.0 else 1e-20)) / (2e8 * p.tox)
    cinv = with_centroid(1.9e-9 / (1.0 + field ** 0.7))

    vg = vgt - dphi
    vdse = _smooth_min(vg / a, vds, 0.02)
    ad = a * vdse
    d12 = 12.0 * (vg - 0.5 * ad + 1e-20)
    qinv = cinv * (vg - 0.5 * ad + ad * ad / d12)
    qb_ch = cinv * (1.0 - a) * vdse * (0.5 - ad / d12)
    qs = _partition_linear(p.xpart, cinv, vg, ad, qinv)
    qg, qb = qinv - qb_ch + qacc + qdep, qb_ch - qacc - qdep
    return qg, -(qg + qb + qs), qs, qb


def _overlap(p, v, cgo, cgl):
    """Charge of a gate overlap at gate voltage v against the terminal below."""
    if p.capmod == 0:
        return cgo * v
    cl = p.weff_cv * cgl
    if p.capmod == 1:
        if v >= 0.0:
            return (cgo + cl) * v
        return cgo * v - 0.5 * cl * p.ckappa * (math.sqrt(1.0 - 4.0 * v / p.ckappa) - 1.0)
    t0 = v + 0.02
    t2 = 0.5 * (t0 - math.sqrt(t0 * t0 + 0.08))
    return (cgo + cl) * v - cl * (t2 + 0.5 * p.ckappa * (math.sqrt(1.0 - 4.0 * t2 / p.ckappa) - 1.0))


def terminal_charges(params, kind, vd, vg, vs, vb, temp=27.0, prepared=None):
    """Charges of drain, gate, source and bulk at the given terminal voltages:
    intrinsic charges and overlaps, without the junctions."""
    p = prepared or Prepared(params, kind, temp)
    sign = 1.0 if kind == "nmos" else -1.0
    vd, vg, vs, vb = (sign * v for v in (vd, vg, vs, vb))
    reverse = vd < vs
    dr, so = (vs, vd) if reverse else (vd, vs)  # the terminals acting as drain and source
    qg = qd = qs = qb = 0.0
    if p.xpart >= 0.0:
        _, _, bias = _channel(p, vg - so, dr - so, vb - so)
        qg, qd, qs, qb = (_cap0 if p.capmod == 0 else _cap123)(p, dr - so, bias)
        if reverse:
            qd, qs = qs, qd
    ogs = _overlap(p, vg - vs, p.cgso, p.cgsl)
    ogd = _overlap(p, vg - vd, p.cgdo, p.cgdl)
    ogb = p.cgbo * (vg - vb)
    m = p.mult
    return tuple(sign * m * q for q in (qd - ogd, qg + ogs + ogd + ogb, qs - ogs, qb - ogb))
