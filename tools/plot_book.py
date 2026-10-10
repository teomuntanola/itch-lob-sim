#!/usr/bin/env python3
"""Dark three-panel market-microstructure dashboard from itch_sim CSV exports.

  itch_sim --symbol AAPL --from 09:30:00 --to 09:32:00 --depth-ms 100 --levels 20 \
           --bbo bbo.csv --depth depth.csv --trades trades.csv  <itch file>
  python3 tools/plot_book.py --depth depth.csv --bbo bbo.csv --trades trades.csv \
           --title "AAPL | 30 Jan 2019 | NASDAQ ITCH 5.0" --out dashboard.png --gif dashboard.gif

Panels (shared market-time axis):
  1. resting liquidity heatmap (top levels per side), best bid / best ask, micro-price, executions
  2. quoted spread in cents
  3. order flow imbalance (Cont-Kukanov-Stoikov) summed over a rolling window

Needs python3, numpy, matplotlib and Pillow (Pillow ships with matplotlib).
"""
import argparse
import csv
import math
import sys
import warnings

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, LogNorm  # noqa: E402
from matplotlib.gridspec import GridSpec  # noqa: E402

BG = "#0a0d12"
PANEL = "#11151c"
GRID = "#1e2430"
TXT = "#e1e6ed"
MUTED = "#8b98a9"
AXIS = "#2e3646"
BID = "#00f076"
ASK = "#ff3355"
MICRO = "#00b8ff"
SPREAD = "#b07cff"
TICK = 0.01
# Blue-to-white scale so the green/red price lines and markers stay readable on top.
ICE = LinearSegmentedColormap.from_list("ice", ["#0a0d12", "#0f2a4d", "#1b5fa8", "#5fb4f5", "#ffffff"])


# ----------------------------------------------------------------------------- input
def fnum(text):
    return float(text) if text != "" else float("nan")


def read_depth(path):
    ts, px, qty = [], [], []
    with open(path, newline="") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            ts.append(int(row[0]))
            px.append(float(row[3]))
            qty.append(int(row[4]))
    return np.array(ts, dtype=np.int64), np.array(px), np.array(qty, dtype=np.float64)


def read_bbo(path):
    ts, bid, bq, ask, aq = [], [], [], [], []
    with open(path, newline="") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            ts.append(int(row[0]))
            bid.append(fnum(row[1]))
            bq.append(fnum(row[2]))
            ask.append(fnum(row[3]))
            aq.append(fnum(row[4]))
    return (np.array(ts, dtype=np.int64), np.array(bid), np.array(bq), np.array(ask), np.array(aq))


def read_trades(path):
    ts, px, sh, side = [], [], [], []
    with open(path, newline="") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            ts.append(int(row[0]))
            px.append(float(row[1]))
            sh.append(int(row[2]))
            side.append(row[3])
    return (np.array(ts, dtype=np.int64), np.array(px), np.array(sh, dtype=np.float64), np.array(side))


# ----------------------------------------------------------------------------- maths
def micro_price(bid, bq, ask, aq):
    """Size-weighted mid: leans toward the side with less resting size."""
    tot = bq + aq
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(tot > 0, (bid * aq + ask * bq) / tot, np.nan)


def order_flow_imbalance(bid, bq, ask, aq):
    """Per-update OFI e_n (Cont, Kukanov, Stoikov 2014); NaN where a side is empty."""
    e = np.full(len(bid), np.nan)
    if len(bid) < 2:
        return e
    pb0, qb0, pa0, qa0 = bid[:-1], bq[:-1], ask[:-1], aq[:-1]
    pb1, qb1, pa1, qa1 = bid[1:], bq[1:], ask[1:], aq[1:]
    e_bid = np.where(pb1 > pb0, qb1, np.where(pb1 == pb0, qb1 - qb0, -qb0))
    e_ask = np.where(pa1 < pa0, qa1, np.where(pa1 == pa0, qa1 - qa0, -qa0))
    e[1:] = e_bid - e_ask
    return e


def build_matrix(d_ts, d_px, d_qty, t0_ns, dt_ns, lo, n_prices):
    """Rows = price ticks, columns = time bins; forward-filled between snapshots."""
    nbins = int((d_ts.max() - t0_ns) // dt_ns) + 1
    mat = np.zeros((n_prices, nbins))
    has = np.zeros(nbins, dtype=bool)
    cols = ((d_ts - t0_ns) // dt_ns).astype(np.int64)
    rows = np.rint((d_px - lo) / TICK).astype(np.int64)
    ok = (rows >= 0) & (rows < n_prices) & (cols >= 0)
    mat[rows[ok], cols[ok]] += d_qty[ok]
    has[np.unique(cols[cols >= 0])] = True
    last = None
    for j in range(nbins):
        if has[j]:
            last = j
        elif last is not None:
            mat[:, j] = mat[:, last]
    return mat


# ----------------------------------------------------------------------------- drawing
def clock(sec_since_midnight, with_ms=False):
    s = int(sec_since_midnight)
    base = "%02d:%02d:%02d" % (s // 3600, (s // 60) % 60, s % 60)
    if with_ms:
        base += ".%03d" % int((sec_since_midnight - s) * 1000)
    return base


def nice_ticks(lo, hi):
    span = max(hi - lo, 1e-9)
    step = next((c for c in (1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600) if span / c <= 8), 3600)
    first = math.ceil(lo / step) * step
    return np.arange(first, hi + 1e-9, step)


def style(ax):
    ax.set_facecolor(PANEL)
    ax.grid(True, color=GRID, linestyle="--", linewidth=0.5, alpha=0.6)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(AXIS)
    ax.tick_params(colors=MUTED, labelsize=8)


def view(arr_t, lo, hi):
    """Index range of a sorted time array covering [lo, hi], plus one sample before for step continuity."""
    i0, i1 = np.searchsorted(arr_t, [lo, hi])
    return max(i0 - 1, 0), i1


def render(axes, D, t_lo, t_hi, ylim, big=True, now_label=None):
    ax1, ax2, ax3 = axes
    for ax in axes:
        ax.clear()
        style(ax)
    fs = 7.5 if big else 6.5

    # ---- panel 1: heatmap + BBO + micro-price + executions
    im = None
    c0 = max(int(t_lo * 1e9 // D.dt), 0)
    c1 = min(int(math.ceil(t_hi * 1e9 / D.dt)) + 1, D.nbins)
    r0 = max(int(round((ylim[0] - D.lo) / TICK)), 0)
    r1 = min(int(round((ylim[1] - D.lo) / TICK)) + 1, D.n_prices)
    if D.heat and c1 > c0 and r1 > r0:
        im = ax1.imshow(D.masked[r0:r1, c0:c1], origin="lower", aspect="auto", interpolation="nearest",
                        cmap=D.cmap, norm=D.norm,
                        extent=[c0 * D.dt / 1e9, c1 * D.dt / 1e9, D.lo + r0 * TICK - TICK / 2,
                                D.lo + (r1 - 1) * TICK + TICK / 2])
    i0, i1 = view(D.rb, t_lo, t_hi)
    sl = slice(i0, i1)
    ax1.step(D.rb[sl], D.ask[sl], where="post", color=ASK, lw=1.0, label="best ask", zorder=5)
    ax1.step(D.rb[sl], D.bid[sl], where="post", color=BID, lw=1.0, label="best bid", zorder=5)
    ax1.step(D.rb[sl], D.mp[sl], where="post", color=MICRO, lw=0.8, ls=":", label="micro-price", zorder=7)
    if D.trades is not None:
        tt, tp, ts_, tside = D.trades
        j0, j1 = np.searchsorted(tt, [t_lo, t_hi])
        tt, tp, ts_, tside = tt[j0:j1], tp[j0:j1], ts_[j0:j1], tside[j0:j1]
        size = (11 if big else 14) + (5 if big else 7) * np.sqrt(ts_ / 100.0)
        buy = tside == "ask"      # a resting ask was executed -> buyer initiated
        ax1.scatter(tt[buy], tp[buy], s=size[buy], c=BID, marker="^", edgecolors="none",
                    alpha=0.85, label="buy-initiated trade", zorder=6)
        ax1.scatter(tt[~buy], tp[~buy], s=size[~buy], c=ASK, marker="v", edgecolors="none",
                    alpha=0.85, label="sell-initiated trade", zorder=6)
    ax1.set_ylim(*ylim)
    ax1.set_ylabel("price ($)", color=TXT, fontsize=9, fontweight="bold")
    ax1.yaxis.set_major_formatter(matplotlib.ticker.FormatStrFormatter("%.2f"))
    ax1.legend(loc="lower left", bbox_to_anchor=(0.0, 1.015), ncol=5, facecolor=BG, edgecolor="none",
               labelcolor=TXT, fontsize=fs, framealpha=0.0, borderaxespad=0.0, columnspacing=1.2,
               handletextpad=0.4, markerscale=1.8)
    if now_label:
        ax1.text(0.995, 0.97, now_label, transform=ax1.transAxes, ha="right", va="top", color=TXT,
                 fontsize=9, family="monospace", zorder=10)

    # ---- panel 2: spread in cents (fixed-rate samples plus a rolling median)
    s0, s1 = view(D.sp_t, t_lo, t_hi)
    ax2.step(D.sp_t[s0:s1], D.sp_raw[s0:s1], where="post", color=SPREAD, lw=0.5, alpha=0.45,
             label="%d ms samples" % int(D.dt / 1e6))
    ax2.step(D.sp_t[s0:s1], D.sp_med[s0:s1], where="post", color=SPREAD, lw=1.5,
             label="%g s median" % D.sp_win)
    ax2.fill_between(D.sp_t[s0:s1], 0, D.sp_med[s0:s1], step="post", color=SPREAD, alpha=0.18)
    ax2.set_ylim(0, D.spread_top)
    ax2.set_ylabel("spread (cents)", color=TXT, fontsize=8.5, fontweight="bold")
    ax2.legend(loc="upper right", facecolor=PANEL, edgecolor=AXIS, labelcolor=TXT, fontsize=fs - 0.5,
               framealpha=0.9, ncol=2)

    # ---- panel 3: rolling OFI
    k0, k1 = view(D.ofi_t, t_lo, t_hi)
    x, y = D.ofi_t[k0:k1], D.ofi_roll[k0:k1]
    ax3.axhline(0, color="#555b66", lw=0.6)
    ax3.fill_between(x, 0, y, where=y >= 0, step="post", color=BID, alpha=0.35)
    ax3.fill_between(x, 0, y, where=y < 0, step="post", color=ASK, alpha=0.35)
    ax3.step(x, y, where="post", color=TXT, lw=0.5, alpha=0.8)
    ax3.set_ylim(-D.ofi_top, D.ofi_top)
    ax3.set_ylabel("OFI (%gs, shares)" % D.ofi_win, color=TXT, fontsize=8.5, fontweight="bold")

    # ---- shared time axis (labels on the bottom panel only)
    ticks = nice_ticks(D.sec0 + t_lo, D.sec0 + t_hi) - D.sec0
    for ax in axes:
        ax.set_xlim(t_lo, t_hi)
        ax.set_xticks(ticks)
    ax1.tick_params(labelbottom=False)
    ax2.tick_params(labelbottom=False)
    ax3.set_xticklabels([clock(D.sec0 + t) for t in ticks], fontsize=8)
    ax3.set_xlabel("market time (ET)", color=TXT, fontsize=8.5, fontweight="bold")
    return im


class Data:
    pass


def prepare(args):
    d_ts, d_px, d_qty = read_depth(args.depth)
    b_ts, b_bid, b_bq, b_ask, b_aq = read_bbo(args.bbo)
    if len(d_ts) == 0 or len(b_ts) == 0:
        sys.exit("empty depth or bbo CSV: check --from/--to and that the symbol was found")
    D = Data()
    uniq = np.unique(d_ts)
    gaps = np.diff(uniq)
    med_ms = float(np.median(gaps)) / 1e6 if len(gaps) else 100.0
    nominal = [v for v in (10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000) if v <= med_ms]
    D.dt = int((nominal[-1] if nominal else 10) * 1e6)
    t0_ns = int(min(uniq[0], b_ts[0])) // 10**9 * 10**9   # whole second, so the first tick is a round clock time
    D.sec0 = t0_ns / 1e9
    quoted = np.concatenate([b_bid[~np.isnan(b_bid)], b_ask[~np.isnan(b_ask)]])
    D.lo = math.floor((np.percentile(quoted, 0.5) - args.price_pad) / TICK) * TICK
    D.hi = math.ceil((np.percentile(quoted, 99.5) + args.price_pad) / TICK) * TICK
    D.n_prices = int(round((D.hi - D.lo) / TICK)) + 1
    mat = build_matrix(d_ts, d_px, d_qty, t0_ns, D.dt, D.lo, D.n_prices)
    D.nbins = mat.shape[1]
    D.t_span = max(D.nbins * D.dt / 1e9, (b_ts[-1] - t0_ns) / 1e9)
    D.heat = not args.no_heatmap
    vmax = max(float(np.percentile(mat[mat > 0], 99.5)), 200.0) if (mat > 0).any() else 1000.0
    D.norm = LogNorm(vmin=100.0, vmax=vmax)
    D.cmap = ICE.copy()
    D.cmap.set_bad(PANEL)
    D.masked = np.ma.masked_less_equal(mat, 0)

    D.rb = (b_ts - t0_ns) / 1e9
    D.bid, D.ask = b_bid, b_ask
    D.mp = micro_price(b_bid, b_bq, b_ask, b_aq)
    D.spread_c = (b_ask - b_bid) * 100.0
    nb = int(math.ceil(D.t_span * 1e9 / D.dt)) + 1
    D.sp_t = np.arange(nb) * D.dt / 1e9
    at = np.searchsorted(D.rb, D.sp_t, side="right") - 1          # last BBO update at or before each sample
    D.sp_raw = np.where(at >= 0, D.spread_c[np.clip(at, 0, None)], np.nan)
    D.sp_win = args.spread_window_sec
    kk = max(int(round(args.spread_window_sec * 1e9 / D.dt)), 1)
    padded = np.concatenate([np.full(kk - 1, np.nan), D.sp_raw])
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", category=RuntimeWarning)
        D.sp_med = np.nanmedian(np.lib.stride_tricks.sliding_window_view(padded, kk), axis=1)
    sp = D.sp_raw[~np.isnan(D.sp_raw)]
    D.spread_top = max(3.0, 1.15 * float(np.percentile(sp, 99))) if len(sp) else 5.0

    # OFI: per-update values -> sum per snapshot bin -> rolling sum over the window
    e = np.nan_to_num(order_flow_imbalance(b_bid, b_bq, b_ask, b_aq), nan=0.0)
    binned = np.zeros(nb)
    np.add.at(binned, np.clip(((b_ts - t0_ns) // D.dt).astype(np.int64), 0, nb - 1), e)
    k = max(int(round(args.ofi_window_sec * 1e9 / D.dt)), 1)
    cs = np.concatenate([[0.0], np.cumsum(binned)])
    roll = cs[1:] - np.concatenate([np.zeros(k), cs[1:-k]]) if nb > k else cs[1:]
    D.ofi_t = (np.arange(nb) + 1) * D.dt / 1e9
    D.ofi_roll = roll
    D.ofi_win = args.ofi_window_sec
    D.ofi_top = max(100.0, 1.2 * float(np.percentile(np.abs(roll), 99)))

    D.trades = None
    if args.trades:
        t = read_trades(args.trades)
        if len(t[0]):
            D.trades = ((t[0] - t0_ns) / 1e9, t[1], t[2], t[3])
    D.quoted = quoted
    return D


def make_figure(figsize, dpi, title, badge, footer, big):
    fig = plt.figure(figsize=figsize, dpi=dpi, facecolor=BG)
    gs = GridSpec(3, 1, height_ratios=[3.4, 1.0, 1.3], hspace=0.10,
                  left=0.075 if big else 0.10, right=0.925 if big else 0.97,
                  top=0.895 if big else 0.865, bottom=0.085 if big else 0.12)
    axes = [fig.add_subplot(gs[i]) for i in range(3)]
    fig.suptitle(title, color="white", fontsize=13 if big else 10, fontweight="bold", x=0.075 if big else 0.10,
                 y=0.975, ha="left")
    if badge:
        fig.text(0.925 if big else 0.97, 0.975, badge, color="#00e5ff", fontsize=8 if big else 6.5,
                 fontweight="bold", family="monospace", ha="right", va="top")
    fig.text(0.01, 0.008, footer, color="#6b7685", fontsize=7 if big else 5.5)
    return fig, axes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--depth", required=True)
    ap.add_argument("--bbo", required=True)
    ap.add_argument("--trades")
    ap.add_argument("--out", default="dashboard.png")
    ap.add_argument("--title", default="AAPL | NASDAQ ITCH 5.0 replay")
    ap.add_argument("--badge", default="", help="short text, top right (e.g. measured latency with hardware)")
    ap.add_argument("--footer", default="itch-lob-sim | github.com/teomuntanola/itch-lob-sim")
    ap.add_argument("--price-pad", type=float, default=0.15, help="USD margin around the quoted range")
    ap.add_argument("--ofi-window-sec", type=float, default=1.0)
    ap.add_argument("--spread-window-sec", type=float, default=1.0)
    ap.add_argument("--no-heatmap", action="store_true", help="draw only the price lines and trades in panel 1")
    ap.add_argument("--gif", help="also write a scrolling animation to this path")
    ap.add_argument("--gif-start", type=float, default=0.0, help="seconds after the first sample")
    ap.add_argument("--gif-seconds", type=float, default=30.0, help="market seconds covered by the GIF, including the first window")
    ap.add_argument("--window-sec", type=float, default=10.0, help="visible width of the GIF")
    ap.add_argument("--step-ms", type=float, default=200.0, help="market ms advanced per GIF frame")
    ap.add_argument("--fps", type=int, default=12)
    args = ap.parse_args()

    D = prepare(args)

    # ---------------------------------------------------------------- static PNG
    fig, axes = make_figure((13, 8), 150, args.title, args.badge, args.footer, big=True)
    im = render(axes, D, 0.0, D.t_span, (D.lo, D.hi), big=True)
    if im is not None:
        cax = fig.add_axes([0.935, 0.50, 0.010, 0.38])
        cb = fig.colorbar(im, cax=cax)
        cb.set_label("resting shares / level", color=TXT, fontsize=8)
        cb.ax.tick_params(colors=MUTED, labelsize=7)
    fig.savefig(args.out, facecolor=BG)
    plt.close(fig)
    print("wrote", args.out)

    # ---------------------------------------------------------------- scrolling GIF
    if args.gif:
        from PIL import Image

        g0 = args.gif_start
        g1 = min(args.gif_start + args.gif_seconds, D.t_span)
        if g1 - g0 < 1.0:
            sys.exit("GIF range is shorter than 1 s: check --gif-start/--gif-seconds against the exported window")
        step = args.step_ms / 1000.0
        W = args.window_sec
        first = g0 + W                    # the first frame (also the preview) already shows a full window
        n_frames = int((g1 - first) / step)
        if n_frames < 5:
            sys.exit("--gif-seconds must be clearly longer than --window-sec (the first window is the opening frame)")
        sel = (D.rb >= g0 - W) & (D.rb <= g1)
        q = np.concatenate([D.bid[sel][~np.isnan(D.bid[sel])], D.ask[sel][~np.isnan(D.ask[sel])]])
        if len(q) == 0:
            q = D.quoted
        ylim = (math.floor((q.min() - args.price_pad) / TICK) * TICK, math.ceil((q.max() + args.price_pad) / TICK) * TICK)

        fig, axes = make_figure((9, 5.6), 80, args.title, args.badge, args.footer, big=False)
        frames = []
        for k in range(n_frames):
            now = first + k * step
            render(axes, D, now - W, now, ylim, big=False, now_label=clock(D.sec0 + now, with_ms=True))
            fig.canvas.draw()
            rgba = np.asarray(fig.canvas.buffer_rgba())
            frames.append(Image.fromarray(rgba[:, :, :3]).convert("P", palette=Image.ADAPTIVE, colors=256))
        plt.close(fig)
        durations = [int(1000 / args.fps)] * n_frames
        durations[-1] = 1500                # hold the last frame before the loop restarts
        frames[0].save(args.gif, save_all=True, append_images=frames[1:], duration=durations, loop=0)
        print("wrote", args.gif, "(%d frames, %.1f s playback)" % (n_frames, n_frames / args.fps))


if __name__ == "__main__":
    main()
